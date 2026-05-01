#include "GeoidConverter.hpp"

#include "../third_party/egm96/EGM96.h"

#include <cmath>

namespace reactnativecesium {

namespace {

double normalizeLongitudeDegrees(double longitudeDeg) {
  if (!std::isfinite(longitudeDeg)) return 0.0;
  double wrapped = std::fmod(longitudeDeg + 180.0, 360.0);
  if (wrapped < 0.0) wrapped += 360.0;
  return wrapped - 180.0;
}

} // namespace

double mslToEllipsoidMeters(double latitudeDeg,
                            double longitudeDeg,
                            double mslMeters) {
  if (!std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg) ||
      !std::isfinite(mslMeters)) {
    return mslMeters;
  }

  const double normalizedLon = normalizeLongitudeDegrees(longitudeDeg);
  const double geoidOffset =
      egm96_compute_altitude_offset(latitudeDeg, normalizedLon);

  if (!std::isfinite(geoidOffset)) return mslMeters;
  return mslMeters + geoidOffset;
}

} // namespace reactnativecesium
