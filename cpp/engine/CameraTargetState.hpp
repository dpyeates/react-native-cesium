#pragma once

#include "EngineTunables.hpp"
#include "GlobeCamera.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

namespace reactnativecesium {

// Thread-safe holder for the *demand* camera (what the JS side last asked for).
//
// Writers: the React Native JS / worklet thread, via setCamera / setCameraQuaternion.
// Readers: the dedicated render thread (per frame), via shouldRenderNextFrame /
//          renderFrame.
//
// We use a small mutex around `target_` because the writes are uncontended and
// the read side only happens once per frame; the cost is negligible compared to
// the cost of *not* synchronising a 56-byte struct that contains a glm::dquat.
//
// `dirty_` and `forceRender_` are independent atomic flags so the read side can
// poll without taking the lock when the camera has not moved.
//
// Per-DoF linear interpolation (lat/lon, altitude, heading)
// ──────────────────────────────────────────────────────────
// Each of lat/lon, altitude, and heading maintains its own independent EWMA
// of inter-arrival times and only updates that EWMA when *its own value*
// actually changes. snapshot() then linearly interpolates each DoF from its
// previous value to its current target over its own EWMA interval.
//
// This means lat/lon, altitude, and heading each self-tune independently:
//   - 60 Hz gesture input → EWMA ~16 ms → interpolation near-instant (snap)
//   - 1 Hz GPS → EWMA ~1 s → constant-velocity glide between fixes
//   - 50 Hz barometer for altitude + 1 Hz GPS for lat/lon → altitude behaves
//     near-instant, lat/lon glides — with no special-casing needed.
//
// Special cases in snapshot():
//   - t >= 1: camera has reached the target, returns target_ directly.
//   - Snap-through guard: large jumps bypass interpolation so deliberate
//     teleports remain instant.
//   - First change per DoF: "from" = destination, so lerp(dest, dest, t) = dest
//     for all t — clean snap, no startup glide.
//   - setAll (init-only): never touches any interpolation state.
//
// pitch / roll / viewCorrection are NOT interpolated here; CameraSmoother
// applies fixed-rate exponential curves to them (appropriate for high-rate
// attitude sensors).
class CameraTargetState {
public:
  CameraTargetState() = default;

  // Replace the entire target. Used only for engine initialisation; does NOT
  // update any interpolation state so the first rendered frame snaps.
  void setAll(const CameraParams& target);

  // Update lat/lon/alt + heading/pitch/roll, leaving viewCorrection unchanged.
  // Independently tracks cadence for lat/lon, altitude, and heading.
  void setHpr(double lat, double lon, double alt,
              double heading, double pitch, double roll);

  void setViewCorrection(const glm::dquat& q);

  // Snapshot the demand target for this render frame. lat/lon, altitude, and
  // heading are linearly interpolated to their "current" position based on
  // each DoF's own EWMA interval. pitch/roll/viewCorrection are returned as-is
  // (CameraSmoother handles their exponential curves).
  CameraParams snapshot() const;

  // Lightweight, lock-free check used by the render thread to decide whether
  // it should bother taking a full snapshot at all.
  bool isDirty() const noexcept {
    return dirty_.load(std::memory_order_acquire);
  }
  void clearDirty() noexcept {
    dirty_.store(false, std::memory_order_release);
  }

  bool consumeForceRender() noexcept {
    return forceRender_.exchange(false, std::memory_order_acq_rel);
  }
  void requestForceRender() noexcept {
    forceRender_.store(true, std::memory_order_release);
  }

  // Compares snapshot to "current" (the engine's current camera) and returns
  // true if any DoF differs by more than the matching epsilon. Used to decide
  // whether to render this frame.
  static bool deltaExceedsEpsilon(const CameraParams& a, const CameraParams& b);

private:
  mutable std::mutex mutex_;
  CameraParams target_;
  std::atomic<bool> dirty_{false};
  std::atomic<bool> forceRender_{true};

  // ── Per-DoF linear interpolation state (all protected by mutex_) ─────────
  // Each DoF has: a "from" value, a wall-clock timestamp of the last change,
  // an EWMA of inter-arrival times, and an "active" flag.
  //
  // Pattern per DoF:
  //   First change:      from = destination (snap), seed EWMA, set active.
  //   Subsequent change: from = old target, update EWMA, reset clock.
  //   snapshot():        t = clamp(elapsed/ewma, 0, 1); result = from + t*delta.

  // lat / lon
  double                                posFromLat_{0.0};
  double                                posFromLon_{0.0};
  std::chrono::steady_clock::time_point posUpdateTime_{};
  double                                posEwmaIntervalSec_{0.0};
  bool                                  hasPosInterp_{false};

  // altitude
  double                                altFrom_{0.0};
  std::chrono::steady_clock::time_point altUpdateTime_{};
  double                                altEwmaIntervalSec_{0.0};
  bool                                  hasAltInterp_{false};

  // heading
  double                                hdgFrom_{0.0};
  std::chrono::steady_clock::time_point hdgUpdateTime_{};
  double                                hdgEwmaIntervalSec_{0.0};
  bool                                  hasHdgInterp_{false};
};

} // namespace reactnativecesium
