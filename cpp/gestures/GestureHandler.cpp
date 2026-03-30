#include "GestureHandler.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace reactnativecesium {

static constexpr double kPanScale = 1.55e-8;

void GestureHandler::setViewportSize(float width, float height) {
  viewportWidth_  = std::max(width,  1.0f);
  viewportHeight_ = std::max(height, 1.0f);
}

int GestureHandler::findTouch(int id) const {
  for (int i = 0; i < touchCount_; ++i) {
    if (touches_[i].id == id) return i;
  }
  return -1;
}

void GestureHandler::onTouchDown(int pointerId, float x, float y) {
  if (touchCount_ < kMaxTouches) {
    touches_[touchCount_++] = {pointerId, {x, y}};
  }
  updateGestureState();
}

void GestureHandler::onTouchMove(int pointerId, float x, float y) {
  int idx = findTouch(pointerId);
  if (idx < 0) return;
  touches_[idx].pt = {x, y};

  if (state_ == GestureState::Pan && touchCount_ == 1 && panEnabled_) {
    deltaPanX_ += (x - prevPanPoint_.x) * panSensitivity_;
    deltaPanY_ += (y - prevPanPoint_.y) * panSensitivity_;
    prevPanPoint_ = {x, y};

  } else if (state_ == GestureState::Pinch && touchCount_ >= 2) {
    float currentDist = pinchDistance();
    if (pinchZoomEnabled_ && prevPinchDist_ > 1.0f && currentDist > 1.0f) {
      double ratio = static_cast<double>(currentDist) / prevPinchDist_;
      // Remap ratio through sensitivity: 1 + (ratio - 1) * s
      double adj = 1.0 + (ratio - 1.0) * pinchSensitivity_;
      deltaAltitudeScale_ *= std::max(adj, 1e-6);
    }
    prevPinchDist_ = currentDist;

    if (pinchRotateEnabled_) {
      const auto& p0 = touches_[0].pt;
      const auto& p1 = touches_[1].pt;
      float angle = std::atan2(p1.y - p0.y, p1.x - p0.x);
      if (prevRotationAngle_ != 0.0f) {
        deltaHeading_ += pinchSensitivity_ *
            glm::degrees(static_cast<double>(angle - prevRotationAngle_));
      }
      prevRotationAngle_ = angle;
    } else {
      prevRotationAngle_ = 0.0f;
    }
  }
}

void GestureHandler::onTouchUp(int pointerId) {
  int idx = findTouch(pointerId);
  if (idx >= 0) {
    touches_[idx] = touches_[--touchCount_]; // compact: swap with last
  }
  updateGestureState();
  prevRotationAngle_ = 0.0f;
}

void GestureHandler::updateGestureState() {
  if (touchCount_ == 0) {
    state_ = GestureState::None;
  } else if (touchCount_ == 1) {
    state_ = GestureState::Pan;
    prevPanPoint_ = touches_[0].pt;
  } else {
    state_ = GestureState::Pinch;
    prevPinchDist_     = pinchDistance();
    prevRotationAngle_ = 0.0f;
  }
}

float GestureHandler::pinchDistance() const {
  if (touchCount_ < 2) return 0.0f;
  float dx = touches_[1].pt.x - touches_[0].pt.x;
  float dy = touches_[1].pt.y - touches_[0].pt.y;
  return std::sqrt(dx * dx + dy * dy);
}

void GestureHandler::applyToCamera(GlobeCamera& camera) const {
  auto params = camera.getParams();

  if (deltaPanX_ != 0.0 || deltaPanY_ != 0.0) {
    const double sens   = params.altitude * kPanScale;
    const double H      = glm::radians(params.heading);
    const double cosH   = std::cos(H);
    const double sinH   = std::sin(H);
    const double latRad = glm::radians(params.latitude);

    const double fwdAmount = deltaPanY_ * sens;
    const double rgtAmount = -deltaPanX_ * sens;

    params.latitude  += cosH * fwdAmount + (-sinH) * rgtAmount;
    params.longitude += (sinH * fwdAmount + cosH * rgtAmount)
                        / std::max(std::cos(latRad), 0.01);
  }

  params.altitude *= deltaAltitudeScale_;
  params.heading  += deltaHeading_;

  params.latitude = std::clamp(params.latitude, -90.0, 90.0);
  params.altitude = std::clamp(params.altitude, 10.0, 100000000.0);
  params.heading  = std::fmod(params.heading, 360.0);
  if (params.heading < 0.0) params.heading += 360.0;

  camera.setParams(params);
}

void GestureHandler::resetDeltas() {
  deltaPanX_          = 0.0;
  deltaPanY_          = 0.0;
  deltaAltitudeScale_ = 1.0;
  deltaHeading_       = 0.0;
}

} // namespace reactnativecesium
