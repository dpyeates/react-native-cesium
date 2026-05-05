#include "CameraSmoother.hpp"

#include "EngineTunables.hpp"

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

namespace {

inline double lerpAngleDeg(double a, double b, double t) {
  // Wraps difference to [-180, 180] before applying the mix factor so we
  // always rotate the short way around the unit circle.
  double diff = std::fmod(b - a + 540.0, 360.0) - 180.0;
  return a + diff * t;
}

inline double angleDeltaAbsDeg(double a, double b) {
  return std::abs(std::fmod(b - a + 540.0, 360.0) - 180.0);
}

inline double quatDotAbs(const glm::dquat& a, const glm::dquat& b) {
  return std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
}

inline double wrapLonDeltaDeg(double a, double b) {
  return std::fmod(b - a + 540.0, 360.0) - 180.0;
}

} // namespace

CameraParams CameraSmoother::step(
    const CameraParams& current, const CameraParams& target, double dt,
    double recentIntervalSec) {
  CameraParams next = current;

  // ── lat/lon: adaptive exponential smoothing ─────────────────────────────
  // τ scales with the cadence of incoming setCamera calls so gestures feel
  // instant (τ at the floor) while sparse GPS-rate updates ease across each
  // interval (τ near the ceiling).
  const double tau  = std::clamp(
      tunables::kSmoothPositionAlpha * recentIntervalSec,
      tunables::kSmoothPositionTauMin,
      tunables::kSmoothPositionTauMax);
  const double aPos = 1.0 - std::exp(-dt / tau);

  const double dLat = target.latitude - current.latitude;
  const double dLon = wrapLonDeltaDeg(current.longitude, target.longitude);
  const bool snapThrough =
      std::abs(dLat) > tunables::kPosSnapThroughDeg ||
      std::abs(dLon) > tunables::kPosSnapThroughDeg;

  if (snapThrough) {
    next.latitude  = target.latitude;
    next.longitude = target.longitude;
  } else {
    next.latitude  = current.latitude + aPos * dLat;
    next.longitude = lerpAngleDeg(current.longitude, target.longitude, aPos);
  }

  const double aAlt   = 1.0 - std::exp(-tunables::kSmoothAltitude * dt);
  const double aHdg   = 1.0 - std::exp(-tunables::kSmoothHeading  * dt);
  const double aPitch = 1.0 - std::exp(-tunables::kSmoothPitch    * dt);
  const double aRoll  = 1.0 - std::exp(-tunables::kSmoothRoll     * dt);
  const double aViewQ = 1.0 - std::exp(-tunables::kSmoothViewCorr * dt);

  next.altitude = current.altitude + aAlt * (target.altitude - current.altitude);
  next.heading  = lerpAngleDeg(current.heading, target.heading, aHdg);
  next.pitch    = lerpAngleDeg(current.pitch,   target.pitch,   aPitch);
  next.roll     = lerpAngleDeg(current.roll,    target.roll,    aRoll);

  glm::dquat cq = glm::normalize(current.viewCorrection);
  glm::dquat tq = glm::normalize(target.viewCorrection);
  if (glm::dot(cq, tq) < 0.0) tq = -tq;
  next.viewCorrection = glm::normalize(glm::slerp(cq, tq, aViewQ));

  // Eventually-exact convergence: snap once the residual is sub-epsilon.
  if (std::abs(target.latitude - next.latitude) <= tunables::kEpsLatLon)
    next.latitude = target.latitude;
  if (std::abs(wrapLonDeltaDeg(next.longitude, target.longitude))
        <= tunables::kEpsLatLon)
    next.longitude = target.longitude;
  if (std::abs(target.altitude - next.altitude) <= tunables::kEpsAltitudeMeters)
    next.altitude = target.altitude;
  if (angleDeltaAbsDeg(next.heading, target.heading) <= tunables::kEpsAngleDeg)
    next.heading = target.heading;
  if (angleDeltaAbsDeg(next.pitch, target.pitch) <= tunables::kEpsAngleDeg)
    next.pitch = target.pitch;
  if (angleDeltaAbsDeg(next.roll, target.roll) <= tunables::kEpsAngleDeg)
    next.roll = target.roll;
  if ((1.0 - quatDotAbs(next.viewCorrection, target.viewCorrection))
        <= tunables::kEpsQuatDot)
    next.viewCorrection = target.viewCorrection;

  return next;
}

} // namespace reactnativecesium
