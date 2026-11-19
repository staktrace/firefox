/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Pieces of jemalloc5_glue.cpp that need jemalloc's internal headers.

#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/assert.h"
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
