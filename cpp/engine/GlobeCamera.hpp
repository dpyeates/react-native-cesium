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

  void setVerticalFovDegrees(double degrees);
  double getVerticalFovDegrees() const;

  /// Bearing / tilt to look from the current camera position toward |target*| (degrees).
  void computeHeadingPitchToward(double targetLatDeg,
                                 double targetLonDeg,
                                 double targetAltMeters,
                                 double& outHeadingDeg,
                                 double& outPitchDeg) const;

  Cesium3DTilesSelection::ViewState
  computeViewState(double viewportWidth, double viewportHeight) const;

  glm::dvec3 getECEFPosition() const;

  glm::mat4 computeVPMatrix(double viewportWidth, double viewportHeight) const;

private:
  void recompute() const;

  mutable std::mutex mutex_;
  CameraParams params_;

  double verticalFovDeg_ = 60.0;

  mutable bool       dirty_        = true;
  mutable glm::dvec3 ecefPosition_{0.0};
  mutable glm::dvec3 direction_{0.0, 1.0, 0.0};
  mutable glm::dvec3 up_{0.0, 0.0, 1.0};
};

} // namespace reactnativecesium
