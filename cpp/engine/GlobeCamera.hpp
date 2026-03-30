#pragma once

#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <Cesium3DTilesSelection/ViewState.h>

#include <glm/glm.hpp>
#include <mutex>

namespace reactnativecesium {

struct CameraParams {
  double latitude  = 46.10;
  double longitude = 7.80;
  double altitude  = 5500.0;
  double heading   = 220.0;
  double pitch     = -10.0;
  double roll      = 0.0;
};

class GlobeCamera {
public:
  GlobeCamera();

  void setParams(const CameraParams& params);
  CameraParams getParams() const;

  // Cesium tile-selection view state (double precision, full position + FOV).
  Cesium3DTilesSelection::ViewState
  computeViewState(double viewportWidth, double viewportHeight) const;

  // Camera ECEF position (double, used for eye-relative vertex computation).
  glm::dvec3 getECEFPosition() const;

  // View-projection matrix for the GPU:
  //   flipY(reversedZProjection) * rotationOnlyViewMatrix
  // Rotation-only view + reversed-Z infinite projection (near=1, far=50M km).
  // Combined here so CesiumBridge only needs one matrix.
  glm::mat4 computeVPMatrix(double viewportWidth, double viewportHeight) const;

private:
  void recompute() const;

  mutable std::mutex mutex_;
  CameraParams params_;

  mutable bool      dirty_        = true;
  mutable glm::dvec3 ecefPosition_{0.0};
  mutable glm::dvec3 direction_  {0.0, 1.0, 0.0};
  mutable glm::dvec3 up_         {0.0, 0.0, 1.0};
};

} // namespace reactnativecesium
