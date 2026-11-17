/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CausalMarkers_h
#define CausalMarkers_h

// Markers that express explicit cause -> effect links, used for critical path
// analysis. See "Causal markers" in tools/profiler/docs/markers-guide.md.
//
// The id type is `Flow` (mozilla/Flow.h). A trigger marker says "work with id X
// was made necessary here"; an effect marker says "this is the work that ran
// because of X". All of these are gated on ProfilerFeature::Flows.

#include "mozilla/BaseProfilerMarkers.h"
#include "mozilla/CausalPending.h"
#include "mozilla/ProfilerLabels.h"
#include "mozilla/ProfilerMarkers.h"
#include "nsString.h"
#include "ETWTools.h"

namespace geckoprofiler::markers {

// An instant marker that is the trigger for the given id.
class CausalTriggerMarker
    : public mozilla::BaseMarkerType<CausalTriggerMarker> {
 public:
  static constexpr const char* Name = "CausalTriggerMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {{
      "trigger",
      MS::InputType::Flow,
      "Trigger",
      MS::Format::CausalTrigger,
  }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* AllLabels =
      "{marker.name} (trigger={marker.data.trigger})";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;
};

class CausalTriggerTextMarker
    : public mozilla::BaseMarkerType<CausalTriggerTextMarker> {
 public:
  static constexpr const char* Name = "CausalTriggerTextMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {
      {
          "name",
          MS::InputType::CString,
          "Details",
          MS::Format::UniqueString,
      },
      {
          "trigger",
          MS::InputType::Flow,
          "Trigger",
          MS::Format::CausalTrigger,
      }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* TableLabel =
      "{marker.data.name}(trigger={marker.data.trigger})";
  static constexpr const char* ChartLabel = "{marker.name}";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;
};

// An effect marker. Effects should be interval markers covering the work that
// ran because of the id, hence IsStackBased.
class CausalEffectMarker : public mozilla::BaseMarkerType<CausalEffectMarker> {
 public:
  static constexpr const char* Name = "CausalEffectMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {{
      "effect",
      MS::InputType::Flow,
      "Effect",
      MS::Format::CausalEffect,
  }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* AllLabels =
      "{marker.name} (effect={marker.data.effect})";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;

  static constexpr bool IsStackBased = true;
};

class CausalEffectTextMarker
    : public mozilla::BaseMarkerType<CausalEffectTextMarker> {
 public:
  static constexpr const char* Name = "CausalEffectTextMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {
      {
          "name",
          MS::InputType::CString,
          "Details",
          MS::Format::UniqueString,
      },
      {
          "effect",
          MS::InputType::Flow,
          "Effect",
          MS::Format::CausalEffect,
      }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* TableLabel =
      "{marker.data.name}(effect={marker.data.effect})";
  static constexpr const char* ChartLabel = "{marker.name}";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;

  static constexpr bool IsStackBased = true;
};

// Dispatch-side marker for runnables and tasks: carries the legacy flow id and
// a causal trigger with the same id. The id must be the runnable/task pointer
// that a later "Runnable"/"Task" marker reports.
class FlowAndTriggerMarker
    : public mozilla::BaseMarkerType<FlowAndTriggerMarker> {
 public:
  static constexpr const char* Name = "FlowAndTriggerMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {
      {
          "flow",
          MS::InputType::Flow,
          "Flow",
          MS::Format::Flow,
      },
      {
          "trigger",
          MS::InputType::Flow,
          "Trigger",
          MS::Format::CausalTrigger,
      }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* AllLabels =
      "{marker.name} (flow={marker.data.flow})";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;

  // Callers pass a single Flow, which is written to both fields.
  static void TranslateMarkerInputToSchema(void* aContext, const Flow& aFlow) {
    ETW::OutputMarkerSchema(aContext, FlowAndTriggerMarker{}, aFlow, aFlow);
  }

  static void StreamJSONMarkerData(
      mozilla::baseprofiler::SpliceableJSONWriter& aWriter, const Flow& aFlow) {
    aWriter.FlowProperty("flow", aFlow);
    aWriter.FlowProperty("trigger", aFlow);
  }
};

// Run-side marker for microtasks: terminates the legacy flow and is the effect
// of the causal id (same id in both fields).
class TerminatingFlowAndEffectMarker
    : public mozilla::BaseMarkerType<TerminatingFlowAndEffectMarker> {
 public:
  static constexpr const char* Name = "TerminatingFlowAndEffectMarker";

  using MS = mozilla::MarkerSchema;
  static constexpr MS::PayloadField PayloadFields[] = {
      {
          "flow",
          MS::InputType::Flow,
          "Flow",
          MS::Format::TerminatingFlow,
      },
      {
          "effect",
          MS::InputType::Flow,
          "Effect",
          MS::Format::CausalEffect,
      }};

  static constexpr MS::Location Locations[] = {MS::Location::MarkerChart,
                                               MS::Location::MarkerTable};
  static constexpr const char* AllLabels =
      "{marker.name} (flow={marker.data.flow})";

  static constexpr MS::ETWMarkerGroup Group = MS::ETWMarkerGroup::Generic;

  static constexpr bool IsStackBased = true;

  // Callers pass a single Flow, which is written to both fields.
  static void TranslateMarkerInputToSchema(void* aContext, const Flow& aFlow) {
    ETW::OutputMarkerSchema(aContext, TerminatingFlowAndEffectMarker{}, aFlow,
                            aFlow);
  }

  static void StreamJSONMarkerData(
      mozilla::baseprofiler::SpliceableJSONWriter& aWriter, const Flow& aFlow) {
    aWriter.FlowProperty("flow", aFlow);
    aWriter.FlowProperty("effect", aFlow);
  }
};

}  // namespace geckoprofiler::markers

namespace mozilla {

class MOZ_RAII AutoProfilerTerminatingFlowEffectMarker {
 public:
  AutoProfilerTerminatingFlowEffectMarker(
      const char* aMarkerName, const mozilla::MarkerCategory& aCategory,
      Flow aFlow)
      : mMarkerName(aMarkerName), mCategory(aCategory), mFlow(aFlow) {
    if (profiler_is_active_and_unpaused() &&
        profiler_feature_active(ProfilerFeature::Flows)) {
      mActive = true;
      mOptions.Set(mozilla::MarkerTiming::InstantNow());
    }
  }

  ~AutoProfilerTerminatingFlowEffectMarker() {
    if (mActive && profiler_is_active_and_unpaused()) {
      mOptions.TimingRef().SetIntervalEnd();
      profiler_add_marker(
          mozilla::ProfilerString8View::WrapNullTerminatedString(mMarkerName),
          mCategory, std::move(mOptions),
          ::geckoprofiler::markers::TerminatingFlowAndEffectMarker{}, mFlow);
    }
  }

 public:
  const char* mMarkerName;
  mozilla::MarkerCategory mCategory;
  mozilla::MarkerOptions mOptions;
  Flow mFlow;
  bool mActive = false;
};

class MOZ_RAII AutoProfilerCausalEffectMarker {
 public:
  AutoProfilerCausalEffectMarker(const char* aMarkerName,
                                 const mozilla::MarkerCategory& aCategory,
                                 Flow aFlow, CausalPending* aPending = nullptr)
      : mMarkerName(aMarkerName), mCategory(aCategory), mFlow(aFlow) {
    // TakeEffect() must run even when the profiler is inactive.
    if ((!aPending || aPending->TakeEffect()) &&
        profiler_is_active_and_unpaused() &&
        profiler_feature_active(ProfilerFeature::Flows)) {
      mActive = true;
      mOptions.Set(mozilla::MarkerTiming::InstantNow());
    }
  }

  ~AutoProfilerCausalEffectMarker() {
    if (mActive && profiler_is_active_and_unpaused()) {
      mOptions.TimingRef().SetIntervalEnd();
      profiler_add_marker(
          mozilla::ProfilerString8View::WrapNullTerminatedString(mMarkerName),
          mCategory, std::move(mOptions),
          ::geckoprofiler::markers::CausalEffectMarker{}, mFlow);
    }
  }

 public:
  const char* mMarkerName;
  mozilla::MarkerCategory mCategory;
  mozilla::MarkerOptions mOptions;
  Flow mFlow;
  bool mActive = false;
};

#define AUTO_PROFILER_CAUSAL_EFFECT_MARKER(markerName, categoryName, flow) \
  ::mozilla::AutoProfilerCausalEffectMarker PROFILER_RAII(                 \
      markerName, ::mozilla::baseprofiler::category::categoryName, flow)

// Like AUTO_PROFILER_CAUSAL_EFFECT_MARKER, for a coalesced id deduplicated
// with a CausalPending: the effect is only recorded if a trigger is open.
#define AUTO_PROFILER_CAUSAL_EFFECT_MARKER_IF_PENDING(pending, markerName, \
                                                      categoryName, flow)  \
  ::mozilla::AutoProfilerCausalEffectMarker PROFILER_RAII(                 \
      markerName, ::mozilla::baseprofiler::category::categoryName, flow,   \
      &(pending))

// Record an instant trigger marker for `flow` (only with the Flows feature).
#define PROFILER_CAUSAL_TRIGGER(markerName, categoryName, flow)                \
  do {                                                                         \
    if (profiler_feature_active(ProfilerFeature::Flows)) {                     \
      profiler_add_marker(markerName, ::geckoprofiler::category::categoryName, \
                          {}, ::geckoprofiler::markers::CausalTriggerMarker{}, \
                          flow);                                               \
    }                                                                          \
  } while (false)

// Like PROFILER_CAUSAL_TRIGGER, for a coalesced id deduplicated with a
// CausalPending: only the first trigger since the last effect is recorded.
#define PROFILER_CAUSAL_TRIGGER_ONCE(pending, markerName, categoryName, flow)  \
  do {                                                                         \
    if (profiler_feature_active(ProfilerFeature::Flows) &&                     \
        (pending).MarkTrigger()) {                                             \
      profiler_add_marker(markerName, ::geckoprofiler::category::categoryName, \
                          {}, ::geckoprofiler::markers::CausalTriggerMarker{}, \
                          flow);                                               \
    }                                                                          \
  } while (false)

}  // namespace mozilla

#endif  // CausalMarkers_h
