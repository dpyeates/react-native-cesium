#include "GeoidConverter.hpp"

#include "../third_party/egm96/EGM96.h"

#include <cmath>
#include <limits>

namespace reactnativecesium {

namespace {

double normalizeLongitudeDegrees(double longitudeDeg) {
  double wrapped = std::fmod(longitudeDeg + 180.0, 360.0);
  if (wrapped < 0.0) wrapped += 360.0;
  return wrapped - 180.0;
}

// Geoid undulation N, where HAE = MSL + N. NaN when the position or the
// model result is non-finite; callers then return the input height unchanged.
double geoidUndulationMeters(double latitudeDeg, double longitudeDeg) {
  if (!std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double undulation = egm96_compute_altitude_offset(
      latitudeDeg, normalizeLongitudeDegrees(longitudeDeg));
  if (!std::isfinite(undulation)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return undulation;
}

double applyUndulation(double heightMeters,
                       double latitudeDeg,
                       double longitudeDeg,
                       double sign) {
  if (!std::isfinite(heightMeters)) return heightMeters;
  const double undulation = geoidUndulationMeters(latitudeDeg, longitudeDeg);
  if (!std::isfinite(undulation)) return heightMeters;
  return heightMeters + sign * undulation;
}

} // namespace

double mslToEllipsoidMeters(double latitudeDeg,
                            double longitudeDeg,
                            double mslMeters) {
  return applyUndulation(mslMeters, latitudeDeg, longitudeDeg, 1.0);
}

double ellipsoidToMslMeters(double latitudeDeg,
                            double longitudeDeg,
                            double ellipsoidMeters) {
  return applyUndulation(ellipsoidMeters, latitudeDeg, longitudeDeg, -1.0);
}

} // namespace reactnativecesium
