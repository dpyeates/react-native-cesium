#include "CameraTargetState.hpp"

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

namespace {

inline double angleDeltaAbsDeg(double a, double b) {
  return std::abs(std::fmod(b - a + 540.0, 360.0) - 180.0);
}

inline double quatDotAbs(const glm::dquat& a, const glm::dquat& b) {
  return std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
}

// Shortest signed angular delta for longitude, wrapped to [-180, 180].
inline double lonDeltaDeg(double from, double to) {
  return std::fmod(to - from + 540.0, 360.0) - 180.0;
}

} // namespace

// ── Public writers ────────────────────────────────────────────────────────────

void CameraTargetState::setAll(const CameraParams& target) {
  {
    std::lock_guard<std::mutex> g(mutex_);
    target_ = target;
    // Deliberately do NOT touch interpolation state: setAll is engine-init only
    // and the first frame must snap to the requested position immediately.
  }
  dirty_.store(true, std::memory_order_release);
  forceRender_.store(true, std::memory_order_release);
}

void CameraTargetState::setHpr(double lat, double lon, double alt,
                                double heading, double pitch, double roll) {
  {
    std::lock_guard<std::mutex> g(mutex_);

    const bool posChanged =
        std::abs(lat - target_.latitude)  > tunables::kEpsLatLon ||
        std::abs(lon - target_.longitude) > tunables::kEpsLatLon;

    if (posChanged) {
      const auto now = std::chrono::steady_clock::now();

      if (!hasInterpolation_) {
        // First runtime lat/lon update: seed posFrom = destination so that
        // lerp(dest, dest, t) = dest for all t — clean snap, no startup glide.
        posFromLat_       = lat;
        posFromLon_       = lon;
        ewmaIntervalSec_  = tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha;
        posUpdateTime_    = now;
        hasInterpolation_ = true;
      } else {
        // Subsequent update: record where we're moving FROM (the previous
        // target), update the EWMA interval, and reset the clock.
        posFromLat_ = target_.latitude;
        posFromLon_ = target_.longitude;

        const double dt = std::chrono::duration<double>(now - posUpdateTime_).count();
        if (dt > 0.0) {
          ewmaIntervalSec_ = ewmaIntervalSec_ +
              tunables::kEwmaIntervalAlpha * (dt - ewmaIntervalSec_);
        }
        posUpdateTime_ = now;
      }
    }

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

void CameraTargetState::setViewCorrection(const glm::dquat& q) {
  {
    std::lock_guard<std::mutex> g(mutex_);
    target_.viewCorrection = q;
    // Position cadence intentionally NOT updated — viewCorrection changes
    // carry no lat/lon information and must not affect the interpolation window.
  }
  dirty_.store(true, std::memory_order_release);
  forceRender_.store(true, std::memory_order_release);
}

// ── Snapshot (render-thread read) ─────────────────────────────────────────────

CameraParams CameraTargetState::snapshot() const {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  CameraParams result = target_;

  if (hasInterpolation_ && ewmaIntervalSec_ > 0.0) {
    const double dLat = target_.latitude - posFromLat_;
    const double dLon = lonDeltaDeg(posFromLon_, target_.longitude);

    // Snap-through guard: genuine teleports (setCamera(NewYork → Tokyo)) bypass
    // the interpolation so they remain instant rather than gliding across the planet.
    const bool snapThrough =
        std::abs(dLat) > tunables::kPosSnapThroughDeg ||
        std::abs(dLon) > tunables::kPosSnapThroughDeg;

    if (!snapThrough) {
      const double elapsed =
          std::chrono::duration<double>(now - posUpdateTime_).count();
      const double t = std::min(elapsed / ewmaIntervalSec_, 1.0);

      result.latitude  = posFromLat_ + t * dLat;
      result.longitude = posFromLon_ + t * dLon;
    }
  }

  return result;
}

// ── Epsilon check (shared) ────────────────────────────────────────────────────

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

} // namespace reactnativecesium
