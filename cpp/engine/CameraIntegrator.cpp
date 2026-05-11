#include "CameraIntegrator.hpp"

#include "EngineTunables.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

namespace {

// WGS84 mean radius (m). Hard-coded so the integrator can be unit-tested in
// isolation without linking CesiumGeospatial. Matches Ellipsoid::WGS84.
inline constexpr double kEarthMeanRadiusM = 6371008.7714150595;

inline double shortAngleDeltaDeg(double from, double to) {
  return std::fmod(to - from + 540.0, 360.0) - 180.0;
}

inline double angleAbsDeltaDeg(double a, double b) {
  return std::abs(shortAngleDeltaDeg(a, b));
}

inline double wrap360(double deg) {
  double v = std::fmod(deg, 360.0);
  if (v < 0.0) v += 360.0;
  return v;
}

inline double clampAbs(double v, double maxAbs) {
  if (maxAbs <= 0.0) return v;
  if (v >  maxAbs) return  maxAbs;
  if (v < -maxAbs) return -maxAbs;
  return v;
}

inline double quatDotAbs(const glm::dquat& a, const glm::dquat& b) {
  return std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
}

// Resolve a runtime rate cap (caps.X) against the compile-time default.
// 0.0 means "no override" so we fall back to the EngineTunables value, which
// may itself be 0.0 (i.e. uncapped). Negative values are treated as 0.
inline double resolveCap(double rt, double dflt) {
  return rt > 0.0 ? rt : (dflt > 0.0 ? dflt : 0.0);
}

} // namespace

CameraIntegrator::CameraIntegrator(const CameraParams& initial) {
  teleport(initial);
}

// ── α-β core ──────────────────────────────────────────────────────────────

void CameraIntegrator::updateAlphaBeta(
    AlphaBeta& s, double z, TimePoint tz,
    double alpha, double beta, bool wrapAngle) {
  // Predict the value at the time the measurement arrived. On the very first
  // call (tState default-constructed = epoch) the predicted value is the
  // current state (velocity == 0), so the result is identical to a snap.
  const double dt   = std::chrono::duration<double>(tz - s.tState).count();
  const double pred = s.value + s.velocity * std::max(dt, 0.0);

  const double residual =
      wrapAngle ? shortAngleDeltaDeg(pred, z) : (z - pred);

  s.value = pred + alpha * residual;

  // Velocity update is gated on a meaningful time gap. Two reasons:
  //   - Bootstrap: a teleport(...) followed immediately by a setter in the
  //     same microtask has dt ≈ 0. Dividing β by that produces an absurd
  //     velocity estimate ("you climbed 100 m in zero seconds").
  //   - Same-tick burst writes (e.g. setAltitude + setHeading inside a
  //     single useAnimatedReaction tick): treat them as coincident updates
  //     to two independent DoFs, not as a sample of the rate of change.
  //
  // 5 ms is well below any plausible sensor sample interval (max useful
  // rate ~200 Hz) but comfortably above the coincident-call window. For
  // longer gaps we also floor dt to 20 ms when computing β/dt so a single
  // sub-frame measurement does not blow up the velocity estimate.
  constexpr double kVelocityMinDtSec   = 0.005;
  constexpr double kVelocityFloorDtSec = 0.020;
  if (dt > kVelocityMinDtSec) {
    const double dtForVel = std::max(dt, kVelocityFloorDtSec);
    s.velocity += (beta / dtForVel) * residual;
  }
  s.tState = tz;

  // Inter-arrival EWMA — used by the silence-aware velocity bleed in
  // extrapolate(). The first setter after a teleport seeds tLastMeas
  // without contributing to the EWMA (sentinel-epoch check). Sub-millisecond
  // gaps are also skipped so a same-tick burst of writes does not pull the
  // mean toward zero.
  if (s.tLastMeas.time_since_epoch().count() > 0) {
    const double interval = std::chrono::duration<double>(tz - s.tLastMeas).count();
    if (interval > 1e-3) {
      s.meanIntervalSec =
          (1.0 - tunables::kIntervalEwmaAlpha) * s.meanIntervalSec
          + tunables::kIntervalEwmaAlpha * interval;
    }
  }
  s.tLastMeas = tz;
}

void CameraIntegrator::extrapolate(AlphaBeta& s, TimePoint now, double maxRateAbs) {
  double dt = std::chrono::duration<double>(now - s.tState).count();
  if (dt <= 0.0) {
    s.tState = now;
    return;
  }
  if (dt > tunables::kMaxFrameDtSec) {
    dt = tunables::kMaxFrameDtSec;
  }

  // Silence-aware velocity bleed. Once measurements have stopped arriving
  // for this DoF (e.g. the user released a pan gesture, or the joystick
  // returned to centre), bleed the learned velocity exponentially. Both the
  // grace window and the decay τ scale with the EWMA of inter-arrival times
  // so a 60 Hz worklet bleeds in ~50 ms while a 1 Hz GPS glide is left
  // alone between fixes.
  if (s.tLastMeas.time_since_epoch().count() > 0) {
    const double silence = std::chrono::duration<double>(now - s.tLastMeas).count();
    const double grace   = tunables::kVelocitySilenceGraceFactor * s.meanIntervalSec;
    if (silence > grace) {
      const double tau = tunables::kVelocityBleedTauFactor * s.meanIntervalSec;
      if (tau > 1.0e-6) {
        s.velocity *= std::exp(-dt / tau);
      } else {
        s.velocity = 0.0;
      }
    }
  }

  if (maxRateAbs > 0.0) {
    s.velocity = clampAbs(s.velocity, maxRateAbs);
  }
  s.value += s.velocity * dt;
  s.tState = now;
}

// ── Demand setters ────────────────────────────────────────────────────────

void CameraIntegrator::setPosition(double latitudeDeg, double longitudeDeg) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  if (tunables::kOutlierLatLonDeg > 0.0) {
    if (std::abs(latitudeDeg  - lat_.value) > tunables::kOutlierLatLonDeg ||
        std::abs(shortAngleDeltaDeg(lon_.value, longitudeDeg))
            > tunables::kOutlierLatLonDeg) {
      return;
    }
  }

  updateAlphaBeta(lat_, latitudeDeg,  now,
                  tunables::kAlphaPos, tunables::kBetaPos, false);
  updateAlphaBeta(lon_, longitudeDeg, now,
                  tunables::kAlphaPos, tunables::kBetaPos, true);
  demand_.latitude  = latitudeDeg;
  demand_.longitude = longitudeDeg;
}

void CameraIntegrator::setAltitude(double altitudeMeters) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  if (tunables::kOutlierAltMeters > 0.0 &&
      std::abs(altitudeMeters - alt_.value) > tunables::kOutlierAltMeters) {
    return;
  }

  updateAlphaBeta(alt_, altitudeMeters, now,
                  tunables::kAlphaAlt, tunables::kBetaAlt, false);
  demand_.altitude = altitudeMeters;
}

void CameraIntegrator::setHeading(double headingDeg) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  if (tunables::kOutlierHeadingDeg > 0.0 &&
      angleAbsDeltaDeg(headingDeg, hdg_.value) > tunables::kOutlierHeadingDeg) {
    return;
  }

  updateAlphaBeta(hdg_, headingDeg, now,
                  tunables::kAlphaHdg, tunables::kBetaHdg, true);
  hdg_.value = wrap360(hdg_.value);
  demand_.heading = headingDeg;
}

void CameraIntegrator::setAttitude(double pitchDeg, double rollDeg) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  updateAlphaBeta(pitch_, pitchDeg, now,
                  tunables::kAlphaPitch, tunables::kBetaPitch, true);
  updateAlphaBeta(roll_,  rollDeg,  now,
                  tunables::kAlphaRoll,  tunables::kBetaRoll,  true);
  demand_.pitch = pitchDeg;
  demand_.roll  = rollDeg;
}

void CameraIntegrator::setViewCorrection(const glm::dquat& q) {
  std::lock_guard<std::mutex> g(mutex_);

  const double ql2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  if (ql2 < 1.0e-20) {
    viewCorrTarget_ = glm::dquat(1.0, 0.0, 0.0, 0.0);
  } else {
    const double inv = 1.0 / std::sqrt(ql2);
    viewCorrTarget_ = glm::dquat(q.w * inv, q.x * inv, q.y * inv, q.z * inv);
  }
  demand_.viewCorrection = viewCorrTarget_;
}

void CameraIntegrator::setVerticalFov(double degrees) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  const double residual = degrees - vfov_.value;
  vfov_.value   += tunables::kAlphaVfov * residual;
  vfov_.velocity = 0.0;
  vfov_.tState   = now;
}

void CameraIntegrator::teleport(const CameraParams& target) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  auto seed = [now](AlphaBeta& s, double v) {
    s.value           = v;
    s.velocity        = 0.0;
    s.tState          = now;
    s.tLastMeas       = TimePoint{}; // sentinel: bleed inactive until first setter
    s.meanIntervalSec = 0.1;
  };
  seed(lat_,   target.latitude);
  seed(lon_,   target.longitude);
  seed(alt_,   target.altitude);
  seed(hdg_,   wrap360(target.heading));
  seed(pitch_, target.pitch);
  seed(roll_,  target.roll);

  const glm::dquat& qIn = target.viewCorrection;
  const double ql2 = qIn.w*qIn.w + qIn.x*qIn.x + qIn.y*qIn.y + qIn.z*qIn.z;
  if (ql2 < 1.0e-20) {
    viewCorrTarget_ = viewCorrActual_ = glm::dquat(1.0, 0.0, 0.0, 0.0);
  } else {
    const double inv = 1.0 / std::sqrt(ql2);
    viewCorrTarget_ = viewCorrActual_ =
        glm::dquat(qIn.w * inv, qIn.x * inv, qIn.y * inv, qIn.z * inv);
  }

  seed(vfov_, target.verticalFov > 0.0 ? target.verticalFov : 0.0);

  demand_ = target;
  demand_.viewCorrection = viewCorrTarget_;
  if (demand_.verticalFov <= 0.0) {
    demand_.verticalFov = vfov_.value;
  }
}

CameraParams CameraIntegrator::step() {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> g(mutex_);

  // Resolve effective rate caps once per frame; runtime override beats default.
  const double yawCap   = resolveCap(caps_.maxYawRateDegSec,   tunables::kMaxYawRateDegSec);
  const double pitchCap = resolveCap(caps_.maxPitchRateDegSec, tunables::kMaxPitchRateDegSec);
  const double rollCap  = resolveCap(caps_.maxRollRateDegSec,  tunables::kMaxRollRateDegSec);
  const double climbCap = resolveCap(caps_.maxClimbRateMps,    tunables::kMaxClimbRateMps);

  // Convert ground-speed cap (m/s) to a per-DoF deg/s cap on latitude /
  // longitude. We use the WGS84 mean radius and ignore the cos(lat)
  // correction for the longitude term; this is a first-order approximation
  // but fine for a flight-sim rate cap, and avoids tangling the rate cap
  // with the current latitude (which would otherwise feed back into itself).
  double latLonCapDegSec = 0.0;
  const double groundCap = resolveCap(caps_.maxGroundSpeedMps, tunables::kMaxGroundSpeedMps);
  if (groundCap > 0.0) {
    latLonCapDegSec = glm::degrees(groundCap / kEarthMeanRadiusM);
  }

  extrapolate(lat_,   now, latLonCapDegSec);
  extrapolate(lon_,   now, latLonCapDegSec);
  extrapolate(alt_,   now, climbCap);
  extrapolate(hdg_,   now, yawCap);
  extrapolate(pitch_, now, pitchCap);
  extrapolate(roll_,  now, rollCap);
  extrapolate(vfov_,  now, 0.0); // no velocity term anyway

  hdg_.value = wrap360(hdg_.value);
  lat_.value = std::clamp(lat_.value, -90.0, 90.0);
  // Longitude is allowed to drift over ±180; consumers usually want the raw
  // continuous value for short-arc maths and the camera transform doesn't
  // care.

  // viewCorrection: SLERP toward the latest target.
  glm::dquat cur = glm::normalize(viewCorrActual_);
  glm::dquat tgt = glm::normalize(viewCorrTarget_);
  if (glm::dot(cur, tgt) < 0.0) tgt = -tgt;
  viewCorrActual_ = glm::normalize(
      glm::slerp(cur, tgt, tunables::kAlphaViewCorr));

  CameraParams out;
  out.latitude       = lat_.value;
  out.longitude      = lon_.value;
  out.altitude       = alt_.value;
  out.heading        = hdg_.value;
  out.pitch          = pitch_.value;
  out.roll           = roll_.value;
  out.verticalFov    = vfov_.value;
  out.viewCorrection = viewCorrActual_;
  return out;
}

void CameraIntegrator::clampActualAltitude(double newAltMsl) {
  std::lock_guard<std::mutex> g(mutex_);
  alt_.value    = newAltMsl;
  alt_.velocity = 0.0;
  // Do not touch demand_.altitude — getDemand() should still report what the
  // user asked for.
}

void CameraIntegrator::setRateCaps(const CameraRateCaps& caps) {
  std::lock_guard<std::mutex> g(mutex_);
  caps_ = caps;
}

CameraParams CameraIntegrator::getDemand() const {
  std::lock_guard<std::mutex> g(mutex_);
  return demand_;
}

CameraParams CameraIntegrator::getActual() const {
  std::lock_guard<std::mutex> g(mutex_);
  CameraParams out;
  out.latitude       = lat_.value;
  out.longitude      = lon_.value;
  out.altitude       = alt_.value;
  out.heading        = hdg_.value;
  out.pitch          = pitch_.value;
  out.roll           = roll_.value;
  out.verticalFov    = vfov_.value;
  out.viewCorrection = viewCorrActual_;
  return out;
}

bool CameraIntegrator::isActive() const {
  std::lock_guard<std::mutex> g(mutex_);
  if (std::abs(demand_.latitude  - lat_.value)  > tunables::kEpsLatLon)         return true;
  if (std::abs(shortAngleDeltaDeg(lon_.value, demand_.longitude))
        > tunables::kEpsLatLon)                                                  return true;
  if (std::abs(demand_.altitude  - alt_.value)  > tunables::kEpsAltitudeMeters) return true;
  if (angleAbsDeltaDeg(demand_.heading, hdg_.value) > tunables::kEpsAngleDeg)   return true;
  if (angleAbsDeltaDeg(demand_.pitch,   pitch_.value) > tunables::kEpsAngleDeg) return true;
  if (angleAbsDeltaDeg(demand_.roll,    roll_.value)  > tunables::kEpsAngleDeg) return true;
  if (std::abs(demand_.verticalFov - vfov_.value) > tunables::kEpsAngleDeg)     return true;
  if (std::abs(lat_.velocity)   > tunables::kEpsLatLon         * 60.0)          return true;
  if (std::abs(lon_.velocity)   > tunables::kEpsLatLon         * 60.0)          return true;
  if (std::abs(alt_.velocity)   > tunables::kEpsAltitudeMeters * 60.0)          return true;
  if (std::abs(hdg_.velocity)   > tunables::kEpsAngleDeg       * 60.0)          return true;
  if (std::abs(pitch_.velocity) > tunables::kEpsAngleDeg       * 60.0)          return true;
  if (std::abs(roll_.velocity)  > tunables::kEpsAngleDeg       * 60.0)          return true;
  if ((1.0 - quatDotAbs(viewCorrActual_, viewCorrTarget_))
        > tunables::kEpsQuatDot)                                                return true;
  return false;
}

} // namespace reactnativecesium
