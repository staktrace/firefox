/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of Chromium's
// PartitionAlloc, configured as Chromium configures it for malloc on macOS
// (USE_PARTITION_ALLOC_AS_MALLOC, no BackupRefPtr) but without its allocator
// shim: Zone.c and this file play the shim's role, following
// shim/allocator_shim_default_dispatch_to_partition_alloc.cc.
//
// - The main root is created on first use with the thread cache disabled, and
//   gets its thread cache once the process is up, as in Chromium.
// - Each moz arena is its own PartitionRoot without a thread cache, like
//   Blink's extra partitions. Roots cannot be destroyed while allocations may
//   remain, so disposed arenas are purged and their root reused.
// - PartitionAlloc has no timers; the embedder runs MemoryReclaimer and the
//   thread cache purge periodically. Here Firefox's deferred purge
//   (moz_may_purge_now, driven by TaskController when idle) does that.

#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include <new>

#include "AltCommon.h"

#include "partition_alloc/memory_reclaimer.h"
#include "partition_alloc/partition_address_space.h"
#include "partition_alloc/partition_alloc.h"
#include "partition_alloc/partition_root.h"
#include "partition_alloc/partition_stats.h"
#include "partition_alloc/internal/partition_root_internal.h"
#include "partition_alloc/internal/reservation_offset_table_internal.h"
#include "partition_alloc/reservation_offset_table.h"
#include "partition_alloc/thread_cache.h"

using namespace mozilla::alt;
namespace pa = partition_alloc;

namespace {

constexpr auto kAllocFlags = pa::AllocFlags::kReturnNull;
constexpr auto kZeroAllocFlags =
    pa::AllocFlags::kReturnNull | pa::AllocFlags::kZeroFill;

// Delays reuse of freed slots as the UAF mitigation. Zapping is left to the
// glue so freed memory reads 0xe5.
pa::internal::SchedulerLoopQuarantineConfig QuarantineConfig() {
  pa::internal::SchedulerLoopQuarantineConfig config;
  config.branch_capacity_in_bytes = 4 * 1024 * 1024;
  config.enable_quarantine = true;
  config.zap_max = 0;
  return config;
}

std::atomic<pa::PartitionRoot*> gMainRoot{nullptr};
alignas(pa::PartitionRoot) char gMainRootStorage[sizeof(pa::PartitionRoot)];
std::atomic_flag gMainRootInitLock = ATOMIC_FLAG_INIT;
std::atomic<pthread_t> gMainRootInitThread{nullptr};

[[noreturn]] void OnOutOfMemory(size_t) { MOZ_CRASH("PartitionAlloc OOM"); }

// Creating the main root reserves PartitionAlloc's address space, which on
// macOS queries code signing through CoreFoundation and so allocates. Those
// nested allocations are served by libSystem's own zone; free() forwards them
// back there like any other pointer PartitionAlloc does not own.
malloc_zone_t* SystemZone() {
  static malloc_zone_t* sZone = nullptr;
  if (!sZone) {
    vm_address_t* zones = nullptr;
    unsigned count = 0;
    if (malloc_get_all_zones(0, nullptr, &zones, &count) == KERN_SUCCESS) {
      for (unsigned i = 0; i < count; i++) {
        auto* zone = reinterpret_cast<malloc_zone_t*>(zones[i]);
        if (zone->zone_name && !strcmp(zone->zone_name, "DefaultMallocZone")) {
          sZone = zone;
          break;
        }
      }
    }
    MOZ_RELEASE_ASSERT(sZone);
  }
  return sZone;
}

// Returns null when called from within the main root's own creation.
MOZ_NEVER_INLINE pa::PartitionRoot* InitMainRoot() {
  if (gMainRootInitThread.load(std::memory_order_relaxed) == pthread_self()) {
    return nullptr;
  }
  while (gMainRootInitLock.test_and_set(std::memory_order_acquire)) {
  }
  pa::PartitionRoot* root = gMainRoot.load(std::memory_order_relaxed);
  if (!root) {
    gMainRootInitThread.store(pthread_self(), std::memory_order_relaxed);
    pa::PartitionAllocGlobalInit(OnOutOfMemory);
    pa::PartitionOptions opts;
    opts.thread_cache = pa::PartitionOptions::kDisabled;
    opts.backup_ref_ptr = pa::PartitionOptions::kDisabled;
    // The index the main root's thread cache uses once enabled, as set by
    // Chromium's ConfigurePartitions().
    opts.thread_cache_index = 0;
    opts.scheduler_loop_quarantine_global_config = QuarantineConfig();
    opts.scheduler_loop_quarantine_thread_local_config = QuarantineConfig();
    root = new (gMainRootStorage) pa::PartitionRoot(opts);
    gMainRoot.store(root, std::memory_order_release);
    gMainRootInitThread.store(nullptr, std::memory_order_relaxed);
  }
  gMainRootInitLock.clear(std::memory_order_release);
  return root;
}

MOZ_ALWAYS_INLINE pa::PartitionRoot* MainRootOrNull() {
  pa::PartitionRoot* root = gMainRoot.load(std::memory_order_acquire);
  return MOZ_LIKELY(root) ? root : InitMainRoot();
}

// For callers that never run during the main root's creation.
pa::PartitionRoot* MainRoot() {
  pa::PartitionRoot* root = MainRootOrNull();
  MOZ_RELEASE_ASSERT(root);
  return root;
}

inline bool IsOwned(const void* aPtr) {
  return pa::IsManagedByPartitionAlloc(reinterpret_cast<uintptr_t>(aPtr));
}

// Chromium's GetSizeEstimate: addresses in the pools whose super page has been
// released report 1, so that the zone still claims them without reading
// decommitted metadata.
size_t OwnedSize(const void* aPtr) {
  uintptr_t address = reinterpret_cast<uintptr_t>(aPtr);
  if (!pa::IsManagedByPartitionAlloc(address)) {
    return 0;
  }
  if (!pa::internal::ReservationOffsetTable::Get(address)
           .IsManagedByNormalBucketsOrDirectMap(address)) {
    return 1;
  }
  size_t size = pa::PartitionRoot::GetExternalUsableSize(aPtr);
  return size ? size : 1;
}

constexpr size_t kMaxArenas = 1024;

alignas(pa::PartitionAllocator) char gArenaStorage[kMaxArenas][sizeof(
    pa::PartitionAllocator)];
pa::PartitionRoot* gArenaRoots[kMaxArenas];
pthread_mutex_t gArenaLock = PTHREAD_MUTEX_INITIALIZER;
size_t gArenaCount = 1;  // Id 0 is the main root.
arena_id_t gFreeArenas[kMaxArenas];
size_t gFreeArenaCount = 0;

inline pa::PartitionRoot* RootForArena(arena_id_t aId) {
  return aId && aId < kMaxArenas && gArenaRoots[aId] ? gArenaRoots[aId]
                                                     : MainRoot();
}

template <typename F>
void ForEachRoot(F aFunc) {
  aFunc(MainRoot());
  pthread_mutex_lock(&gArenaLock);
  size_t count = gArenaCount;
  pthread_mutex_unlock(&gArenaLock);
  for (size_t i = 1; i < count; i++) {
    if (pa::PartitionRoot* root = gArenaRoots[i]) {
      aFunc(root);
    }
  }
}

constexpr int kPurgeFlags = pa::PurgeFlags::kDecommitEmptySlotSpans |
                            pa::PurgeFlags::kDiscardUnusedSystemPages;

std::atomic<bool> gDeferredPurge{false};
std::atomic<int64_t> gNextReclaimMicros{0};
std::atomic<int64_t> gNextThreadCachePurgeMicros{0};

int64_t NowMicros() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

pid_t gForkPid = -1;

}  // namespace

// Gives the main root its thread cache once the process is up, the way
// Chromium does after its early allocations, and registers it with the
// memory reclaimer (which allocates, so it cannot happen on first use).
__attribute__((constructor)) static void FinishMainRootSetup() {
  pa::PartitionRoot* root = MainRoot();
  root->EnableThreadCacheIfSupported();
  pa::MemoryReclaimer::Instance()->RegisterPartition(root);
}

MOZ_MEMORY_API void* malloc_impl(size_t aSize) {
  pa::PartitionRoot* root = MainRootOrNull();
  if (MOZ_UNLIKELY(!root)) {
    return malloc_zone_malloc(SystemZone(), aSize);
  }
  return root->Alloc<kAllocFlags>(aSize);
}

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  size_t total;
  if (__builtin_mul_overflow(aNum, aSize, &total)) {
    return nullptr;
  }
  pa::PartitionRoot* root = MainRootOrNull();
  if (MOZ_UNLIKELY(!root)) {
    return malloc_zone_calloc(SystemZone(), aNum, aSize);
  }
  return root->Alloc<kZeroAllocFlags>(total);
}

static inline void RawFree(void* aPtr) {
  pa::PartitionRoot::FreeInUnknownRoot<pa::FreeFlags::kNoHooks |
                                       pa::FreeFlags::kSchedulerLoopQuarantine>(
      aPtr);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    PoisonFreedKeepingLinks(aPtr, OwnedSize(aPtr));
    RawFree(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

static void* ReallocIn(pa::PartitionRoot* aRoot, void* aPtr, size_t aSize) {
  auto alloc = [aRoot](size_t aN) { return aRoot->Alloc<kAllocFlags>(aN); };
  if (!aPtr) {
    return alloc(aSize);
  }
  if (!IsOwned(aPtr)) {
    return ForeignRealloc(aPtr, aSize, alloc);
  }
  return PoisoningRealloc(aPtr, aSize, OwnedSize, alloc, RawFree,
                          [aRoot](void* aP, size_t aN) {
                            return aRoot->Realloc<kAllocFlags>(aP, aN, "");
                          });
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  pa::PartitionRoot* root = MainRootOrNull();
  if (MOZ_UNLIKELY(!root)) {
    return malloc_zone_realloc(SystemZone(), aPtr, aSize);
  }
  return ReallocIn(root, aPtr, aSize);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  pa::PartitionRoot* root = MainRootOrNull();
  if (MOZ_UNLIKELY(!root)) {
    return malloc_zone_memalign(SystemZone(), aAlignment, aSize);
  }
  if (aAlignment <= pa::internal::kAlignment) {
    return root->Alloc<kAllocFlags>(aSize);
  }
  return root->AlignedAlloc<kAllocFlags>(aAlignment, aSize);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return OwnedSize(aPtr);
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  pa::PartitionRoot* root = MainRootOrNull();
  return root ? root->AllocationCapacityFromRequestedSize(aSize) : aSize;
}

MOZ_JEMALLOC_API arena_id_t moz_create_arena_with_params(arena_params_t*) {
  pthread_mutex_lock(&gArenaLock);
  arena_id_t id = 0;
  if (gFreeArenaCount) {
    id = gFreeArenas[--gFreeArenaCount];
    pthread_mutex_unlock(&gArenaLock);
    return id;
  }
  if (gArenaCount < kMaxArenas) {
    id = gArenaCount++;
  }
  pthread_mutex_unlock(&gArenaLock);
  if (!id) {
    return 0;
  }
  MainRoot();
  pa::PartitionOptions opts;
  opts.thread_cache = pa::PartitionOptions::kDisabled;
  opts.backup_ref_ptr = pa::PartitionOptions::kDisabled;
  opts.scheduler_loop_quarantine_global_config = QuarantineConfig();
  auto* allocator = new (gArenaStorage[id]) pa::PartitionAllocator(opts);
  __atomic_store_n(&gArenaRoots[id], allocator->root(), __ATOMIC_RELEASE);
  return id;
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t aId) {
  if (!aId || aId >= kMaxArenas || !gArenaRoots[aId]) {
    return;
  }
  gArenaRoots[aId]->PurgeMemory(kPurgeFlags);
  pthread_mutex_lock(&gArenaLock);
  gFreeArenas[gFreeArenaCount++] = aId;
  pthread_mutex_unlock(&gArenaLock);
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t aId, size_t aSize) {
  return RootForArena(aId)->Alloc<kAllocFlags>(aSize);
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t aId, size_t aNum,
                                        size_t aSize) {
  size_t total;
  if (__builtin_mul_overflow(aNum, aSize, &total)) {
    return nullptr;
  }
  return RootForArena(aId)->Alloc<kZeroAllocFlags>(total);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t aId, void* aPtr,
                                         size_t aSize) {
  return ReallocIn(RootForArena(aId), aPtr, aSize);
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t, void* aPtr) {
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t aId, size_t aAlignment,
                                          size_t aSize) {
  pa::PartitionRoot* root = RootForArena(aId);
  if (aAlignment <= pa::internal::kAlignment) {
    return root->Alloc<kAllocFlags>(aSize);
  }
  return root->AlignedAlloc<kAllocFlags>(aAlignment, aSize);
}

namespace {

class TotalsDumper : public pa::PartitionStatsDumper {
 public:
  void PartitionDumpTotals(const char*,
                           const pa::PartitionMemoryStats* aStats) override {
    mMapped += aStats->total_mmapped_bytes;
    mCommitted += aStats->total_committed_bytes;
    mAllocated += aStats->total_allocated_bytes;
    mResident += aStats->total_resident_bytes;
    mActive += aStats->total_active_bytes;
    mDecommittable += aStats->total_decommittable_bytes;
    mDiscardable += aStats->total_discardable_bytes;
    if (aStats->has_thread_cache) {
      mThreadCache += aStats->all_thread_caches_stats.bucket_total_memory;
    }
  }
  void PartitionsDumpBucketStats(
      const char*, const pa::PartitionBucketMemoryStats*) override {}

  size_t mMapped = 0;
  size_t mCommitted = 0;
  size_t mAllocated = 0;
  size_t mResident = 0;
  size_t mActive = 0;
  size_t mDecommittable = 0;
  size_t mDiscardable = 0;
  size_t mThreadCache = 0;
};

}  // namespace

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  TotalsDumper dumper;
  size_t roots = 0;
  ForEachRoot([&](pa::PartitionRoot* aRoot) {
    aRoot->DumpStats("moz", /* is_light_dump */ true,
                     /* populate_discardable_bytes */ true, &dumper);
    roots++;
  });
  aStats->narenas = roots;
  aStats->quantum = pa::internal::kAlignment;
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->chunksize = pa::internal::kSuperPageSize;
  aStats->mapped = dumper.mMapped;
  // The roots count slots held by thread caches as allocated.
  aStats->allocated = dumper.mAllocated > dumper.mThreadCache
                          ? dumper.mAllocated - dumper.mThreadCache
                          : dumper.mAllocated;
  aStats->bin_unused = dumper.mThreadCache;
  aStats->waste =
      dumper.mResident > dumper.mActive + dumper.mDecommittable
          ? dumper.mResident - dumper.mActive - dumper.mDecommittable
          : 0;
  aStats->pages_dirty = dumper.mDecommittable;
  aStats->pages_madvised = dumper.mCommitted > dumper.mResident
                               ? dumper.mCommitted - dumper.mResident
                               : 0;
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  size_t allocated = 0;
  ForEachRoot([&](pa::PartitionRoot* aRoot) {
    allocated += aRoot->get_total_size_of_allocated_bytes();
  });
  aStats->allocated_bytes = allocated;
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  pa::ThreadCache::PurgeCurrentThread();
  pa::MemoryReclaimer::Instance()->ReclaimAll();
}

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() {
  pa::ThreadCache::PurgeAllThread();
  pa::MemoryReclaimer::Instance()->ReclaimAll();
}

MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() {
  pa::MemoryReclaimer::Instance()->ReclaimFast();
}

MOZ_JEMALLOC_API void moz_set_max_dirty_page_modifier(int32_t) {}

MOZ_JEMALLOC_API bool moz_enable_deferred_purge(bool aEnable) {
  return gDeferredPurge.exchange(aEnable);
}

// Runs MemoryReclaimer::ReclaimFast and ThreadCache::RunPeriodicPurge at the
// intervals PartitionAlloc recommends, which Chromium does from its own task
// runners.
MOZ_JEMALLOC_API may_purge_now_result_t moz_may_purge_now(bool aPeekOnly,
                                                          uint32_t,
                                                          purge_keep_going_t,
                                                          void*) {
  if (!gDeferredPurge.load(std::memory_order_relaxed)) {
    return Done;
  }
  int64_t now = NowMicros();
  bool reclaimDue = now >= gNextReclaimMicros.load(std::memory_order_relaxed);
  bool tcacheDue =
      now >= gNextThreadCachePurgeMicros.load(std::memory_order_relaxed);
  if (!reclaimDue && !tcacheDue) {
    return WantsLater;
  }
  if (aPeekOnly) {
    return NeedsMore;
  }
  if (reclaimDue) {
    auto* reclaimer = pa::MemoryReclaimer::Instance();
    reclaimer->ReclaimFast();
    gNextReclaimMicros.store(
        now + reclaimer->GetRecommendedReclaimIntervalInMicroseconds(),
        std::memory_order_relaxed);
  }
  if (tcacheDue) {
    pa::ThreadCache::RunPeriodicPurge();
    gNextThreadCachePurgeMicros.store(
        now + pa::ThreadCache::GetPeriodicPurgeNextIntervalInMicroseconds(),
        std::memory_order_relaxed);
  }
  return WantsLater;
}

MOZ_JEMALLOC_API void jemalloc_reset_small_alloc_randomization(bool) {}

MOZ_JEMALLOC_API void jemalloc_thread_local_arena(bool) {}

MOZ_JEMALLOC_API void jemalloc_ptr_info(const void* aPtr,
                                        jemalloc_ptr_info_t* aInfo) {
  size_t size = OwnedSize(aPtr);
  if (size > 1) {
    SetPtrInfo(aInfo, TagLiveAlloc, const_cast<void*>(aPtr), size);
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

// The hooks Chromium's macOS zone calls from force_lock / force_unlock /
// reinit_lock. Zone.c calls _malloc_postfork in both parent and child.
extern "C" void _malloc_prefork(void) {
  gForkPid = getpid();
  pa::PartitionAllocMallocHookOnBeforeForkInParent();
}

extern "C" void _malloc_postfork(void) {
  if (getpid() == gForkPid) {
    pa::PartitionAllocMallocHookOnAfterForkInParent();
  } else {
    pa::PartitionAllocMallocHookOnAfterForkInChild();
  }
  gForkPid = -1;
}
