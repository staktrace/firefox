/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of mimalloc (v3).
//
// Ownership comes from mimalloc's page map (mi_any_heap_contains), which is
// safe for arbitrary pointers. Every moz arena is a mimalloc first-class heap
// (v3 heaps allocate from any thread); the arena id is the heap pointer.

#include "AltCommon.h"

#include "mimalloc.h"
#include "mimalloc-stats.h"

using namespace mozilla::alt;

namespace {

inline bool IsOwned(const void* aPtr) { return mi_any_heap_contains(aPtr); }

inline mi_heap_t* HeapForArena(arena_id_t aId) {
  return reinterpret_cast<mi_heap_t*>(aId);
}

// mimalloc prints and carries on after a detected double free (EAGAIN) and,
// outside MI_SECURE, after corruption (EFAULT). Crash instead, as mozjemalloc
// does.
void OnMimallocError(int aErr, void*) {
  if (aErr == EAGAIN || aErr == EFAULT) {
    MOZ_CRASH("mimalloc detected heap corruption or a double free");
  }
}

__attribute__((constructor)) void RegisterErrorHandler() {
  mi_register_error(OnMimallocError, nullptr);
}

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) { return mi_malloc(aSize); }

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  return mi_calloc(aNum, aSize);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    PoisonFreedKeepingLinks(aPtr, mi_usable_size(aPtr));
    mi_free(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  if (!aPtr) {
    return mi_malloc(aSize);
  }
  if (IsOwned(aPtr)) {
    return PoisoningRealloc(aPtr, aSize, mi_usable_size, mi_malloc, mi_free,
                            mi_realloc);
  }
  return ForeignRealloc(aPtr, aSize, mi_malloc);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return mi_malloc_aligned(aSize, aAlignment);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return IsOwned(aPtr) ? mi_usable_size(aPtr) : 0;
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  return mi_good_size(aSize);
}

MOZ_JEMALLOC_API arena_id_t moz_create_arena_with_params(arena_params_t*) {
  mi_heap_t* heap = mi_heap_new();
  MOZ_RELEASE_ASSERT(heap);
  return reinterpret_cast<arena_id_t>(heap);
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t aId) {
  mi_heap_delete(HeapForArena(aId));
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t aId, size_t aSize) {
  return mi_heap_malloc(HeapForArena(aId), aSize);
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t aId, size_t aNum,
                                        size_t aSize) {
  return mi_heap_calloc(HeapForArena(aId), aNum, aSize);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t aId, void* aPtr,
                                         size_t aSize) {
  mi_heap_t* heap = HeapForArena(aId);
  if (!aPtr) {
    return mi_heap_malloc(heap, aSize);
  }
  if (IsOwned(aPtr)) {
    return PoisoningRealloc(
        aPtr, aSize, mi_usable_size,
        [heap](size_t aN) { return mi_heap_malloc(heap, aN); }, mi_free,
        [heap](void* aP, size_t aN) { return mi_heap_realloc(heap, aP, aN); });
  }
  return ForeignRealloc(aPtr, aSize,
                        [heap](size_t aN) { return mi_heap_malloc(heap, aN); });
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t, void* aPtr) {
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t aId, size_t aAlignment,
                                          size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return mi_heap_malloc_aligned(HeapForArena(aId), aSize, aAlignment);
}

namespace {

bool GetStats(mi_stats_t* aStats) {
  memset(aStats, 0, sizeof(*aStats));
  aStats->size = sizeof(*aStats);
  aStats->version = MI_STAT_VERSION;
  return mi_stats_get(aStats);
}

inline size_t Positive(int64_t aValue) {
  return aValue > 0 ? size_t(aValue) : 0;
}

}  // namespace

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->chunksize = mi_arena_min_alignment();

  mi_stats_t s;
  if (!GetStats(&s)) {
    return;
  }
  size_t allocated = Positive(s.allocated.current);
  size_t inPages = Positive(s.page_committed.current);
  size_t committed = Positive(s.committed.current);
  aStats->narenas = Positive(s.heaps.current);
  aStats->mapped = Positive(s.reserved.current);
  aStats->allocated = allocated;
  aStats->bin_unused = inPages > allocated ? inPages - allocated : 0;
  aStats->pages_dirty = committed > inPages ? committed - inPages : 0;
  aStats->num_operations =
      Positive(s.malloc_count.total) + Positive(s.free_count.total);
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  mi_stats_t s;
  if (!GetStats(&s)) {
    aStats->allocated_bytes = 0;
    aStats->num_operations = 0;
    return;
  }
  aStats->allocated_bytes = Positive(s.allocated.current);
  aStats->num_operations =
      Positive(s.malloc_count.total) + Positive(s.free_count.total);
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() { mi_collect(true); }

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() { mi_collect(true); }

MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() { mi_collect(false); }

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
               mi_usable_size(aPtr));
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

extern "C" void _malloc_prefork(void) {}

extern "C" void _malloc_postfork(void) {}
