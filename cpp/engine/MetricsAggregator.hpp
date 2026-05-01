#pragma once

#include "../renderer/RenderTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace reactnativecesium {

// Per-frame snapshot the platform shells expose to JS via Nitro / JNI.
struct MetricsSnapshot {
  double fps;
  int    tilesRendered;
  int    tilesLoading;
  int    tilesVisited;
  bool   ionTokenConfigured;
  bool   tilesetReady;
  bool   tlsConfigured;
  std::string creditsPlainText;
};

// Aggregates per-frame metrics on the render thread, throttling emission
// (every kMetricsEmitEveryFrames frames) so we do not spam the JS bridge.
//
// Usage: call `tick(dt, frameResult, tlsConfigured)` once per rendered frame;
// take the optional return value as the "publish to JS" signal. The internal
// FPS EMA persists between ticks.
class MetricsAggregator {
public:
  MetricsAggregator() = default;

  // Returns std::nullopt on most frames; returns the latest values once the
  // throttle counter wraps. Idempotent w.r.t. credit text — only re-strips
  // HTML when the joined HTML lines actually change frame-to-frame.
  std::optional<MetricsSnapshot>
  tick(double dt, const FrameResult& frame, bool tlsConfigured);

  // Returns the most recently *computed* snapshot (fps EMA + counters); used
  // by the platform read-back accessors. Cheap.
  const MetricsSnapshot& latest() const { return latest_; }

  // Strips HTML tags + collapses whitespace + trims. Pure utility, exposed so
  // platform shells that prefer to render their own credit lines can reuse it.
  static std::string stripHtmlToPlain(const std::string& html);

private:
  double fpsEma_      = 0.0;
  int    metricsTick_ = 0;
  std::string lastCreditHtmlJoined_;
  MetricsSnapshot latest_{};
};

} // namespace reactnativecesium
