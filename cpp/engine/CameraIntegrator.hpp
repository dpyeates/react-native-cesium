#pragma once

#include "GlobeCamera.hpp"

#include <chrono>
#include <mutex>

namespace reactnativecesium {

// Runtime-tunable per-DoF rate caps and outlier thresholds. Mirrors the
// compile-time defaults in EngineTunables but exposed as a struct so a view
// prop can override them at runtime without recompiling.
struct CameraRateCaps {
  double maxYawRateDegSec   = 0.0;
  double maxPitchRateDegSec = 0.0;
  double maxRollRateDegSec  = 0.0;
  double maxClimbRateMps    = 0.0;
  double maxGroundSpeedMps  = 0.0;
};

// Single owner of the camera's per-DoF α-β tracker state. Measurements are
// written from any thread (typically a Reanimated worklet on the UI thread or
// a GPS / IMU callback on the JS thread); the render thread ticks step() once
// per frame and reads the new actual camera back out.
//
// Internally every scalar DoF (latitude, longitude, altitude, heading, pitch,
// roll, verticalFov) has independent (value, velocity, t_state) state. On a
// measurement the residual is computed against the predicted value at the
// arrival time and split between value (scaled by α) and velocity
// (scaled by β / dt_arrival). On step() the value is extrapolated forward
// using the running velocity, clamped against the rate cap for that DoF.
//
// viewCorrection is a unit quaternion with no useful velocity model; it uses
// a single-coefficient SLERP toward the latest demand each frame.
//
// All public methods take `mutex_` briefly — contention is uncontested in
// practice (writers are sub-microsecond and the render thread reads once per
// frame). The integrator owns its own clock (Clock::now() inside both the
// setters and step()), so no platform time-base needs to be threaded in.
class CameraIntegrator {
public:
  using Clock     = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit CameraIntegrator(const CameraParams& initial = {});

  // ── Per-DoF demand setters ──────────────────────────────────────────────
  // Each call stamps the measurement with the current steady_clock now() and
  // performs one α-β update against the predicted value at that time.
  void setPosition(double latitudeDeg, double longitudeDeg);
  void setAltitude(double altitudeMeters);
  void setHeading(double headingDeg);
  void setAttitude(double pitchDeg, double rollDeg);
  void setViewCorrection(const glm::dquat& q);
  void setVerticalFov(double degrees);

  // Atomically reset every DoF to `target` — value, velocity (0), demand,
  // all timestamps. Used for initial seeding and for explicit scripted
  // scene jumps; bypasses the trackers entirely.
  void teleport(const CameraParams& target);

  // Render-thread tick. Extrapolates every DoF forward using its current
  // velocity, applies rate caps, then returns the new actual camera. dt is
  // clamped internally to kMaxFrameDtSec so a long sleep can't catapult the
  // camera along a stale velocity vector.
  //
  // The wall-clock used to compute dt is Clock::now() (steady_clock) — the
  // same clock the setters stamp with. This is deliberate: the integrator
  // is the single owner of the time base. Earlier versions accepted an
  // externally supplied `now` from the platform render loop, but on iOS
  // Simulator `CACurrentMediaTime()` and `std::chrono::steady_clock` do
  // not share an epoch, so mixing them produced multi-thousand-second dt
  // values and an instant runaway.
  CameraParams step();

  // Terrain-floor write-back hook. Replaces the *actual* altitude value with
  // `newAltMsl` and zeroes vertical velocity so the integrator does not
  // immediately try to descend back through the floor. The *demand* altitude
  // is left untouched — getDemand().altitude continues to reflect what the
  // consumer asked for.
  void clampActualAltitude(double newAltMsl);

  // Runtime override of the rate caps (driven by view props). 0 = uncapped
  // for a given axis. The defaults match tunables::kMax*.
  void setRateCaps(const CameraRateCaps& caps);

  // ── Readback ────────────────────────────────────────────────────────────

  // What the consumer last asked for (each DoF independently). This is the
  // unfiltered demand: pre-α-β. Useful for HUDs that want to display the
  // requested camera rather than the rendered one.
  CameraParams getDemand() const;

  // The most recent value of step() (or the initial seed if step() has not
  // run yet). Equivalent to what is currently being rendered.
  CameraParams getActual() const;

  // True when there is meaningful work to do: any residual between actual
  // and demand exceeds its epsilon, or any DoF has non-zero velocity. The
  // render loop can use this to drop back to idle when the camera is at
  // rest.
  bool isActive() const;

private:
  // Per-DoF α-β state. `value` and `velocity` are the running estimates,
  // `tState` the time of the last value update. `tLastMeas` is the time of
  // the most recent setter call for this DoF (sentinel epoch == "no
  // measurement seen yet"); `meanIntervalSec` is an EWMA of inter-arrival
  // times used to drive the silence-aware velocity bleed.
  struct AlphaBeta {
    double    value           = 0.0;
    double    velocity        = 0.0;
    TimePoint tState{};
    TimePoint tLastMeas{};
    double    meanIntervalSec = 0.1; // seed: ~10 Hz, irrelevant after a few setters
  };

  // Apply one α-β measurement update. `wrapAngle` is true for the angular
  // DoFs (heading, pitch, roll, longitude) so the residual is computed on
  // the shortest signed arc.
  static void updateAlphaBeta(AlphaBeta& s, double z, TimePoint tz,
                              double alpha, double beta, bool wrapAngle);

  // Advance the running estimate to `now`, clamp velocity to `maxRateAbs`
  // (per second; 0 = uncapped). For angles, the resulting value is also
  // normalised to its canonical range by the caller as needed.
  static void extrapolate(AlphaBeta& s, TimePoint now, double maxRateAbs);

  mutable std::mutex mutex_;

  AlphaBeta lat_, lon_, alt_;
  AlphaBeta hdg_, pitch_, roll_;
  AlphaBeta vfov_; // velocity always zero — α-only

  // Quaternion SLERP target + most recent actual (rendered) value.
  glm::dquat viewCorrTarget_{1.0, 0.0, 0.0, 0.0};
  glm::dquat viewCorrActual_{1.0, 0.0, 0.0, 0.0};

  // Demand mirrors (raw last-set values, no α-β filter applied).
  CameraParams demand_{};

  // Runtime overrides; 0 = use compile-time defaults from EngineTunables.
  CameraRateCaps caps_{};
};

} // namespace reactnativecesium
