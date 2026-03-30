#pragma once

#include "../engine/GlobeCamera.hpp"

#include <array>
#include <cstdint>

namespace reactnativecesium {

struct TouchPoint {
  float x = 0;
  float y = 0;
};

enum class GestureState { None, Pan, Pinch, Rotate };

class GestureHandler {
public:
  void setViewportSize(float width, float height);

  void onTouchDown(int pointerId, float x, float y);
  void onTouchMove(int pointerId, float x, float y);
  void onTouchUp(int pointerId);

  void applyToCamera(GlobeCamera& camera) const;

  void resetDeltas();

private:
  void updateGestureState();
  float pinchDistance() const;
  int   findTouch(int id) const; // returns index or -1

  float viewportWidth_ = 1.0f;
  float viewportHeight_ = 1.0f;

  // Fixed-size compact array of active touches.  Removal swaps with last entry
  // so the first kMaxTouches slots are always densely packed.  Crucially, the
  // relative order of the first two entries is stable within a pinch gesture,
  // which avoids the non-deterministic ordering that std::unordered_map can
  // produce after rehashing.
  static constexpr int kMaxTouches = 10;
  struct ActiveTouch { int id = -1; TouchPoint pt{}; };
  std::array<ActiveTouch, kMaxTouches> touches_{};
  int touchCount_ = 0;

  GestureState state_ = GestureState::None;

  float prevPinchDist_ = 0.0f;
  TouchPoint prevPanPoint_{};
  float prevRotationAngle_ = 0.0f;

  // Accumulated deltas per frame (raw pixels for pan; others in their natural units)
  double deltaPanX_          = 0.0; // rightward screen pixels
  double deltaPanY_          = 0.0; // downward screen pixels
  double deltaAltitudeScale_ = 1.0;
  double deltaHeading_       = 0.0;
};

} // namespace reactnativecesium
