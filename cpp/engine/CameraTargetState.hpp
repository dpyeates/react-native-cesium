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
// Lat/lon interpolation
// ─────────────────────
// When setHpr is called with a new lat/lon, `posFrom` (old target) and
// `posUpdateTime` are recorded inside the mutex. snapshot() then linearly
// interpolates posFrom → target over `ewmaIntervalSec` (EWMA of inter-arrival
// times). This produces constant-velocity motion between position fixes —
// realistic for any vehicle/GPS source — without any changes to the public API.
//
// Special cases handled in snapshot():
//   - t >= 1 : camera has reached the target, returns target_ directly.
//   - Snap-through guard: deltas > kPosSnapThroughDeg bypass interpolation so
//     deliberate teleports (setCamera(NewYork → Tokyo)) remain instant.
//   - First setHpr for a given lat/lon: posFrom is set to the *destination*
//     so lerp(dest, dest, t) = dest — no startup glide.
//   - setAll (initialisation only): does not touch interpolation state; always
//     snaps directly.
class CameraTargetState {
public:
  CameraTargetState() = default;

  // Replace the entire target. Used only for engine initialisation; does NOT
  // update position-interpolation state so the first rendered frame snaps.
  void setAll(const CameraParams& target);

  // Update lat/lon/alt + heading/pitch/roll, leaving viewCorrection unchanged.
  // When lat/lon actually changes, updates the linear-interpolation bookkeeping.
  void setHpr(double lat, double lon, double alt,
              double heading, double pitch, double roll);

  void setViewCorrection(const glm::dquat& q);

  // Snapshot the demand target for this render frame.  Lat/lon are linearly
  // interpolated between the previous target (posFrom) and the current target
  // over the EWMA inter-arrival interval.  All other DoFs are returned as-is
  // (the smoother handles their exponential curves).
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

  // ── Lat/lon linear interpolation state (all protected by mutex_) ──────────
  // posFromLat_/posFromLon_: the target position when the last update arrived
  //   (i.e. where we are smoothly moving FROM).
  // posUpdateTime_:          wall-clock time of the last lat/lon change.
  // ewmaIntervalSec_:        EWMA of inter-arrival times of lat/lon changes.
  // hasInterpolation_:       true once we have valid posFrom + posUpdateTime
  //                          data (false until the second distinct lat/lon value).
  double                                 posFromLat_{0.0};
  double                                 posFromLon_{0.0};
  std::chrono::steady_clock::time_point  posUpdateTime_{};
  double                                 ewmaIntervalSec_{0.0};
  bool                                   hasInterpolation_{false};
};

} // namespace reactnativecesium
