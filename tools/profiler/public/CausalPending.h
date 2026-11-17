/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CausalPending_h
#define CausalPending_h

#include "mozilla/Atomics.h"

namespace mozilla {

// Deduplication state for a causal id where many triggers can precede one
// effect. Used with PROFILER_CAUSAL_TRIGGER_ONCE and
// AUTO_PROFILER_CAUSAL_EFFECT_MARKER_IF_PENDING (CausalMarkers.h): only the
// first trigger since the last effect is recorded, and an effect only when a
// trigger is open.
class CausalPending {
 public:
  // Returns true if no trigger was open; the caller should then record one.
  bool MarkTrigger() { return !mPending.exchange(true); }

  // Returns true if a trigger was open; the caller should then record the
  // effect. Must be called even when the profiler is inactive.
  bool TakeEffect() { return mPending && mPending.exchange(false); }

 private:
  Atomic<bool, Relaxed> mPending{false};
};

}  // namespace mozilla

#endif  // CausalPending_h
