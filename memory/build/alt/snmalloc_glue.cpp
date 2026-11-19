/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of snmalloc.
//
// The per-thread allocator lives behind a pthread key rather than snmalloc's
// default thread_local: on macOS the first thread_local access on a thread
// makes dyld malloc its storage, which would re-enter our zone. Allocators
// come from snmalloc's AllocPool, as with the default thread_local setup.
//
// Ownership comes from snmalloc's pagemap. snmalloc has no heaps, so moz arenas
// share the thread allocators.

#include <pthread.h>

#define SNMALLOC_EXTERNAL_THREAD_ALLOC
#include "snmalloc/snmalloc_core.h"
#include "snmalloc/backend/globalconfig.h"

namespace snmalloc {
using Config = snmalloc::StandardConfigClientMeta<NoClientMetaDataProvider>;
using Alloc = snmalloc::Allocator<Config>;
}  // namespace snmalloc

class ThreadAllocExternal {
  static inline pthread_key_t sKey;
  static inline pthread_once_t sOnce = PTHREAD_ONCE_INIT;

  static void MakeKey() { pthread_key_create(&sKey, &Cleanup); }

  // pthread clears the key before calling us; keep it set while flushing so
  // frees made by the flush use this allocator.
  static void Cleanup(void* aAlloc) {
    auto* alloc = static_cast<snmalloc::Alloc*>(aAlloc);
    pthread_setspecific(sKey, alloc);
    alloc->flush();
    pthread_setspecific(sKey, nullptr);
    snmalloc::AllocPool<snmalloc::Config>::release(alloc);
  }

  SNMALLOC_SLOW_PATH static snmalloc::Alloc* Create() {
    snmalloc::Alloc* alloc = snmalloc::AllocPool<snmalloc::Config>::acquire();
    pthread_setspecific(sKey, alloc);
    return alloc;
  }

 public:
  static snmalloc::Alloc& get() {
    pthread_once(&sOnce, MakeKey);
    void* p = pthread_getspecific(sKey);
    if (SNMALLOC_UNLIKELY(!p)) {
      return *Create();
    }
    return *static_cast<snmalloc::Alloc*>(p);
  }
};

#include "snmalloc/snmalloc_front.h"

#include "AltCommon.h"

using namespace mozilla::alt;

namespace sn = snmalloc::libc;

namespace {

inline bool IsOwned(const void* aPtr) {
  // Non-canonical addresses cannot be snmalloc's, and reading their metadata
  // faults.
  if (!aPtr || (reinterpret_cast<uintptr_t>(aPtr) >> 48) != 0) {
    return false;
  }
  return snmalloc::Config::Backend::template get_metaentry<true>(
             snmalloc::address_cast(const_cast<void*>(aPtr)))
      .is_owned();
}

std::atomic<arena_id_t> gNextArenaId{1};
std::atomic<size_t> gLiveArenas{0};

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) { return sn::malloc(aSize); }

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  return sn::calloc(aNum, aSize);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(IsOwned(aPtr))) {
    sn::free(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  if (!aPtr || IsOwned(aPtr)) {
    return sn::realloc(aPtr, aSize);
  }
  return ForeignRealloc(aPtr, aSize, [](size_t aN) { return sn::malloc(aN); });
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return sn::memalign(aAlignment, aSize);
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return IsOwned(aPtr) ? sn::malloc_usable_size(aPtr) : 0;
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  return snmalloc::round_size(aSize);
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

// snmalloc only tracks the memory it holds from the OS, not allocated bytes.
MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t*) {
  if (!aStats) {
    return;
  }
  memset(aStats, 0, sizeof(*aStats));
  size_t usage = snmalloc::Alloc::Config::Backend::get_current_usage();
  aStats->narenas = 1 + gLiveArenas.load(std::memory_order_relaxed);
  aStats->page_size = PageSize();
  aStats->real_page_size = PageSize();
  aStats->mapped = usage;
  aStats->allocated = usage;
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() { return 0; }

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  aStats->allocated_bytes =
      snmalloc::Alloc::Config::Backend::get_current_usage();
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

// Flushes the allocators of exited threads back to the shared pool.
MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  snmalloc::cleanup_unused<snmalloc::Config>();
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
    void* base = sn::__malloc_start_pointer(const_cast<void*>(aPtr));
    SetPtrInfo(aInfo, TagLiveAlloc, base, snmalloc::alloc_size(base));
  } else {
    SetPtrInfo(aInfo, TagUnknown, nullptr, 0);
  }
}

ALT_DEFINE_PROFILER_CALLBACKS_STUB()

extern "C" void _malloc_prefork(void) {}

extern "C" void _malloc_postfork(void) {}
