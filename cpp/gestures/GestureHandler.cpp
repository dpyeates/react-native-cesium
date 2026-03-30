#include "GestureHandler.hpp"

#include <cmath>

namespace reactnativecesium {

// Scale factor: degrees of lat/lon per pixel per metre of altitude.
// At 5800m altitude, a 100px swipe covers ~1km (~0.009 degrees).
static constexpr double kPanScale = 1.55e-8;

void GestureHandler::setViewportSize(float width, float height) {
  viewportWidth_  = std::max(width,  1.0f);
  viewportHeight_ = std::max(height, 1.0f);
}

void GestureHandler::onTouchDown(int pointerId, float x, float y) {
  activePointers_[pointerId] = {x, y};
  updateGestureState();

  if (state_ == GestureState::Pan) {
    prevPanPoint_ = {x, y};
  } else if (state_ == GestureState::Pinch) {
    prevPinchDist_ = pinchDistance();
  }
}

void GestureHandler::onTouchMove(int pointerId, float x, float y) {
  auto it = activePointers_.find(pointerId);
  if (it == activePointers_.end())
    return;
  it->second = {x, y};

  if (state_ == GestureState::Pan && activePointers_.size() == 1) {
    // Accumulate raw pixel deltas; heading-relative projection happens in applyToCamera.
    deltaPanX_ += x - prevPanPoint_.x;
    deltaPanY_ += y - prevPanPoint_.y;
    prevPanPoint_ = {x, y};

  } else if (state_ == GestureState::Pinch && activePointers_.size() >= 2) {
    float currentDist = pinchDistance();
    if (prevPinchDist_ > 1.0f && currentDist > 1.0f) {
      // Zoom in (fingers together): currentDist < prevPinchDist_ → ratio < 1 → altitude decreases.
      // Zoom out (fingers apart):   currentDist > prevPinchDist_ → ratio > 1 → altitude increases.
      double ratio = static_cast<double>(currentDist) / prevPinchDist_;
      deltaAltitudeScale_ *= ratio;
    }
    prevPinchDist_ = currentDist;

    // Two-finger rotation → heading change.
    auto iter = activePointers_.begin();
    const auto& p0 = iter->second;
    ++iter;
    const auto& p1 = iter->second;
    float angle = std::atan2(p1.y - p0.y, p1.x - p0.x);
    if (prevRotationAngle_ != 0.0f) {
      float angleDelta = angle - prevRotationAngle_;
      deltaHeading_ += glm::degrees(static_cast<double>(angleDelta));
    }
    prevRotationAngle_ = angle;
  }
}

void GestureHandler::onTouchUp(int pointerId) {
  activePointers_.erase(pointerId);
  updateGestureState();
  prevRotationAngle_ = 0.0f;
}

void GestureHandler::updateGestureState() {
  if (activePointers_.empty()) {
    state_ = GestureState::None;
  } else if (activePointers_.size() == 1) {
    state_ = GestureState::Pan;
    auto& pt = activePointers_.begin()->second;
    prevPanPoint_ = pt;
  } else {
    state_ = GestureState::Pinch;
    prevPinchDist_      = pinchDistance();
    prevRotationAngle_  = 0.0f;
  }
}

float GestureHandler::pinchDistance() const {
  if (activePointers_.size() < 2)
    return 0.0f;
  auto it = activePointers_.begin();
  const auto& a = it->second;
  ++it;
  const auto& b = it->second;
  float dx = b.x - a.x;
  float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void GestureHandler::applyToCamera(GlobeCamera& camera) const {
  auto params = camera.getParams();

  // ── Heading-relative pan ────────────────────────────────────────────────
  // Sensitivity scales with altitude so a fixed screen swipe always covers
  // a geographically proportional distance regardless of zoom level.
  if (deltaPanX_ != 0.0 || deltaPanY_ != 0.0) {
    const double sens   = params.altitude * kPanScale;
    const double H      = glm::radians(params.heading);
    const double cosH   = std::cos(H);
    const double sinH   = std::sin(H);
    const double latRad = glm::radians(params.latitude);

    // dy > 0: finger moved down → camera moves forward (in heading direction).
    // dx > 0: finger moved right → camera moves left (negative right direction).
    const double fwdAmount = deltaPanY_ * sens;
    const double rgtAmount = -deltaPanX_ * sens;

    // ENU: forward = (sinH, cosH) east/north; right = (cosH, -sinH) east/north.
    const double northDelta = cosH * fwdAmount + (-sinH) * rgtAmount;
    const double eastDelta  = sinH * fwdAmount +   cosH  * rgtAmount;

    params.latitude  += northDelta;
    params.longitude += eastDelta / std::max(std::cos(latRad), 0.01);
  }

  // ── Altitude (pinch) ────────────────────────────────────────────────────
  params.altitude *= deltaAltitudeScale_;

  // ── Heading (two-finger rotate) ─────────────────────────────────────────
  params.heading += deltaHeading_;

  // ── Clamp / wrap ────────────────────────────────────────────────────────
  params.latitude  = std::clamp(params.latitude, -90.0, 90.0);
  params.altitude  = std::clamp(params.altitude, 10.0, 100000000.0);
  params.heading   = std::fmod(params.heading, 360.0);
  if (params.heading < 0.0)
    params.heading += 360.0;

  camera.setParams(params);
}

void GestureHandler::resetDeltas() {
  deltaPanX_          = 0.0;
  deltaPanY_          = 0.0;
  deltaAltitudeScale_ = 1.0;
  deltaHeading_       = 0.0;
}

} // namespace reactnativecesium
