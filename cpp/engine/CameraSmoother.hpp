#pragma once

#include "GlobeCamera.hpp"

namespace reactnativecesium {

// Stateless helper. Given a current camera state and a target camera state,
// returns the next camera state for a frame of duration `dt` seconds.
//
// Per-DoF smoothing rules:
//   - lat / lon: adaptive exponential smoothing `1 - exp(-dt/τ)` where
//     τ = clamp(α · recentIntervalSec, τmin, τmax). High update rates (60 Hz
//     gestures) drive τ to the floor so the camera tracks the finger without
//     perceptible lag; sparse updates (1 Hz GPS) widen τ so each sample
//     interpolates smoothly across its interval. Snap-through guard for
//     genuine teleports keeps cross-planet `setCamera` jumps instant.
//   - altitude / heading / pitch / roll / viewCorrection: exponential decay
//     `1 - exp(-k * dt)` per DoF with k taken from EngineTunables.
//   - When the delta drops below the matching epsilon, the value is snapped
//     to the target so we eventually reach exact equality.
class CameraSmoother {
public:
  static CameraParams step(const CameraParams& current,
                           const CameraParams& target,
                           double dt,
                           double recentIntervalSec);
};

} // namespace reactnativecesium
