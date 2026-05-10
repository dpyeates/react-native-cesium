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

// Shortest signed angular delta wrapped to [-180, 180].
inline double shortAngleDeltaDeg(double from, double to) {
  return std::fmod(to - from + 540.0, 360.0) - 180.0;
}

// Advance an EWMA with a new sample, or seed it on first use.
inline void updateEwma(double& ewma, bool& active,
                       std::chrono::steady_clock::time_point& lastTime,
                       const std::chrono::steady_clock::time_point& now,
                       double seedSec) {
  if (!active) {
    ewma     = seedSec;
    lastTime = now;
    active   = true;
  } else {
    const double dt = std::chrono::duration<double>(now - lastTime).count();
    if (dt > 0.0) {
      ewma = ewma + tunables::kEwmaIntervalAlpha * (dt - ewma);
    }
    lastTime = now;
  }
}

} // namespace

// ── Public writers ────────────────────────────────────────────────────────────

void CameraTargetState::setAll(const CameraParams& target) {
  {
    std::lock_guard<std::mutex> g(mutex_);
    target_ = target;
    // Deliberately do NOT touch any interpolation state: setAll is init-only
    // and the first rendered frame must snap to the requested position.
  }
  dirty_.store(true, std::memory_order_release);
  forceRender_.store(true, std::memory_order_release);
}

void CameraTargetState::setHpr(double lat, double lon, double alt,
                                double heading, double pitch, double roll) {
  {
    std::lock_guard<std::mutex> g(mutex_);

    const auto now = std::chrono::steady_clock::now();

    // ── lat / lon ─────────────────────────────────────────────────────────
    const bool posChanged =
        std::abs(lat - target_.latitude)  > tunables::kEpsLatLon ||
        std::abs(lon - target_.longitude) > tunables::kEpsLatLon;

    if (posChanged) {
      if (!hasPosInterp_) {
        // First runtime change: seed "from" = destination for a clean snap,
        // then enable interpolation for subsequent updates.
        posFromLat_ = lat;
        posFromLon_ = lon;
        updateEwma(posEwmaIntervalSec_, hasPosInterp_, posUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
      } else {
        posFromLat_ = target_.latitude;
        posFromLon_ = target_.longitude;
        updateEwma(posEwmaIntervalSec_, hasPosInterp_, posUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
      }
    }

    // ── altitude ──────────────────────────────────────────────────────────
    const bool altChanged =
        std::abs(alt - target_.altitude) > tunables::kEpsAltitudeMeters;

    if (altChanged) {
      if (!hasAltInterp_) {
        altFrom_ = alt;
        updateEwma(altEwmaIntervalSec_, hasAltInterp_, altUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
      } else {
        altFrom_ = target_.altitude;
        updateEwma(altEwmaIntervalSec_, hasAltInterp_, altUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
      }
    }

    // ── heading ───────────────────────────────────────────────────────────
    const bool hdgChanged =
        angleDeltaAbsDeg(heading, target_.heading) > tunables::kEpsAngleDeg;

    if (hdgChanged) {
      if (!hasHdgInterp_) {
        hdgFrom_ = heading;
        updateEwma(hdgEwmaIntervalSec_, hasHdgInterp_, hdgUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
      } else {
        hdgFrom_ = target_.heading;
        updateEwma(hdgEwmaIntervalSec_, hasHdgInterp_, hdgUpdateTime_, now,
                   tunables::kSmoothPositionTauMax / tunables::kSmoothPositionAlpha);
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
    // Interpolation state is not touched — viewCorrection carries no
    // positional information and must not affect any of the three EWMAs.
  }
  dirty_.store(true, std::memory_order_release);
  forceRender_.store(true, std::memory_order_release);
}

// ── Snapshot (render-thread read) ─────────────────────────────────────────────

CameraParams CameraTargetState::snapshot() const {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  CameraParams result = target_;

  // ── lat / lon ─────────────────────────────────────────────────────────────
  if (hasPosInterp_ && posEwmaIntervalSec_ > 0.0) {
    const double dLat = target_.latitude - posFromLat_;
    const double dLon = shortAngleDeltaDeg(posFromLon_, target_.longitude);

    const bool snapThrough =
        std::abs(dLat) > tunables::kPosSnapThroughDeg ||
        std::abs(dLon) > tunables::kPosSnapThroughDeg;

    if (!snapThrough) {
      const double elapsed =
          std::chrono::duration<double>(now - posUpdateTime_).count();
      const double t = std::min(elapsed / posEwmaIntervalSec_, 1.0);
      result.latitude  = posFromLat_ + t * dLat;
      result.longitude = posFromLon_ + t * dLon;
    }
  }

  // ── altitude ──────────────────────────────────────────────────────────────
  if (hasAltInterp_ && altEwmaIntervalSec_ > 0.0) {
    const double dAlt = target_.altitude - altFrom_;

    if (std::abs(dAlt) <= tunables::kAltSnapThroughMeters) {
      const double elapsed =
          std::chrono::duration<double>(now - altUpdateTime_).count();
      const double t = std::min(elapsed / altEwmaIntervalSec_, 1.0);
      result.altitude = altFrom_ + t * dAlt;
    }
  }

  // ── heading ───────────────────────────────────────────────────────────────
  if (hasHdgInterp_ && hdgEwmaIntervalSec_ > 0.0) {
    const double dHdg = shortAngleDeltaDeg(hdgFrom_, target_.heading);

    if (std::abs(dHdg) <= tunables::kHdgSnapThroughDeg) {
      const double elapsed =
          std::chrono::duration<double>(now - hdgUpdateTime_).count();
      const double t = std::min(elapsed / hdgEwmaIntervalSec_, 1.0);
      result.heading = hdgFrom_ + t * dHdg;
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
