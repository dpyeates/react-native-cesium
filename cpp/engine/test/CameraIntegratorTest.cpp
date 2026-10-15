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
#include "../TerrainHeight.hpp"

#include <algorithm>
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

static void followFloors(const double* floors, int count, const char* label,
                         bool expectRiseThenFall) {
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = floors[0];
  integ.teleport(seed);
  // A demand under every floor, plus a downward velocity sample, must not
  // pull the rendered camera through the surface.
  integ.setAltitude(0.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  integ.setAltitude(0.0);

  double prev = 0.0;
  bool havePrev = false;
  bool sawPeak = false;
  for (int i = 0; i < count; ++i) {
    const double floor = floors[i];
    integ.setAltitude(0.0);
    integ.setTerrainFloor(floor);
    const CameraParams actual = integ.step();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s.exact_%d", label, i);
    CHECK(approxEq(actual.altitude, floor, 1e-6), buf);
    CHECK(approxEq(integ.getDemand().altitude, 0.0, 1e-9), label);
    if (havePrev) {
      if (!expectRiseThenFall) {
        std::snprintf(buf, sizeof(buf), "%s.monotonic_%d", label, i);
        if (floors[i] > floors[i - 1]) {
          CHECK(actual.altitude > prev - 1e-6, buf);
        } else {
          CHECK(actual.altitude < prev + 1e-6, buf);
        }
      }
      if (expectRiseThenFall && floors[i] < floors[i - 1]) {
        sawPeak = true;
        std::snprintf(buf, sizeof(buf), "%s.descends_%d", label, i);
        CHECK(actual.altitude < prev - 1e-6, buf);
      }
      if (expectRiseThenFall && sawPeak && floors[i] > floors[i - 1]) {
        std::snprintf(buf, sizeof(buf), "%s.climbs_%d", label, i);
        CHECK(actual.altitude > prev + 1e-6, buf);
      }
    }
    prev = actual.altitude;
    havePrev = true;
  }
}

static void testUphillContact() {
  const double floors[] = {100.0, 110.0, 120.0, 130.0};
  followFloors(floors, 4, "uphill", false);
}

static void testDownhillContact() {
  const double floors[] = {130.0, 120.0, 110.0, 100.0};
  followFloors(floors, 4, "downhill", false);
}

static void testCrest() {
  const double floors[] = {100.0, 110.0, 120.0, 110.0, 100.0};
  followFloors(floors, 5, "crest", true);
}

static void testValley() {
  const double floors[] = {120.0, 110.0, 100.0, 110.0, 120.0};
  followFloors(floors, 5, "valley", true);

  // Descent velocity must not carry the camera into the rising face.
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = 100.0;
  integ.teleport(seed);
  integ.clearTerrainFloor();
  integ.setAltitude(0.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  integ.setAltitude(0.0);
  CHECK(integ.getActual().altitude < 100.0, "valley.velocity_descended");
  integ.setTerrainFloor(110.0);
  CHECK(approxEq(integ.getActual().altitude, 110.0, 1e-6), "valley.climb_exact");
  CHECK(approxEq(integ.step().altitude, 110.0, 1e-6), "valley.climb_holds");
}

static void testSetAltitudeUnderFloor() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = 100.0;
  integ.teleport(seed);
  integ.setTerrainFloor(150.0);
  integ.setAltitude(10.0);
  CHECK(approxEq(integ.getActual().altitude, 150.0, 1e-6), "under_floor.actual");
  CHECK(approxEq(integ.getDemand().altitude, 10.0, 1e-9), "under_floor.demand");
  const CameraParams stepped = integ.step();
  CHECK(approxEq(stepped.altitude, 150.0, 1e-6), "under_floor.after_step");
}

static void testIdleOnFloor() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = 100.0;
  integ.teleport(seed);
  integ.setTerrainFloor(150.0);
  integ.setAltitude(40.0);
  const CameraParams stepped = integ.step();
  CHECK(approxEq(stepped.altitude, 150.0, 1e-6), "idle_floor.actual");
  CHECK(!integ.isActive(), "idle_floor.inactive");
}

static void testFlyingAboveFloor() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.altitude = 500.0;
  integ.teleport(seed);
  integ.setTerrainFloor(100.0);
  const CameraParams stepped = integ.step();
  CHECK(approxEq(stepped.altitude, 500.0, 1e-3), "above_floor.untouched");
  CHECK(approxEq(integ.getDemand().altitude, 500.0, 1e-9), "above_floor.demand");
}

static double wrap180(double deg) {
  double v = std::fmod(deg + 180.0, 360.0);
  if (v < 0.0) v += 360.0;
  return v - 180.0;
}

static void testHeadingShortPull() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.heading = 350.0;
  integ.teleport(seed);
  integ.setHeading(10.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  bool onShortArc = true;
  for (int i = 0; i < 90; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double h = integ.step().heading;
    if (!(h >= 330.0 || h <= 40.0)) onShortArc = false;
  }
  CHECK(onShortArc, "heading_pull.short_arc");
  const double end = integ.getActual().heading;
  const double err = std::abs(wrap180(end - 10.0));
  CHECK(err < 2.0, "heading_pull.converged");
}

static void testLongitudeShortPull() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.longitude = 179.0;
  integ.teleport(seed);
  integ.setPosition(seed.latitude, -179.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  bool onShortArc = true;
  for (int i = 0; i < 90; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double wrapped = wrap180(integ.step().longitude);
    if (!(std::abs(wrapped) > 90.0)) onShortArc = false;
  }
  CHECK(onShortArc, "lon_pull.short_arc");
  const double err = std::abs(wrap180(integ.getActual().longitude - (-179.0)));
  CHECK(err < 2.0, "lon_pull.converged");
}

static void testVerticalFovConverges() {
  CameraIntegrator integ;
  CameraParams seed;
  seed.verticalFov = 60.0;
  integ.teleport(seed);
  integ.setVerticalFov(30.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(16));
  const double first = integ.step().verticalFov;
  CHECK(first < 59.0 && first > 40.0, "vfov.first_step_eases");
  CameraParams mid;
  for (int i = 0; i < 80; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    mid = integ.step();
  }
  CHECK(approxEq(mid.verticalFov, 30.0, 0.5), "vfov.converged");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const double held = integ.step().verticalFov;
  CHECK(approxEq(held, 30.0, 0.5), "vfov.stays");
}

static void testLeadAndTriangle() {
  float out = 0.0f;
  CHECK(resolveSurfaceEllipsoid(true, 100.0f, true, 90.0f, out) && out == 100.0f,
        "lead.crest_no_sticky_peak");
  CHECK(resolveSurfaceEllipsoid(true, 100.0f, true, 110.0f, out) && out == 110.0f,
        "lead.valley_raises");
  CHECK(resolveSurfaceEllipsoid(true, 120.0f, false, 0.0f, out) && out == 120.0f,
        "lead.nadir_only");
  CHECK(!resolveSurfaceEllipsoid(false, 0.0f, false, 0.0f, out), "lead.neither");

  double lat = 0.0, lon = 0.0;
  const bool slew = terrainLeadLatLon(0.0, 1.0, 0.0, 0.0, true, lat, lon);
  CHECK(!slew, "lead.slew_ignored");

  const bool step = terrainLeadLatLon(0.0, 0.0001, 0.0, 0.0, true, lat, lon);
  CHECK(step, "lead.one_frame");
  CHECK(approxEq(lat, 0.0, 1e-12), "lead.lat");
  CHECK(approxEq(lon, 0.0002, 1e-9), "lead.lon_ahead");

  float h = -1.0f;
  const bool hit = horizontalTriangleHeight(
      2.0, 2.0,
      0.0, 0.0, 0.0f,
      10.0, 0.0, 0.0f,
      0.0, 10.0, 30.0f,
      h);
  CHECK(hit && approxEq(h, 6.0, 1e-3), "tri.interpolated");

  float outside = 0.0f;
  CHECK(!horizontalTriangleHeight(
            20.0, 20.0,
            0.0, 0.0, 0.0f,
            10.0, 0.0, 0.0f,
            0.0, 10.0, 30.0f,
            outside),
        "tri.outside");

  float skirt = 0.0f;
  CHECK(!horizontalTriangleHeight(
            1.0, 0.0,
            0.0, 0.0, 100.0f,
            0.0, 0.0, 0.0f,
            5.0, 0.0, 100.0f,
            skirt),
        "tri.skirt_rejected");
  // 100 m edge, 1 mm of horizontal width: a float-error skirt, not terrain.
  CHECK(!horizontalTriangleHeight(
            0.0, 0.0005,
            0.0, 0.0, 100.0f,
            100.0, 0.0, 100.0f,
            0.0, 0.001, 0.0f,
            skirt),
        "tri.thin_skirt_rejected");

  float low = 0.0f, high = 0.0f;
  const bool lowHit = horizontalTriangleHeight(
      2.0, 2.0,
      0.0, 0.0, 5.0f,
      10.0, 0.0, 5.0f,
      0.0, 10.0, 5.0f,
      low);
  const bool highHit = horizontalTriangleHeight(
      2.0, 2.0,
      0.0, 0.0, 20.0f,
      10.0, 0.0, 20.0f,
      0.0, 10.0, 20.0f,
      high);
  CHECK(lowHit && highHit && std::max(low, high) == 20.0f, "tri.overlap_keeps_higher");
}

int main() {
  testStepInput();
  testConstantVelocity();
  testRateCap();
  testLongPause();
  testGestureEndBleed();
  testTerrainClamp();
  testUphillContact();
  testDownhillContact();
  testCrest();
  testValley();
  testSetAltitudeUnderFloor();
  testIdleOnFloor();
  testFlyingAboveFloor();
  testHeadingShortPull();
  testLongitudeShortPull();
  testVerticalFovConverges();
  testLeadAndTriangle();
  if (g_failures == 0) {
    std::fprintf(stderr, "\nALL TESTS PASSED.\n");
  } else {
    std::fprintf(stderr, "\n%d TEST(S) FAILED.\n", g_failures);
  }
  return g_failures;
}
