/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of WebKit's libpas, built
// as WebKit builds it for bmalloc (PAS_BMALLOC=1). This file stands in for the
// parts of WebKit's bmalloc that libpas expects (g_config, the SystemHeap
// hooks) and drives libpas the way bmalloc/bmalloc.cpp does.
//
// Every moz arena gets its own libpas auxiliary (primitive) heap, the same
// mechanism bmalloc uses for its Gigacage heaps. libpas cannot destroy a heap,
// so disposed arenas are scavenged and their heap reused by the next arena.

#include <pthread.h>
#include <unistd.h>

#include "AltCommon.h"

// libpas headers include <stdatomic.h>, which must not end up inside the
// extern "C" blocks below.
#include <stdatomic.h>

extern "C" {
#include "bmalloc_heap.h"
#include "bmalloc_heap_inlines.h"
#include "bmalloc_heap_innards.h"
#include "bmalloc_heap_ref.h"
#include "bmalloc_heap_utils.h"
#include "bmalloc_type.h"
#include "pas_all_heaps.h"
#include "pas_heap_lock.h"
#include "pas_heap_summary.h"
#include "pas_probabilistic_guard_malloc_allocator.h"
#include "pas_runtime_config.h"
#include "pas_scavenger.h"
#include "pas_segregated_heap.h"
#include "pas_system_heap.h"
#include "pas_thread_local_cache.h"
#include "pas_utility_heap.h"
}

using namespace mozilla::alt;

// WebKit keeps libpas' runtime configuration in WTF's g_config.
extern "C" {
alignas(16) Slot g_config[PAS_RUNTIME_CONFIG_RESERVED_SLOTS];
}

// WebKit's SystemHeap, which diverts bmalloc to system malloc when requested
// through the environment. Like WebKit by default, it is never enabled here.
extern "C" {
bool pas_system_heap_is_enabled(pas_heap_config_kind) { return false; }
bool pas_system_heap_should_supplant_bmalloc(pas_heap_config_kind) {
  return false;
}
void* pas_system_heap_malloc(size_t) { MOZ_CRASH("no system heap"); }
void* pas_system_heap_memalign(size_t, size_t) { MOZ_CRASH("no system heap"); }
void* pas_system_heap_realloc(void*, size_t) { MOZ_CRASH("no system heap"); }
void* pas_system_heap_malloc_untagged(size_t) { MOZ_CRASH("no system heap"); }
void* pas_system_heap_memalign_untagged(size_t, size_t) {
  MOZ_CRASH("no system heap");
}
void* pas_system_heap_realloc_untagged(void*, size_t) {
  MOZ_CRASH("no system heap");
}
void pas_system_heap_free(void*) { MOZ_CRASH("no system heap"); }
}

// WebKit turns PGM on in a small random fraction of processes; enable it
// everywhere, for every allocation slow-path call. PGM's own memory budget
// bounds how many allocations it guards at a time.
__attribute__((constructor)) static void EnablePGM() {
  pas_probabilistic_guard_malloc_initialize_pgm_as_enabled(1);
}

namespace {

// bmalloc_get_allocation_size returns 0 for PGM allocations, which would make
// our malloc zone disown them and system free() abort, so look those up in
// PGM's own table.
inline size_t OwnedSize(const void* aPtr) {
  if (!aPtr) {
    return 0;
  }
  if (size_t size = bmalloc_get_allocation_size(const_cast<void*>(aPtr))) {
    return size;
  }
  pas_heap_lock_lock();
  pas_large_map_entry entry =
      pas_probabilistic_guard_malloc_return_as_large_map_entry(
          reinterpret_cast<uintptr_t>(aPtr));
  pas_heap_lock_unlock();
  return entry.end - entry.begin;
}

constexpr size_t kMaxArenas = 1024;

// libpas keeps pointers to the type and heap ref, so slots never move.
struct Arena {
  bmalloc_type mType;
  pas_primitive_heap_ref mRef;
  char mLabel[128];
};

Arena gArenas[kMaxArenas];
pthread_mutex_t gArenaLock = PTHREAD_MUTEX_INITIALIZER;
size_t gArenaCount = 1;  // Id 0 is the common heap.
arena_id_t gFreeArenas[kMaxArenas];
size_t gFreeArenaCount = 0;
std::atomic<size_t> gLiveArenas{0};

inline pas_primitive_heap_ref* RefForArena(arena_id_t aId) {
  return aId && aId < kMaxArenas ? &gArenas[aId].mRef : nullptr;
}

void ScavengeThisThread() {
  pas_thread_local_cache_shrink(pas_thread_local_cache_try_get(),
                                pas_lock_is_not_held);
}

pid_t gForkPid = -1;

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) {
  return bmalloc_try_allocate_inline(aSize);
}

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  size_t total;
  if (__builtin_mul_overflow(aNum, aSize, &total)) {
    return nullptr;
  }
  return bmalloc_try_allocate_zeroed_inline(total);
}

namespace {
inline bool TryDeallocate(void* aPtr) {
  return bmalloc_try_deallocate_inline_only(aPtr) ||
         bmalloc_try_deallocate_casual(aPtr);
}
}  // namespace

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (!aPtr) {
    return;
  }
  if (size_t size = OwnedSize(aPtr)) {
    PoisonFreed(aPtr, size);
  }
  if (!TryDeallocate(aPtr)) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  if (!aPtr) {
    return malloc_impl(aSize);
  }
  if (OwnedSize(aPtr)) {
    return PoisoningRealloc(aPtr, aSize, OwnedSize, malloc_impl, TryDeallocate,
                            [](void* aP, size_t aN) {
                              return bmalloc_try_reallocate_inline(
                                  aP, aN, pas_reallocate_free_if_successful);
                            });
  }
  return ForeignRealloc(aPtr, aSize, malloc_impl);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return bmalloc_try_allocate_with_alignment_inline(aSize, aAlignment);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return OwnedSize(aPtr);
}

// libpas has no public size-class rounding.
MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) { return aSize; }

MOZ_JEMALLOC_API arena_id_t
moz_create_arena_with_params(arena_params_t* aParams) {
  gLiveArenas.fetch_add(1, std::memory_order_relaxed);
  pthread_mutex_lock(&gArenaLock);
  arena_id_t id = 0;
  if (gFreeArenaCount) {
    id = gFreeArenas[--gFreeArenaCount];
  } else if (gArenaCount < kMaxArenas) {
    id = gArenaCount++;
    Arena& arena = gArenas[id];
    strlcpy(arena.mLabel,
            aParams && aParams->mLabel ? aParams->mLabel : "moz_arena",
            sizeof(arena.mLabel));
    arena.mType = BMALLOC_TYPE_INITIALIZER(1, 1, arena.mLabel);
    arena.mRef = BMALLOC_AUXILIARY_HEAP_REF_INITIALIZER(&arena.mType);
  }
  pthread_mutex_unlock(&gArenaLock);
  return id;
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t aId) {
  gLiveArenas.fetch_sub(1, std::memory_order_relaxed);
  if (!RefForArena(aId)) {
    return;
  }
  pthread_mutex_lock(&gArenaLock);
  gFreeArenas[gFreeArenaCount++] = aId;
  pthread_mutex_unlock(&gArenaLock);
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t aId, size_t aSize) {
  if (pas_primitive_heap_ref* ref = RefForArena(aId)) {
    return bmalloc_try_allocate_auxiliary_inline(ref, aSize);
  }
  return malloc_impl(aSize);
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t aId, size_t aNum,
                                        size_t aSize) {
  pas_primitive_heap_ref* ref = RefForArena(aId);
  if (!ref) {
    return calloc_impl(aNum, aSize);
  }
  size_t total;
  if (__builtin_mul_overflow(aNum, aSize, &total)) {
    return nullptr;
  }
  return bmalloc_try_allocate_auxiliary_zeroed_inline(ref, total);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t aId, void* aPtr,
                                         size_t aSize) {
  pas_primitive_heap_ref* ref = RefForArena(aId);
  if (!ref) {
    return realloc_impl(aPtr, aSize);
  }
  if (!aPtr) {
    return bmalloc_try_allocate_auxiliary_inline(ref, aSize);
  }
  if (OwnedSize(aPtr)) {
    return PoisoningRealloc(
        aPtr, aSize, OwnedSize,
        [ref](size_t aN) {
          return bmalloc_try_allocate_auxiliary_inline(ref, aN);
        },
        TryDeallocate,
        [ref](void* aP, size_t aN) {
          return bmalloc_try_reallocate_auxiliary_inline(
              aP, ref, aN, pas_reallocate_free_if_successful);
        });
  }
  return ForeignRealloc(aPtr, aSize, [ref](size_t aN) {
    return bmalloc_try_allocate_auxiliary_inline(ref, aN);
  });
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t, void* aPtr) {
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t aId, size_t aAlignment,
                                          size_t aSize) {
  pas_primitive_heap_ref* ref = RefForArena(aId);
  if (!ref) {
    return memalign_impl(aAlignment, aSize);
  }
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return bmalloc_try_allocate_auxiliary_with_alignment_inline(ref, aSize,
                                                              aAlignment);
}

namespace {

// Includes libpas' own utility heap so that heap-allocated covers everything
// libpas has handed out.
pas_heap_summary HeapSummary() {
  pas_heap_lock_lock();
  pas_heap_summary summary = pas_heap_summary_add(
      pas_all_heaps_compute_total_non_utility_summary(),
      pas_segregated_heap_compute_summary(&pas_utility_segregated_heap));
  pas_heap_lock_unlock();
  return summary;
}

}  // namespace

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  pas_heap_summary s = HeapSummary();
  aStats->narenas = 1 + gLiveArenas.load(std::memory_order_relaxed);
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->mapped = s.committed + s.decommitted;
  aStats->allocated = s.allocated;
  aStats->waste = s.free_ineligible_for_decommit;
  aStats->pages_dirty = s.free_eligible_for_decommit;
  aStats->pages_madvised = s.decommitted;
  aStats->bookkeeping = s.meta;
  aStats->bin_unused = s.cached;
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  aStats->allocated_bytes = HeapSummary().allocated;
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

// Same as bmalloc::api::scavenge().
MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  pas_scavenger_run_synchronously_now();
  ScavengeThisThread();
}

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() {
  jemalloc_purge_freed_pages();
}

MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() {
  pas_scavenger_decommit_free_memory();
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
  if (size_t size = OwnedSize(aPtr)) {
    SetPtrInfo(aInfo, TagLiveAlloc, const_cast<void*>(aPtr), size);
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

// libpas registers no fork handlers. Hold the heap lock across fork; the child
// may not unlock an os_unfair_lock taken by the parent, so it reinitializes it.
extern "C" void _malloc_prefork(void) {
  pas_heap_lock_lock();
  gForkPid = getpid();
}

extern "C" void _malloc_postfork(void) {
  if (getpid() == gForkPid) {
    pas_heap_lock_unlock();
  } else {
    pas_lock_construct(&pas_heap_lock);
  }
  gForkPid = -1;
}
