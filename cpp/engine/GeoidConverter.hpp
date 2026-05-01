#pragma once

namespace reactnativecesium {

/// Converts orthometric height (MSL) to ellipsoidal height (HAE, WGS84).
double mslToEllipsoidMeters(double latitudeDeg,
                            double longitudeDeg,
                            double mslMeters);

} // namespace reactnativecesium
