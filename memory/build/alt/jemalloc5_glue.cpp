/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// mozmemory API (malloc_decls.h) implemented on top of upstream jemalloc 5,
// built with the je_ prefix and without its own macOS zone.
//
// Every moz arena is a jemalloc arena (arenas.create) used through
// MALLOCX_ARENA with the automatic thread cache, so small allocations may be
// served from the calling thread's cache rather than the arena itself.
// jemalloc cannot destroy an arena while other threads' caches may still hold
// its regions, so disposed arenas are purged and their index reused.

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "AltCommon.h"

#include "jemalloc/jemalloc.h"

extern "C" {
size_t moz_je5_owned_size(const void* aPtr);
}

using namespace mozilla::alt;

namespace {

inline size_t OwnedSize(const void* aPtr) {
  return aPtr ? moz_je5_owned_size(aPtr) : 0;
}

inline size_t NonZero(size_t aSize) { return aSize ? aSize : 1; }

template <typename T>
T CtlRead(const char* aName, T aDefault = T()) {
  T value = aDefault;
  size_t len = sizeof(value);
  if (je_mallctl(aName, &value, &len, nullptr, 0) != 0) {
    return aDefault;
  }
  return value;
}

template <typename T>
T CtlReadf(T aDefault, const char* aFormat, ...) {
  char name[128];
  va_list args;
  va_start(args, aFormat);
  vsnprintf(name, sizeof(name), aFormat, args);
  va_end(args);
  return CtlRead<T>(name, aDefault);
}

void CtlCallf(const char* aFormat, unsigned aArena) {
  char name[64];
  snprintf(name, sizeof(name), aFormat, aArena);
  je_mallctl(name, nullptr, nullptr, nullptr, 0);
}

void RefreshStats() {
  uint64_t epoch = 1;
  size_t len = sizeof(epoch);
  je_mallctl("epoch", &epoch, &len, &epoch, len);
}

pthread_mutex_t gArenaLock = PTHREAD_MUTEX_INITIALIZER;
constexpr size_t kMaxFreeArenas = 4096;
unsigned gFreeArenas[kMaxFreeArenas];
size_t gFreeArenaCount = 0;

}  // namespace

MOZ_MEMORY_API void* malloc_impl(size_t aSize) { return je_malloc(aSize); }

MOZ_MEMORY_API void* calloc_impl(size_t aNum, size_t aSize) {
  return je_calloc(aNum, aSize);
}

MOZ_MEMORY_API void free_impl(void* aPtr) {
  if (MOZ_LIKELY(OwnedSize(aPtr))) {
    je_free(aPtr);
  } else if (aPtr) {
    ForeignFree(aPtr);
  }
}

MOZ_MEMORY_API void* realloc_impl(void* aPtr, size_t aSize) {
  if (!aPtr || OwnedSize(aPtr)) {
    return je_realloc(aPtr, aSize);
  }
  return ForeignRealloc(aPtr, aSize, je_malloc);
}

MOZ_MEMORY_API void* memalign_impl(size_t aAlignment, size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return je_mallocx(NonZero(aSize), MALLOCX_ALIGN(aAlignment));
}

ALT_DEFINE_MEMALIGN_VARIANTS()

MOZ_MEMORY_API size_t malloc_usable_size_impl(usable_ptr_t aPtr) {
  return OwnedSize(aPtr);
}

MOZ_MEMORY_API size_t malloc_good_size_impl(size_t aSize) {
  return je_nallocx(NonZero(aSize), 0);
}

MOZ_JEMALLOC_API arena_id_t moz_create_arena_with_params(arena_params_t*) {
  pthread_mutex_lock(&gArenaLock);
  if (gFreeArenaCount) {
    unsigned index = gFreeArenas[--gFreeArenaCount];
    pthread_mutex_unlock(&gArenaLock);
    return index;
  }
  pthread_mutex_unlock(&gArenaLock);
  unsigned index;
  size_t len = sizeof(index);
  MOZ_RELEASE_ASSERT(je_mallctl("arenas.create", &index, &len, nullptr, 0) ==
                     0);
  return index;
}

MOZ_JEMALLOC_API void moz_dispose_arena(arena_id_t aId) {
  CtlCallf("arena.%u.purge", unsigned(aId));
  pthread_mutex_lock(&gArenaLock);
  if (gFreeArenaCount < kMaxFreeArenas) {
    gFreeArenas[gFreeArenaCount++] = unsigned(aId);
  }
  pthread_mutex_unlock(&gArenaLock);
}

MOZ_JEMALLOC_API void* moz_arena_malloc(arena_id_t aId, size_t aSize) {
  return je_mallocx(NonZero(aSize), MALLOCX_ARENA(aId));
}

MOZ_JEMALLOC_API void* moz_arena_calloc(arena_id_t aId, size_t aNum,
                                        size_t aSize) {
  size_t total;
  if (__builtin_mul_overflow(aNum, aSize, &total)) {
    return nullptr;
  }
  return je_mallocx(NonZero(total), MALLOCX_ARENA(aId) | MALLOCX_ZERO);
}

MOZ_JEMALLOC_API void* moz_arena_realloc(arena_id_t aId, void* aPtr,
                                         size_t aSize) {
  if (!aPtr) {
    return moz_arena_malloc(aId, aSize);
  }
  if (OwnedSize(aPtr)) {
    return je_rallocx(aPtr, NonZero(aSize), MALLOCX_ARENA(aId));
  }
  return ForeignRealloc(aPtr, aSize,
                        [aId](size_t aN) { return moz_arena_malloc(aId, aN); });
}

MOZ_JEMALLOC_API void moz_arena_free(arena_id_t, void* aPtr) {
  free_impl(aPtr);
}

MOZ_JEMALLOC_API void* moz_arena_memalign(arena_id_t aId, size_t aAlignment,
                                          size_t aSize) {
  if (aAlignment < sizeof(void*)) {
    aAlignment = sizeof(void*);
  }
  return je_mallocx(NonZero(aSize),
                    MALLOCX_ARENA(aId) | MALLOCX_ALIGN(aAlignment));
}

MOZ_JEMALLOC_API void jemalloc_stats_internal(jemalloc_stats_t* aStats,
                                              jemalloc_bin_stats_t* aBins) {
  if (!aStats) {
    return;
  }
  RefreshStats();
  memset(aStats, 0, sizeof(*aStats));
  size_t page = CtlRead<size_t>("arenas.page", PageSize());
  size_t allocated = CtlRead<size_t>("stats.allocated");
  size_t active = CtlRead<size_t>("stats.active");
  aStats->narenas = CtlRead<unsigned>("arenas.narenas");
  aStats->quantum = CtlRead<size_t>("arenas.quantum");
  aStats->page_size = page;
  aStats->real_page_size = PageSize();
  aStats->chunksize = page;
  aStats->mapped = CtlRead<size_t>("stats.mapped");
  aStats->allocated = allocated;
  aStats->bin_unused = active > allocated ? active - allocated : 0;
  aStats->bookkeeping = CtlRead<size_t>("stats.metadata");
  aStats->pages_dirty =
      CtlReadf<size_t>(0, "stats.arenas.%u.pdirty", MALLCTL_ARENAS_ALL) * page;
  aStats->pages_madvised =
      CtlReadf<size_t>(0, "stats.arenas.%u.pmuzzy", MALLCTL_ARENAS_ALL) * page;
  aStats->num_operations = 0;
  for (const char* kind : {"small", "large"}) {
    for (const char* op : {"nmalloc", "ndalloc"}) {
      aStats->num_operations += CtlReadf<uint64_t>(
          0, "stats.arenas.%u.%s.%s", MALLCTL_ARENAS_ALL, kind, op);
    }
  }

  if (!aBins) {
    return;
  }
  unsigned nbins = CtlRead<unsigned>("arenas.nbins");
  for (unsigned i = 0; i < nbins; i++) {
    jemalloc_bin_stats_t& bin = aBins[i];
    memset(&bin, 0, sizeof(bin));
    bin.size = CtlReadf<size_t>(0, "arenas.bin.%u.size", i);
    bin.regions_per_run = CtlReadf<uint32_t>(0, "arenas.bin.%u.nregs", i);
    bin.bytes_per_run = CtlReadf<size_t>(0, "arenas.bin.%u.slab_size", i);
    size_t curregs = CtlReadf<size_t>(0, "stats.arenas.%u.bins.%u.curregs",
                                      MALLCTL_ARENAS_ALL, i);
    bin.num_runs = CtlReadf<size_t>(0, "stats.arenas.%u.bins.%u.curslabs",
                                    MALLCTL_ARENAS_ALL, i);
    bin.num_non_full_runs = CtlReadf<size_t>(
        0, "stats.arenas.%u.bins.%u.nonfull_slabs", MALLCTL_ARENAS_ALL, i);
    bin.bytes_total = bin.num_runs * bin.bytes_per_run;
    size_t used = curregs * bin.size;
    bin.bytes_unused = bin.bytes_total > used ? bin.bytes_total - used : 0;
  }
}

MOZ_JEMALLOC_API size_t jemalloc_stats_num_bins() {
  return CtlRead<unsigned>("arenas.nbins");
}

MOZ_JEMALLOC_API void jemalloc_stats_lite(jemalloc_stats_lite_t* aStats) {
  if (!aStats) {
    return;
  }
  RefreshStats();
  aStats->allocated_bytes = CtlRead<size_t>("stats.allocated");
  aStats->num_operations = 0;
}

MOZ_JEMALLOC_API void jemalloc_set_main_thread() {}

MOZ_JEMALLOC_API void jemalloc_purge_freed_pages() {
  je_mallctl("thread.tcache.flush", nullptr, nullptr, nullptr, 0);
  CtlCallf("arena.%u.purge", MALLCTL_ARENAS_ALL);
}

MOZ_JEMALLOC_API void jemalloc_free_dirty_pages() {
  jemalloc_purge_freed_pages();
}

// Purges only what the decay settings say is overdue.
MOZ_JEMALLOC_API void jemalloc_free_excess_dirty_pages() {
  CtlCallf("arena.%u.decay", MALLCTL_ARENAS_ALL);
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

// jemalloc registers its own pthread_atfork handlers; locking again from the
// zone's force_lock would deadlock on its non-recursive mutexes.
extern "C" void _malloc_prefork(void) {}

extern "C" void _malloc_postfork(void) {}
