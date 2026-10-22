// Host check for the EGM96 MSL <-> ellipsoid conversion.
//
//   clang++ -std=c++20 -I cpp -o /tmp/geoid cpp/engine/test/GeoidConverterTest.cpp \
//     cpp/engine/GeoidConverter.cpp cpp/third_party/egm96/EGM96.c
//
// Or: bash cpp/engine/test/build_and_run.sh

#include "../GeoidConverter.hpp"

#include <cmath>
#include <cstdio>

using namespace reactnativecesium;

static int g_failures = 0;

static void check(bool ok, const char* label) {
  if (!ok) {
    std::fprintf(stderr, "FAIL [%s]\n", label);
    ++g_failures;
    return;
  }
  std::fprintf(stderr, "PASS [%s]\n", label);
}

static void checkInverse(const char* label, double lat, double lon, double msl) {
  const double hae = mslToEllipsoidMeters(lat, lon, msl);
  const double back = ellipsoidToMslMeters(lat, lon, hae);
  const double undulation = hae - msl;
  if (!std::isfinite(hae) || !std::isfinite(back) || std::fabs(back - msl) > 1e-6) {
    std::fprintf(stderr, "FAIL [%s]: msl=%.3f hae=%.3f back=%.3f\n", label, msl, hae, back);
    ++g_failures;
    return;
  }
  if (std::fabs(undulation) < 1.0) {
    std::fprintf(stderr, "FAIL [%s]: undulation %.3f m is unexpectedly ~0\n", label, undulation);
    ++g_failures;
    return;
  }
  std::fprintf(stderr, "PASS [%s] undulation %.2f m\n", label, undulation);
}

static void checkDemandRoundTrip(const char* label, double lat, double lon, double msl) {
  const double hae = mslToDemandAltitudeMeters(lat, lon, msl, true);
  const double back = demandAltitudeToMslMeters(lat, lon, hae, true);
  check(std::isfinite(hae) && std::isfinite(back) && std::fabs(back - msl) < 1e-6, label);

  const double mslIdentity = demandAltitudeToMslMeters(lat, lon, msl, false);
  const double haeIdentity = mslToDemandAltitudeMeters(lat, lon, msl, false);
  check(mslIdentity == msl && haeIdentity == msl, "datum_msl_identity");
}

int main() {
  // UK geoid is about +45 m. Alps about +50 m. Indian Ocean is negative.
  checkInverse("uk", 51.5, -0.1, 1000.0);
  checkInverse("alps", 46.0, 8.0, 2000.0);
  checkInverse("indian_ocean", -10.0, 80.0, 50.0);

  const double nan = std::nan("");
  check(ellipsoidToMslMeters(nan, 0.0, 10.0) == 10.0, "ellipsoid_non_finite");
  check(mslToEllipsoidMeters(nan, 0.0, 10.0) == 10.0, "msl_non_finite");

  checkDemandRoundTrip("demand_round_trip", 51.5, -0.1, 1000.0);

  constexpr double kUkLat = 51.5;
  constexpr double kUkLon = -0.1;
  constexpr double kOceanLat = -10.0;
  constexpr double kOceanLon = 80.0;
  constexpr double kMsl = 1000.0;
  const double hae = mslToEllipsoidMeters(kUkLat, kUkLon, kMsl);
  const double moved = mslAfterHorizontalMove(kUkLat, kUkLon, kMsl, kOceanLat, kOceanLon);
  const double movedHae = mslToEllipsoidMeters(kOceanLat, kOceanLon, moved);
  check(std::isfinite(moved) && std::fabs(movedHae - hae) < 1e-6, "move_preserves_hae");
  check(std::fabs(moved - kMsl) > 1.0, "move_changes_msl");

  return g_failures == 0 ? 0 : 1;
}
