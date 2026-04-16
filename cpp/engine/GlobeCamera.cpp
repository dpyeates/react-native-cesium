#include "GlobeCamera.hpp"

#include <CesiumGeospatial/GlobeTransforms.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

static constexpr double kNearPlane = 1.0;
static constexpr double kFarPlane  = 50000000.0;
static constexpr double kMinVfov   = 20.0;
static constexpr double kMaxVfov   = 100.0;

GlobeCamera::GlobeCamera() { recompute(); }

void GlobeCamera::setParams(const CameraParams& p) {
  std::lock_guard<std::mutex> lk(mutex_);
  params_ = p;
  dirty_  = true;
}

CameraParams GlobeCamera::getParams() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return params_;
}

void GlobeCamera::setVerticalFovDegrees(double degrees) {
  std::lock_guard<std::mutex> lk(mutex_);
  verticalFovDeg_ = std::clamp(degrees, kMinVfov, kMaxVfov);
}

double GlobeCamera::getVerticalFovDegrees() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return verticalFovDeg_;
}

void GlobeCamera::recompute() const {
  if (!dirty_) return;
  dirty_ = false;

  const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
  CesiumGeospatial::Cartographic carto(glm::radians(params_.longitude),
                                       glm::radians(params_.latitude),
                                       params_.altitude);
  ecefPosition_ = ellipsoid.cartographicToCartesian(carto);

  glm::dmat4 enuToECEF =
      CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(
          ecefPosition_, ellipsoid);

  glm::dvec3 east  = glm::normalize(glm::dvec3(enuToECEF[0]));
  glm::dvec3 north = glm::normalize(glm::dvec3(enuToECEF[1]));
  glm::dvec3 upENU = glm::normalize(glm::dvec3(enuToECEF[2]));

  double hdgRad   = glm::radians(params_.heading);
  double pitchRad = glm::radians(params_.pitch);
  // Aviation-style sign: positive roll = right wing down, negative = left wing down.
  double rollRad  = -glm::radians(params_.roll);

  // 1. Heading: forward and right in the horizontal plane.
  glm::dvec3 fwdH  = north * std::cos(hdgRad) + east * std::sin(hdgRad);
  glm::dvec3 right = glm::normalize(glm::cross(fwdH, upENU));

  // 2. Pitch: rotate fwdH and upENU around the fixed right axis.
  //    Works for any pitch (full 360° loop) — no cross(fwd, upENU) singularity.
  glm::dvec3 fwd   = fwdH * std::cos(pitchRad) + upENU * std::sin(pitchRad);
  glm::dvec3 camUp = -fwdH * std::sin(pitchRad) + upENU * std::cos(pitchRad);

  // 3. Roll: rotate right and camUp around fwd.
  if (std::abs(rollRad) > 1e-6) {
    glm::dvec3 rolledRight = right * std::cos(rollRad) + camUp * std::sin(rollRad);
    camUp = glm::normalize(glm::cross(rolledRight, fwd));
    right = glm::normalize(rolledRight);
  }

  // 4. Optional camera-space correction (columns: right, camUp, forward).
  const glm::dquat& qIn = params_.viewCorrection;
  const double         ql2 =
      qIn.w * qIn.w + qIn.x * qIn.x + qIn.y * qIn.y + qIn.z * qIn.z;
  glm::dquat qCorr;
  if (ql2 < 1e-20) {
    qCorr = glm::dquat(1.0, 0.0, 0.0, 0.0);
  } else {
    const double inv = 1.0 / std::sqrt(ql2);
    qCorr = glm::dquat(qIn.w * inv, qIn.x * inv, qIn.y * inv, qIn.z * inv);
  }
  glm::dmat3 R_basis(right, camUp, fwd);
  glm::dmat3 R_corr  = glm::mat3_cast(qCorr);
  glm::dmat3 R_final = R_basis * R_corr;
  glm::dvec3 r0      = glm::normalize(glm::dvec3(R_final[0]));
  glm::dvec3 r2      = glm::normalize(glm::dvec3(R_final[2]));
  // Gram–Schmidt: preserve forward (column 2), rebuild right/up (matches right × fwd = camUp).
  fwd   = r2;
  right = glm::normalize(r0 - glm::dot(r0, fwd) * fwd);
  camUp = glm::normalize(glm::cross(right, fwd));

  direction_ = fwd;
  up_        = camUp;
}

glm::dvec3 GlobeCamera::getECEFPosition() const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();
  return ecefPosition_;
}

void GlobeCamera::computeHeadingPitchToward(double targetLatDeg,
                                              double targetLonDeg,
                                              double targetAltMeters,
                                              double& outHeadingDeg,
                                              double& outPitchDeg) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
  CesiumGeospatial::Cartographic targetCarto(
      glm::radians(targetLonDeg),
      glm::radians(targetLatDeg),
      targetAltMeters);
  glm::dvec3 targetEcef = ellipsoid.cartographicToCartesian(targetCarto);
  glm::dvec3 dirEcef    = glm::normalize(targetEcef - ecefPosition_);

  glm::dmat4 enuToECEF =
      CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(
          ecefPosition_, ellipsoid);
  glm::dvec3 east  = glm::normalize(glm::dvec3(enuToECEF[0]));
  glm::dvec3 north = glm::normalize(glm::dvec3(enuToECEF[1]));
  glm::dvec3 upENU = glm::normalize(glm::dvec3(enuToECEF[2]));

  const double dEast  = glm::dot(dirEcef, east);
  const double dNorth = glm::dot(dirEcef, north);
  const double dUp    = glm::dot(dirEcef, upENU);
  const double horiz  = std::sqrt(dEast * dEast + dNorth * dNorth);

  outHeadingDeg = glm::degrees(std::atan2(dEast, dNorth));
  outPitchDeg   = glm::degrees(std::atan2(dUp, horiz));
}

Cesium3DTilesSelection::ViewState
GlobeCamera::computeViewState(double w, double h) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  double aspect = w / std::max(h, 1.0);
  double vfov   = glm::radians(verticalFovDeg_);
  double hfov   = 2.0 * std::atan(std::tan(vfov * 0.5) * aspect);

  return Cesium3DTilesSelection::ViewState(
      ecefPosition_, direction_, up_,
      glm::dvec2(w, h), hfov, vfov);
}

glm::mat4 GlobeCamera::computeVPMatrix(double w, double h) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  glm::dmat4 fullView = glm::lookAt(ecefPosition_, ecefPosition_ + direction_, up_);
  glm::mat4 viewRot   = glm::mat4(glm::dmat4(
      fullView[0], fullView[1], fullView[2], glm::dvec4(0.0, 0.0, 0.0, 1.0)));

  double aspect   = w / std::max(h, 1.0);
  double vfov     = glm::radians(verticalFovDeg_);
  double tanHalfV = std::tan(vfov * 0.5);
  double tanHalfH = tanHalfV * aspect;

  float f = static_cast<float>(kNearPlane);
  glm::mat4 proj(0.0f);
  proj[0][0] = static_cast<float>(1.0 / tanHalfH);
  proj[1][1] = static_cast<float>(1.0 / tanHalfV);
  proj[2][3] = -1.0f;
  proj[3][2] = f;

  return proj * viewRot;
}

glm::dmat4 GlobeCamera::computeVPMatrixDouble(double w, double h) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  // Rotation-only view (camera-at-origin, ECEF-oriented) — same structure as
  // computeVPMatrix but all operations stay in double.
  glm::dmat4 fullView = glm::lookAt(ecefPosition_, ecefPosition_ + direction_, up_);
  glm::dmat4 viewRot(
      fullView[0], fullView[1], fullView[2], glm::dvec4(0.0, 0.0, 0.0, 1.0));

  double aspect   = w / std::max(h, 1.0);
  double vfov     = glm::radians(verticalFovDeg_);
  double tanHalfV = std::tan(vfov * 0.5);
  double tanHalfH = tanHalfV * aspect;

  glm::dmat4 proj(0.0);
  proj[0][0] = 1.0 / tanHalfH;
  proj[1][1] = 1.0 / tanHalfV;
  proj[2][3] = -1.0;
  proj[3][2] = kNearPlane;

  return proj * viewRot;
}

} // namespace reactnativecesium
