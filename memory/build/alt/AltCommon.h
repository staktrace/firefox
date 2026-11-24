/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shared helpers for the memory/build/alt/*_glue.cpp backends.

#ifndef memory_build_alt_AltCommon_h
#define memory_build_alt_AltCommon_h

#include "mozmemory_wrap.h"

#include <atomic>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef XP_DARWIN
#  include <malloc/malloc.h>
#endif

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozjemalloc_types.h"
#ifdef MOZJEMALLOC_PROFILING_CALLBACKS
#  include "mozjemalloc_profiling.h"
#endif

#define MALLOC_DECL(name, return_type, ...) \
  MOZ_MEMORY_API return_type name##_impl(__VA_ARGS__);
#define MALLOC_FUNCS MALLOC_FUNCS_MALLOC
#include "malloc_decls.h"

#define MALLOC_DECL(name, return_type, ...) \
  MOZ_JEMALLOC_API return_type name(__VA_ARGS__);
#define MALLOC_FUNCS (MALLOC_FUNCS_JEMALLOC | MALLOC_FUNCS_ARENA)
#include "malloc_decls.h"

namespace mozilla::alt {

inline size_t PageSize() {
  static size_t sPageSize = size_t(sysconf(_SC_PAGESIZE));
  return sPageSize;
}

// Pointers the backend does not own reach us when another zone allocated them
// (macOS) and the caller used our malloc entry points directly.
#ifdef XP_DARWIN
inline size_t ForeignSize(const void* aPtr) {
  malloc_zone_t* zone = malloc_zone_from_ptr(aPtr);
  return zone ? zone->size(zone, aPtr) : 0;
}

inline void ForeignFree(void* aPtr) {
  malloc_zone_t* zone = malloc_zone_from_ptr(aPtr);
  MOZ_RELEASE_ASSERT(zone);
  malloc_zone_free(zone, aPtr);
}
#else
inline size_t ForeignSize(const void*) { MOZ_CRASH("foreign pointer"); }
inline void ForeignFree(void*) { MOZ_CRASH("foreign pointer"); }
#endif

// Moves a foreign block into the backend: aAlloc(aSize) then copy and free.
template <typename AllocFn>
void* ForeignRealloc(void* aPtr, size_t aSize, AllocFn aAlloc) {
  size_t oldSize = ForeignSize(aPtr);
  void* p = aAlloc(aSize);
  if (p) {
    memcpy(p, aPtr, oldSize < aSize ? oldSize : aSize);
    ForeignFree(aPtr);
  }
  return p;
}

// Poison-on-free with mozjemalloc's poison byte: every byte of every block,
// huge ones included, which mozjemalloc does not poison.
constexpr bool kPoisonAll = true;
constexpr uint8_t kPoisonByte = 0xe5;
constexpr size_t kPoisonPartialSize = 256;
constexpr size_t kPoisonMaxBlockSize = SIZE_MAX;

// aUsable is the usable size of the whole block aPtr is freed from.
inline void PoisonFreed(void* aPtr, size_t aUsable) {
  if (aUsable > kPoisonMaxBlockSize) {
    return;
  }
  memset(aPtr, kPoisonByte,
         kPoisonAll || aUsable < kPoisonPartialSize ? aUsable
                                                    : kPoisonPartialSize);
}

// For backends that keep free-list links in freed blocks (at most two words).
// Leaving them intact means a double free still finds the block's links rather
// than poison, so the backend's own double-free and free-list checks work.
constexpr size_t kFreeListLinkSize = 2 * sizeof(void*);

inline void PoisonFreedKeepingLinks(void* aPtr, size_t aUsable) {
  if (aUsable <= kFreeListLinkSize || aUsable > kPoisonMaxBlockSize) {
    return;
  }
  size_t end =
      kPoisonAll || aUsable < kPoisonPartialSize ? aUsable : kPoisonPartialSize;
  memset(static_cast<char*>(aPtr) + kFreeListLinkSize, kPoisonByte,
         end - kFreeListLinkSize);
}

// realloc for an owned block that poisons whatever it gives back: the tail
// when shrinking in place, the old block when moving.
template <typename UsableFn, typename AllocFn, typename FreeFn,
          typename ReallocFn>
void* PoisoningRealloc(void* aPtr, size_t aSize, UsableFn aUsable,
                       AllocFn aAlloc, FreeFn aFree, ReallocFn aRealloc) {
  size_t oldSize = aUsable(aPtr);
  if (oldSize > kPoisonMaxBlockSize && aSize > kPoisonMaxBlockSize) {
    return aRealloc(aPtr, aSize);
  }
  if (aSize <= oldSize && aSize >= oldSize / 2) {
    if (aSize < oldSize) {
      size_t tail = oldSize - aSize;
      memset(
          static_cast<char*>(aPtr) + aSize, kPoisonByte,
          kPoisonAll || tail < kPoisonPartialSize ? tail : kPoisonPartialSize);
    }
    return aPtr;
  }
  void* p = aAlloc(aSize);
  if (!p) {
    return nullptr;
  }
  memcpy(p, aPtr, oldSize < aSize ? oldSize : aSize);
  PoisonFreed(aPtr, oldSize);
  aFree(aPtr);
  return p;
}

inline bool IsValidAlignment(size_t aAlignment) {
  return aAlignment && !(aAlignment & (aAlignment - 1));
}

inline void SetPtrInfo(jemalloc_ptr_info_t* aInfo, PtrInfoTag aTag, void* aAddr,
                       size_t aSize) {
  aInfo->tag = aTag;
  aInfo->addr = aAddr;
  aInfo->size = aSize;
#ifdef MOZ_DEBUG
  aInfo->arenaId = 0;
#endif
}

}  // namespace mozilla::alt

// Shared posix_memalign/aligned_alloc/valloc built on the backend's
// memalign_impl.
#define ALT_DEFINE_MEMALIGN_VARIANTS()                                       \
  MOZ_MEMORY_API int posix_memalign_impl(void** aMemPtr, size_t aAlignment,  \
                                         size_t aSize) {                     \
    if (aAlignment < sizeof(void*) ||                                        \
        !mozilla::alt::IsValidAlignment(aAlignment)) {                       \
      return EINVAL;                                                         \
    }                                                                        \
    void* p = memalign_impl(aAlignment, aSize);                              \
    if (!p) {                                                                \
      return ENOMEM;                                                         \
    }                                                                        \
    *aMemPtr = p;                                                            \
    return 0;                                                                \
  }                                                                          \
  MOZ_MEMORY_API void* aligned_alloc_impl(size_t aAlignment, size_t aSize) { \
    if (!mozilla::alt::IsValidAlignment(aAlignment) || aSize % aAlignment) { \
      return nullptr;                                                        \
    }                                                                        \
    return memalign_impl(aAlignment, aSize);                                 \
  }                                                                          \
  MOZ_MEMORY_API void* valloc_impl(size_t aSize) {                           \
    return memalign_impl(mozilla::alt::PageSize(), aSize);                   \
  }

#ifdef MOZJEMALLOC_PROFILING_CALLBACKS
#  define ALT_DEFINE_PROFILER_CALLBACKS_STUB()                                 \
    namespace mozilla {                                                        \
    void jemalloc_set_profiler_callbacks(RefPtr<MallocProfilerCallbacks>&&) {} \
    }
#else
#  define ALT_DEFINE_PROFILER_CALLBACKS_STUB()
#endif

#endif  // memory_build_alt_AltCommon_h
