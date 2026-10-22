#pragma once

namespace reactnativecesium {

/// Converts orthometric height (MSL) to ellipsoidal height (HAE, WGS84).
double mslToEllipsoidMeters(double latitudeDeg,
                            double longitudeDeg,
                            double mslMeters);

/// Converts WGS84 ellipsoidal height (HAE) to orthometric height (MSL).
double ellipsoidToMslMeters(double latitudeDeg,
                            double longitudeDeg,
                            double ellipsoidMeters);

/// `ellipsoidHeight` selects HAE→MSL. Otherwise `altitudeMeters` is already MSL.
inline double demandAltitudeToMslMeters(double latitudeDeg,
                                        double longitudeDeg,
                                        double altitudeMeters,
                                        bool ellipsoidHeight) {
  if (!ellipsoidHeight) return altitudeMeters;
  return ellipsoidToMslMeters(latitudeDeg, longitudeDeg, altitudeMeters);
}

/// `ellipsoidHeight` selects MSL→HAE. Otherwise `mslMeters` is already the
/// demand datum. Inverse of `demandAltitudeToMslMeters`.
inline double mslToDemandAltitudeMeters(double latitudeDeg,
                                        double longitudeDeg,
                                        double mslMeters,
                                        bool ellipsoidHeight) {
  if (!ellipsoidHeight) return mslMeters;
  return mslToEllipsoidMeters(latitudeDeg, longitudeDeg, mslMeters);
}

/// Mean-sea-level change below this is left alone when a horizontal move
/// re-expresses an ellipsoidal height. One centimetre.
inline constexpr double kHorizontalMoveMslEpsilonMeters = 0.01;

/// Keeps the same ellipsoidal height when the horizontal position changes.
/// `oldMslMeters` is mean sea level at the old point; the result is mean sea
/// level at the new point.
inline double mslAfterHorizontalMove(double oldLatitudeDeg,
                                     double oldLongitudeDeg,
                                     double oldMslMeters,
                                     double newLatitudeDeg,
                                     double newLongitudeDeg) {
  const double hae =
      mslToEllipsoidMeters(oldLatitudeDeg, oldLongitudeDeg, oldMslMeters);
  return ellipsoidToMslMeters(newLatitudeDeg, newLongitudeDeg, hae);
}

} // namespace reactnativecesium
