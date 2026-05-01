#pragma once

#include "GlobeCamera.hpp"

namespace reactnativecesium {

// Stateless helper. Given a current camera state and a target camera state,
// returns the next camera state for a frame of duration `dt` seconds.
//
// Per-DoF smoothing rules (matching the previous platform-specific code):
//   - lat / lon: copied directly from target (no smoothing — pan must feel
//     instant on touch).
//   - altitude / heading / pitch / roll / viewCorrection: exponential decay
//     `1 - exp(-k * dt)` per DoF with k taken from EngineTunables.
//   - When the delta drops below the matching epsilon, the value is snapped
//     to the target so we eventually reach exact equality.
class CameraSmoother {
public:
  static CameraParams step(const CameraParams& current,
                           const CameraParams& target,
                           double dt);
};

} // namespace reactnativecesium
