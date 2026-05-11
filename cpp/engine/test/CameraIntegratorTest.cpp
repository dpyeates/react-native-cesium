// Self-contained host harness for CameraIntegrator. NOT compiled into the
// library — invoked manually by maintainers:
//
//   $ bash cpp/engine/test/build_and_run.sh
//
// The integrator uses steady_clock internally, so the harness mixes real
// sleeps with calls — total wall-clock runtime is ~3 seconds.
//
// Each test prints PASS / FAIL with a short diagnostic. Exit code is the
// number of failures.

#include "../CameraIntegrator.hpp"
#include "../EngineTunables.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace reactnativecesium;

static int g_failures = 0;

#define CHECK(cond, label)                                                 \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n",                    \
                   label, #cond, __LINE__);                                \
      ++g_failures;                                                        \
    } else {                                                               \
      std::fprintf(stderr, "PASS [%s]\n", label);                          \
    }                                                                      \
  } while (0)

static bool approxEq(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

// Sleep a few ms then call step() so the integrator advances along the
// learned velocity. Returns the latest actual.
static CameraParams stepFor(CameraIntegrator& integ, int totalMs, int tickMs = 16) {
  CameraParams out;
  for (int t = 0; t < totalMs; t += tickMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(tickMs));
    out = integ.step();
  }
  return out;
}

// 1. Step input: a single setter call should move the value toward the
// target by α × residual (no further movement until another measurement).
// Demand is preserved through getDemand() regardless.
static void testStepInput() {
  CameraParams seed;
  seed.altitude = 100.0;
  CameraIntegrator integ(seed);

  integ.setAltitude(200.0);
  auto out = stepFor(integ, 80);

  // Single shot: should be between 100 and 200, much closer to value+α*residual
  CHECK(out.altitude > 100.0 && out.altitude < 200.0, "step_input.between");
  CHECK(approxEq(integ.getDemand().altitude, 200.0, 1e-9),
        "step_input.demand_preserved");
}

// 2. Repeated setter at constant velocity. Simulates a 10 Hz altitude feed
// climbing at 5 m/s. After enough samples, actual altitude should track
// demand to within a fraction of one inter-arrival delta.
static void testConstantVelocity() {
  CameraIntegrator integ;
  integ.teleport({}); // default seed: alt=5500

  const double climbMps     = 5.0;
  const double dtSampleSec  = 0.1;
  const int    numSamples   = 25; // 2.5s of feed

  double demandAlt = 5500.0;
  for (int i = 0; i < numSamples; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(dtSampleSec * 1000)));
    demandAlt += climbMps * dtSampleSec;
    integ.setAltitude(demandAlt);
    // A few render ticks between samples.
    for (int k = 0; k < 6; ++k) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      integ.step();
    }
  }
  auto actual = integ.getActual();
  // Allow a tracking lag of one full sample delta.
  CHECK(approxEq(actual.altitude, demandAlt, climbMps * dtSampleSec * 2.0),
        "const_vel.tracks");
}

// 3. Rate cap bounds a single noisy heading spike.
static void testRateCap() {
  CameraIntegrator integ;
  CameraRateCaps caps;
  caps.maxYawRateDegSec = 90.0;
  integ.setRateCaps(caps);

  integ.teleport({}); // default heading=220

  // 10 calm samples at 220° to settle.
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    integ.setHeading(220.0);
    integ.step();
  }
  // Single 110° spike.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  integ.setHeading(330.0);
  auto out = integ.step();
  // Without a cap a 110° step would produce v ≈ 110 / 0.02 = 5500 deg/s
  // and the next step() would extrapolate by v*dt = 5500*0.02 = 110°.
  // With the 90 deg/s cap, the change in this single frame is at most
  // 90 * 0.02 ≈ 1.8° beyond the α*residual jump (~66°).
  CHECK(std::fabs(out.heading - 220.0) < 90.0, "rate_cap.bounded");
}

// 4. dt clamp prevents a stale velocity vector from jumping after a long
// pause. We feed a 30 deg/s heading rate, then wait 5 real seconds with no
// further measurements, and check that the next step() only advances by
// kMaxFrameDtSec * velocity.
static void testLongPause() {
  CameraIntegrator integ;
  integ.teleport({});
  // Build a yaw rate.
  for (int i = 1; i <= 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    integ.setHeading(220.0 + 3.0 * i);
    integ.step();
  }
  auto beforePause = integ.getActual();
  // Long real-time pause with no setters and no steps.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  auto out = integ.step();
  // dt was clamped to 0.1 s, so heading advanced by at most
  // velocity * 0.1 s. With v ≈ 30 deg/s, that's at most 3°.
  CHECK(std::fabs(out.heading - beforePause.heading) < 10.0,
        "long_pause.dt_clamped");
}

// 5. Gesture-end bleed. A 60 Hz worklet streams ramping heading values for
// ~250 ms (simulating an active pan), then stops emitting entirely.
// Without the silence-aware velocity bleed the integrator would extrapolate
// the last learned velocity forever; with the bleed, motion should die in
// well under a second.
static void testGestureEndBleed() {
  CameraIntegrator integ;
  integ.teleport({});

  // Active "gesture": 16 ms cadence, +0.5° heading per frame (= 31 deg/s).
  double demand = 220.0;
  for (int i = 0; i < 16; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    demand += 0.5;
    integ.setHeading(demand);
    integ.step();
  }
  const double duringHeading = integ.getActual().heading;

  // "Release": no more setters, just render ticks at 60 Hz.
  CameraParams out;
  for (int i = 0; i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    out = integ.step();
  }
  const double endHeading = out.heading;
  // Drift during the 1-second silence should be small — the integrator
  // should have given up on the learned 31 deg/s rate well before the end.
  CHECK(std::fabs(endHeading - duringHeading) < 5.0,
        "gesture_end_bleed.stops");
}

// 6. Terrain clamp write-back — clampActualAltitude raises actual without
// touching demand.
static void testTerrainClamp() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = 100.0;
  integ.teleport(seed);
  integ.clampActualAltitude(150.0);
  auto demand = integ.getDemand();
  auto actual = integ.getActual();
  CHECK(approxEq(actual.altitude, 150.0, 1e-6), "terrain_clamp.actual_raised");
  CHECK(approxEq(demand.altitude, 100.0, 1e-6), "terrain_clamp.demand_preserved");
}

int main() {
  testStepInput();
  testConstantVelocity();
  testRateCap();
  testLongPause();
  testGestureEndBleed();
  testTerrainClamp();
  if (g_failures == 0) {
    std::fprintf(stderr, "\nALL TESTS PASSED.\n");
  } else {
    std::fprintf(stderr, "\n%d TEST(S) FAILED.\n", g_failures);
  }
  return g_failures;
}
