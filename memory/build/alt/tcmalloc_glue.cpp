/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of gperftools tcmalloc
// (tcmalloc_minimal, built with TCMALLOC_SKIP_OVERRIDE so it neither defines
// malloc nor registers its own macOS zone).
//
// Ownership uses MallocExtension::GetOwnership, the same pagemap test as
// tcmalloc's own macOS zone. tcmalloc has no heaps, so moz arenas share the
// thread caches. tcmalloc registers its own pthread_atfork handlers.

#include "AltCommon.h"

#include "gperftools/malloc_extension.h"
#include "gperftools/nallocx.h"
#include "gperftools/tcmalloc.h"

using namespace mozilla::alt;

namespace {

inline bool IsOwned(const void* aPtr) {
  return aPtr && MallocExtension::instance()->GetOwnership(aPtr) ==
                     MallocExtension::kOwned;
}

size_t Property(const char* aName) {
  size_t value = 0;
  MallocExtension::instance()->GetNumericProperty(aName, &value);
  return value;
}

std::atomic<arena_id_t> gNextArenaId{1};
std::atomic<size_t> gLiveArenas{0};

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) { return tc_malloc(aSize); }

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  return tc_calloc(aNum, aSize);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    tc_free(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  if (!aPtr || IsOwned(aPtr)) {
    return tc_realloc(aPtr, aSize);
  }
  return ForeignRealloc(aPtr, aSize, tc_malloc);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return tc_memalign(aAlignment, aSize);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return IsOwned(aPtr) ? MallocExtension::instance()->GetAllocatedSize(aPtr)
                       : 0;
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  return tc_nallocx(aSize ? aSize : 1, 0);
}

MOZ_JEMALLOC_API arena_id_t moz_create_arena_with_params(arena_params_t*) {
  gLiveArenas.fetch_add(1, std::memory_order_relaxed);
  return gNextArenaId.fetch_add(1, std::memory_order_relaxed);
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t) {
  gLiveArenas.fetch_sub(1, std::memory_order_relaxed);
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t, size_t aSize) {
  return malloc_impl(aSize);
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t, size_t aNum, size_t aSize) {
  return calloc_impl(aNum, aSize);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t, void* aPtr, size_t aSize) {
  return realloc_impl(aPtr, aSize);
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t, void* aPtr) {
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t, size_t aAlignment,
                                          size_t aSize) {
  return memalign_impl(aAlignment, aSize);
}

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  aStats->narenas = 1 + gLiveArenas.load(std::memory_order_relaxed);
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->mapped = Property("generic.heap_size");
  aStats->allocated = Property("generic.current_allocated_bytes");
  aStats->pages_dirty = Property("tcmalloc.pageheap_free_bytes");
  aStats->pages_madvised = Property("tcmalloc.pageheap_unmapped_bytes");
  aStats->bin_unused = Property("tcmalloc.central_cache_free_bytes") +
                       Property("tcmalloc.transfer_cache_free_bytes") +
                       Property("tcmalloc.thread_cache_free_bytes");
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  aStats->allocated_bytes = Property("generic.current_allocated_bytes");
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  MallocExtension::instance()->ReleaseFreeMemory();
}

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() {
  MallocExtension::instance()->ReleaseFreeMemory();
}

MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() {
  MallocExtension::instance()->ReleaseFreeMemory();
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
               MallocExtension::instance()->GetAllocatedSize(aPtr));
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

extern "C" void _malloc_prefork(void) {}

extern "C" void _malloc_postfork(void) {}
