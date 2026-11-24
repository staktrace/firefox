/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Pieces of jemalloc5_glue.cpp that need jemalloc's internal headers.

#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_init.h"
#include "jemalloc/internal/jemalloc_internal_inlines_a.h"
#include "jemalloc/internal/jemalloc_internal_inlines_c.h"

// Usable size of aPtr if jemalloc owns it, 0 otherwise. Same test as
// jemalloc's own macOS zone.
size_t moz_je5_owned_size(const void* aPtr) {
  if (!malloc_initialized()) {
    return 0;
  }
  return ivsalloc(tsdn_fetch(), aPtr);
}

// As moz_je5_owned_size, also telling whether aPtr is in an arena created
// through arenas.create rather than one of the automatic arenas.
size_t moz_je5_owned_size_and_arena(const void* aPtr, bool* aManualArena) {
  *aManualArena = false;
  if (!malloc_initialized()) {
    return 0;
  }
  emap_full_alloc_ctx_t ctx;
  if (emap_full_alloc_ctx_try_lookup(tsdn_fetch(), &arena_emap_global, aPtr,
                                     &ctx) ||
      !ctx.edata) {
    return 0;
  }
  *aManualArena = edata_arena_ind_get(ctx.edata) >= manual_arena_base;
  return edata_usize_get(ctx.edata);
}
