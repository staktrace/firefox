/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AllocProbes_h
#define mozilla_AllocProbes_h

#include "mozilla/Types.h"

MOZ_BEGIN_EXTERN_C

/* If aArgv[1] is --alloc-probe, runs the probe the remaining arguments name
 * and returns the process exit status; otherwise returns -1. */
MFBT_API int MozAllocProbesMain(int aArgc, char** aArgv);

MOZ_END_EXTERN_C

#endif /* mozilla_AllocProbes_h */
