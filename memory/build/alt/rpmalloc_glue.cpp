/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of rpmalloc.
//
// rpmalloc gets all of its memory from one address range reserved at init, so
// a pointer is ours iff it lies in that range. The macOS zone (Zone.c) relies
// on malloc_usable_size returning 0 for pointers that are not ours.
//
// Arenas created with ARENA_FLAG_THREAD_MAIN_THREAD_ONLY get their own rpmalloc
// first-class heap; the id is the heap pointer tagged with kHeapArenaTag.
// rpmalloc's first-class heaps are single-threaded and their blocks take the
// unsynchronized free path from any thread, which is exactly the contract of
// main-thread-only arenas. Thread-safe arenas would need a lock around every
// allocation and every free, so they share rpmalloc's thread heaps instead.

#include <atomic>
#include <pthread.h>
#include <sys/mman.h>

#ifdef XP_DARWIN
#  include <mach/mach.h>
#  include <mach/mach_vm.h>
#  include <mach/vm_statistics.h>
#endif

#include "AltCommon.h"

#define RPMALLOC_FIRST_CLASS_HEAPS 1
#include "rpmalloc.h"

extern "C" size_t rpmalloc_good_size(size_t aSize);

using namespace mozilla::alt;

namespace {

constexpr size_t kMaxRegionSize = size_t(1) << 40;
constexpr size_t kMinRegionSize = size_t(64) << 30;
constexpr size_t kSpanSize = size_t(256) << 20;

#ifdef XP_DARWIN
const int kMapFd = VM_MAKE_TAG(VM_MEMORY_MALLOC);
#else
const int kMapFd = -1;
#endif

char* gRegionBase = nullptr;
char* gRegionEnd = nullptr;

// Address space bookkeeping, only touched when rpmalloc maps or unmaps.
pthread_mutex_t gMapLock = PTHREAD_MUTEX_INITIALIZER;
char* gBump = nullptr;
struct FreeRun {
  char* mBase;
  size_t mLen;
};
constexpr size_t kMaxFreeRuns = 4096;
FreeRun gFreeRuns[kMaxFreeRuns];
size_t gFreeRunCount = 0;

std::atomic<size_t> gMapped{0};

std::atomic<bool> gInitialized{false};
pthread_once_t gInitOnce = PTHREAD_ONCE_INIT;

// Thread-safe arenas get even ids; main-thread-only ones are tagged heaps.
constexpr arena_id_t kHeapArenaTag = 1;
std::atomic<arena_id_t> gNextArenaId{2};
std::atomic<size_t> gLiveArenas{0};

inline rpmalloc_heap_t* HeapForArena(arena_id_t aId) {
  if (aId & kHeapArenaTag) {
    return reinterpret_cast<rpmalloc_heap_t*>(aId & ~kHeapArenaTag);
  }
  return nullptr;
}

inline bool IsOwned(const void* aPtr) {
  const char* p = static_cast<const char*>(aPtr);
  return p >= gRegionBase && p < gRegionEnd;
}

inline uintptr_t AlignUp(uintptr_t aValue, size_t aAlignment) {
  return (aValue + aAlignment - 1) & ~(uintptr_t(aAlignment) - 1);
}

// Caller holds gMapLock.
void AddFreeRun(char* aBase, size_t aLen) {
  if (!aLen) {
    return;
  }
  for (size_t i = 0; i < gFreeRunCount; i++) {
    FreeRun& run = gFreeRuns[i];
    if (run.mBase + run.mLen == aBase) {
      run.mLen += aLen;
      return;
    }
    if (aBase + aLen == run.mBase) {
      run.mBase = aBase;
      run.mLen += aLen;
      return;
    }
  }
  if (aBase + aLen == gBump) {
    gBump = aBase;
    return;
  }
  if (gFreeRunCount < kMaxFreeRuns) {
    gFreeRuns[gFreeRunCount++] = {aBase, aLen};
  }
}

// Caller holds gMapLock.
char* TakeRange(size_t aSize, size_t aAlignment) {
  for (size_t i = 0; i < gFreeRunCount; i++) {
    FreeRun run = gFreeRuns[i];
    char* aligned = reinterpret_cast<char*>(
        AlignUp(reinterpret_cast<uintptr_t>(run.mBase), aAlignment));
    char* end = run.mBase + run.mLen;
    if (aligned <= end && aSize <= size_t(end - aligned)) {
      gFreeRuns[i] = gFreeRuns[--gFreeRunCount];
      AddFreeRun(run.mBase, aligned - run.mBase);
      AddFreeRun(aligned + aSize, end - (aligned + aSize));
      return aligned;
    }
  }
  char* aligned = reinterpret_cast<char*>(
      AlignUp(reinterpret_cast<uintptr_t>(gBump), aAlignment));
  if (aligned > gRegionEnd || aSize > size_t(gRegionEnd - aligned)) {
    return nullptr;
  }
  char* gapStart = gBump;
  gBump = aligned + aSize;
  AddFreeRun(gapStart, aligned - gapStart);
  return aligned;
}

void* OsMap(size_t aSize, size_t aAlignment, size_t* aOffset,
            size_t* aMappedSize) {
  pthread_mutex_lock(&gMapLock);
  char* p = TakeRange(aSize, aAlignment ? aAlignment : 1);
  pthread_mutex_unlock(&gMapLock);
  if (!p) {
    return nullptr;
  }
  *aOffset = 0;
  *aMappedSize = aSize;
  gMapped.fetch_add(aSize, std::memory_order_relaxed);
  return p;
}

void OsUnmap(void* aAddress, size_t aOffset, size_t aMappedSize) {
  char* base = static_cast<char*>(aAddress) - aOffset;
  // Replace the pages with fresh zero pages, releasing them, but keep the
  // range reserved so it stays inside the owned region.
  void* r = mmap(base, aMappedSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, kMapFd, 0);
  MOZ_RELEASE_ASSERT(r == base);
  gMapped.fetch_sub(aMappedSize, std::memory_order_relaxed);
  pthread_mutex_lock(&gMapLock);
  AddFreeRun(base, aMappedSize);
  pthread_mutex_unlock(&gMapLock);
}

int OsCommit(void* aAddress, size_t aSize) {
#ifdef MADV_FREE_REUSE
  while (madvise(aAddress, aSize, MADV_FREE_REUSE) == -1 && errno == EAGAIN) {
  }
#endif
  return 0;
}

int OsDecommit(void* aAddress, size_t aSize) {
#ifdef MADV_FREE_REUSABLE
  int ret;
  while ((ret = madvise(aAddress, aSize, MADV_FREE_REUSABLE)) == -1 &&
         errno == EAGAIN) {
  }
#else
  int ret = madvise(aAddress, aSize, MADV_DONTNEED);
#endif
  return ret != 0 ? 1 : 0;
}

// rpmalloc has no allocated byte counter without ENABLE_STATISTICS, so report
// the region's footprint (resident pages not marked reusable) instead.
size_t Footprint() {
#ifdef XP_DARWIN
  if (!gRegionBase) {
    return 0;
  }
  size_t total = 0;
  mach_vm_address_t addr = reinterpret_cast<mach_vm_address_t>(gRegionBase);
  const mach_vm_address_t end = reinterpret_cast<mach_vm_address_t>(gRegionEnd);
  while (addr < end) {
    mach_vm_size_t size = 0;
    vm_region_extended_info_data_t info;
    mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_EXTENDED_INFO,
                       reinterpret_cast<vm_region_info_t>(&info), &count,
                       &object) != KERN_SUCCESS ||
        addr >= end) {
      break;
    }
    if (info.pages_resident > info.pages_reusable) {
      total += size_t(info.pages_resident - info.pages_reusable) * vm_page_size;
    }
    addr += size;
  }
  return total;
#else
  return 0;
#endif
}

void InitOnce() {
  void* region = MAP_FAILED;
  size_t size = kMaxRegionSize;
  for (; size >= kMinRegionSize; size >>= 1) {
    region = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, kMapFd, 0);
    if (region != MAP_FAILED) {
      break;
    }
  }
  if (region == MAP_FAILED) {
    MOZ_CRASH("rpmalloc: could not reserve address space");
  }
  gRegionBase = static_cast<char*>(region);
  gRegionEnd = gRegionBase + size;
  gBump = gRegionBase;

  static rpmalloc_interface_t sInterface = {};
  sInterface.memory_map = OsMap;
  sInterface.memory_unmap = OsUnmap;
  sInterface.memory_commit = OsCommit;
  sInterface.memory_decommit = OsDecommit;
  if (rpmalloc_initialize(&sInterface) != 0) {
    MOZ_CRASH("rpmalloc: initialization failed");
  }
  gInitialized.store(true, std::memory_order_release);
}

MOZ_ALWAYS_INLINE void EnsureInit() {
  if (MOZ_UNLIKELY(!gInitialized.load(std::memory_order_acquire))) {
    pthread_once(&gInitOnce, InitOnce);
  }
}

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) {
  EnsureInit();
  return rpmalloc(aSize);
}

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  EnsureInit();
  return rpcalloc(aNum, aSize);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    rpfree(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  EnsureInit();
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    return rprealloc(aPtr, aSize);
  }
  if (!aPtr) {
    return rpmalloc(aSize);
  }
  return ForeignRealloc(aPtr, aSize, rpmalloc);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  EnsureInit();
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return rpaligned_alloc(aAlignment, aSize);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    return rpmalloc_usable_size(const_cast<void*>(aPtr));
  }
  return 0;
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  EnsureInit();
  return rpmalloc_good_size(aSize ? aSize : 1);
}

MOZ_JEMALLOC_API arena_id_t
moz_create_arena_with_params(arena_params_t* aParams) {
  EnsureInit();
  gLiveArenas.fetch_add(1, std::memory_order_relaxed);
  if (aParams && (aParams->mFlags & ARENA_FLAG_THREAD_MASK) ==
                     ARENA_FLAG_THREAD_MAIN_THREAD_ONLY) {
    rpmalloc_heap_t* heap = rpmalloc_heap_acquire();
    MOZ_RELEASE_ASSERT(heap && !(reinterpret_cast<arena_id_t>(heap) & 1));
    return reinterpret_cast<arena_id_t>(heap) | kHeapArenaTag;
  }
  return gNextArenaId.fetch_add(2, std::memory_order_relaxed);
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t aId) {
  gLiveArenas.fetch_sub(1, std::memory_order_relaxed);
  if (rpmalloc_heap_t* heap = HeapForArena(aId)) {
    rpmalloc_heap_release(heap);
  }
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t aId, size_t aSize) {
  if (rpmalloc_heap_t* heap = HeapForArena(aId)) {
    return rpmalloc_heap_alloc(heap, aSize);
  }
  return malloc_impl(aSize);
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t aId, size_t aNum,
                                        size_t aSize) {
  if (rpmalloc_heap_t* heap = HeapForArena(aId)) {
    return rpmalloc_heap_calloc(heap, aNum, aSize);
  }
  return calloc_impl(aNum, aSize);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t aId, void* aPtr,
                                         size_t aSize) {
  rpmalloc_heap_t* heap = HeapForArena(aId);
  if (heap && !aPtr) {
    return rpmalloc_heap_alloc(heap, aSize);
  }
  if (heap && IsOwned(aPtr)) {
    return rpmalloc_heap_realloc(heap, aPtr, aSize, 0);
  }
  return realloc_impl(aPtr, aSize);
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t aId, void* aPtr) {
  rpmalloc_heap_t* heap = HeapForArena(aId);
  if (heap && IsOwned(aPtr)) {
    rpmalloc_heap_free(heap, aPtr);
    return;
  }
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t aId, size_t aAlignment,
                                          size_t aSize) {
  if (rpmalloc_heap_t* heap = HeapForArena(aId)) {
    if (aAlignment < sizeof(void*)) {
      aAlignment = sizeof(void*);
    }
    return rpmalloc_heap_aligned_alloc(heap, aAlignment, aSize);
  }
  return memalign_impl(aAlignment, aSize);
}

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  aStats->narenas = 1 + gLiveArenas.load(std::memory_order_relaxed);
  aStats->quantum = 16;
  aStats->quantum_max = 1024;
  aStats->chunksize = kSpanSize;
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->mapped = gMapped.load(std::memory_order_relaxed);
  aStats->allocated = Footprint();
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  aStats->allocated_bytes = Footprint();
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  if (gInitialized.load(std::memory_order_acquire)) {
    rpmalloc_thread_collect();
  }
}

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() {
  jemalloc_purge_freed_pages();
}

MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() {
  jemalloc_purge_freed_pages();
}

MOZ_JEMALLOC_API void moz_set_max_dirty_page_modifier(int32_t) {}

MOZ_JEMALLOC_API bool moz_enable_deferred_purge(bool) { return false; }

MOZ_JEMALLOC_API may_purge_now_result_t moz_may_purge_now(bool, uint32_t,
                                                          purge_keep_going_t,
                                                          void*) {
  return Done;
}

MOZ_JEMALLOC_API void jemalloc_reset_small_alloc_randomization(bool) {}

MOZ_JEMALLOC_API void jemalloc_thread_local_arena(bool) {}

MOZ_JEMALLOC_API void jemalloc_ptr_info(const void* aPtr,
                                        jemalloc_ptr_info_t* aInfo) {
  if (IsOwned(aPtr)) {
    SetPtrInfo(aInfo, TagLiveAlloc, const_cast<void*>(aPtr),
               rpmalloc_usable_size(const_cast<void*>(aPtr)));
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

extern "C" void _malloc_prefork(void) { pthread_mutex_lock(&gMapLock); }

extern "C" void _malloc_postfork(void) { pthread_mutex_unlock(&gMapLock); }
