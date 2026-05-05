#include "CameraTargetState.hpp"

#include <cmath>

namespace reactnativecesium {

namespace {
inline double angleDeltaAbsDeg(double a, double b) {
  return std::abs(std::fmod(b - a + 540.0, 360.0) - 180.0);
}
inline double quatDotAbs(const glm::dquat& a, const glm::dquat& b) {
  return std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
}
} // namespace

bool CameraTargetState::deltaExceedsEpsilon(
    const CameraParams& a, const CameraParams& b) {
  return std::abs(a.latitude  - b.latitude)  > tunables::kEpsLatLon
      || std::abs(a.longitude - b.longitude) > tunables::kEpsLatLon
      || std::abs(a.altitude  - b.altitude)  > tunables::kEpsAltitudeMeters
      || angleDeltaAbsDeg(a.heading, b.heading) > tunables::kEpsAngleDeg
      || angleDeltaAbsDeg(a.pitch,   b.pitch)   > tunables::kEpsAngleDeg
      || angleDeltaAbsDeg(a.roll,    b.roll)    > tunables::kEpsAngleDeg
      || (1.0 - quatDotAbs(a.viewCorrection, b.viewCorrection))
             > tunables::kEpsQuatDot;
}

void CameraTargetState::noteUpdateCadence() noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (!hasLastUpdate_) {
    // Seed the EWMA with τmax so the first sample after a quiet period uses
    // the full smoothing budget rather than the floor (5 ms ≈ snap).
    ewmaIntervalSec_.store(tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha,
                           std::memory_order_release);
    lastUpdateTime_ = now;
    hasLastUpdate_  = true;
    return;
  }

  const double dt = std::chrono::duration<double>(now - lastUpdateTime_).count();
  lastUpdateTime_ = now;
  if (dt <= 0.0) return; // back-to-back writes from the same frame: ignore.

  const double prev = ewmaIntervalSec_.load(std::memory_order_relaxed);
  const double next = prev + tunables::kEwmaIntervalAlpha * (dt - prev);
  ewmaIntervalSec_.store(next, std::memory_order_release);
}

} // namespace reactnativecesium
