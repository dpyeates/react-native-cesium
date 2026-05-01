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

} // namespace reactnativecesium
