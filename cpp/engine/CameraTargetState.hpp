#pragma once

#include "EngineTunables.hpp"
#include "GlobeCamera.hpp"

#include <atomic>
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
class CameraTargetState {
public:
  CameraTargetState() = default;

  // Replace the entire target (does not touch viewCorrection if you want to
  // preserve it — use setHpr instead).
  void setAll(const CameraParams& target) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      target_ = target;
    }
    dirty_.store(true, std::memory_order_release);
    forceRender_.store(true, std::memory_order_release);
  }

  // Update lat/lon/alt + heading/pitch/roll, leaving viewCorrection unchanged.
  void setHpr(double lat, double lon, double alt,
              double heading, double pitch, double roll) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      target_.latitude  = lat;
      target_.longitude = lon;
      target_.altitude  = alt;
      target_.heading   = heading;
      target_.pitch     = pitch;
      target_.roll      = roll;
    }
    dirty_.store(true, std::memory_order_release);
    forceRender_.store(true, std::memory_order_release);
  }

  void setViewCorrection(const glm::dquat& q) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      target_.viewCorrection = q;
    }
    dirty_.store(true, std::memory_order_release);
    forceRender_.store(true, std::memory_order_release);
  }

  // Snapshot the demand target (read side; one mutex per frame).
  CameraParams snapshot() const {
    std::lock_guard<std::mutex> g(mutex_);
    return target_;
  }

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
};

} // namespace reactnativecesium
