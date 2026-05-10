#pragma once

#include "GlobeCamera.hpp"

namespace reactnativecesium {

// Stateless helper. Given a current camera state and a target camera state,
// returns the next camera state for a frame of duration `dt` seconds.
//
// lat / lon / altitude / heading are snapped directly to the target.
// CameraTargetState::snapshot() has already linearly interpolated those DoFs
// to their "current" position based on each DoF's own EWMA inter-arrival
// interval, so snapping here gives constant-velocity motion between fixes
// with no ease-in/ease-out. At gesture rates (~60 Hz) the EWMA collapses to
// ~16 ms so the snap is indistinguishable from the previous behaviour.
//
// pitch / roll / viewCorrection use fixed-rate exponential decay
// `1 - exp(-k * dt)` with k from EngineTunables — appropriate for high-rate
// attitude sensors where the ease-out feel is desirable.
// Once a DoF drops below its matching epsilon it is snapped to exact equality.
class CameraSmoother {
public:
  static CameraParams step(const CameraParams& current,
                           const CameraParams& target,
                           double dt);
};

} // namespace reactnativecesium
