#include "GlobeCamera.hpp"

#include <CesiumGeospatial/GlobeTransforms.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

static constexpr double kNearPlane = 1.0;         // 1 m
static constexpr double kFarPlane  = 50000000.0;  // 50,000 km
static constexpr double kVertFovDeg = 60.0;

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
  double rollRad  = glm::radians(params_.roll);

  // Heading rotates around the ENU up axis (North = 0°, East = 90°).
  glm::dvec3 fwd = north * std::cos(hdgRad) + east * std::sin(hdgRad);

  // Pitch tilts the forward vector up/down.
  glm::dvec3 right = glm::normalize(glm::cross(fwd, upENU));
  fwd = glm::normalize(fwd * std::cos(pitchRad) + upENU * std::sin(pitchRad));

  // Recompute right and up after pitch.
  right = glm::normalize(glm::cross(fwd, upENU));
  glm::dvec3 camUp = glm::normalize(glm::cross(right, fwd));

  // Apply roll around the forward axis.
  if (std::abs(rollRad) > 1e-6) {
    glm::dvec3 rolledRight = right * std::cos(rollRad) + camUp * std::sin(rollRad);
    camUp  = glm::normalize(glm::cross(rolledRight, fwd));
    right  = glm::normalize(rolledRight);
  }

  direction_ = fwd;
  up_        = camUp;
}

glm::dvec3 GlobeCamera::getECEFPosition() const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();
  return ecefPosition_;
}

Cesium3DTilesSelection::ViewState
GlobeCamera::computeViewState(double w, double h) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  double aspect  = w / std::max(h, 1.0);
  double vfov    = glm::radians(kVertFovDeg);
  double hfov    = 2.0 * std::atan(std::tan(vfov * 0.5) * aspect);

  return Cesium3DTilesSelection::ViewState(
      ecefPosition_, direction_, up_,
      glm::dvec2(w, h), hfov, vfov);
}

glm::mat4 GlobeCamera::computeVPMatrix(double w, double h) const {
  std::lock_guard<std::mutex> lk(mutex_);
  recompute();

  // Rotation-only view matrix (no camera translation).
  // Eye-relative positions are computed on the CPU in double precision,
  // so the GPU only needs the rotational part of the view transform.
  glm::dmat4 fullView = glm::lookAt(ecefPosition_, ecefPosition_ + direction_, up_);
  glm::mat4 viewRot   = glm::mat4(glm::dmat4(
      fullView[0], fullView[1], fullView[2], glm::dvec4(0.0, 0.0, 0.0, 1.0)));

  double aspect    = w / std::max(h, 1.0);
  double vfov      = glm::radians(kVertFovDeg);
  double tanHalfV  = std::tan(vfov * 0.5);
  double tanHalfH  = tanHalfV * aspect;

  // Infinite reversed-Z perspective matrix (near→NDC 1, far→NDC 0).
  // Compatible with Metal depth clear=0, compare=GREATER.
  //   clip_z = near (constant)
  //   clip_w = -eye_z = depth > 0
  //   NDC_z  = near / depth      →  near=1m → NDC 1.0,  50Mkm → NDC ~0
  float f = static_cast<float>(kNearPlane); // near plane (1 m)
  glm::mat4 proj(0.0f);
  // Metal NDC has Y=+1 at the top of screen — identical to OpenGL.
  // No Y-flip needed (Y-flip is Vulkan-specific where NDC Y is inverted).
  proj[0][0] =  static_cast<float>(1.0 / tanHalfH);
  proj[1][1] =  static_cast<float>(1.0 / tanHalfV);
  proj[2][3] = -1.0f;    // clip_w = -eye_z
  proj[3][2] =  f;       // clip_z = near (constant)

  return proj * viewRot;
}

} // namespace reactnativecesium
