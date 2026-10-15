#pragma once

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

// Pure terrain-floor helpers. No Cesium types, so the host harness can test
// contour rules without a tileset.

// Ignore a lead step shorter than this (hover) or longer than this (a slew,
// not one frame of travel).
inline constexpr double kTerrainLeadMinMeters = 0.05;
inline constexpr double kTerrainLeadMaxMeters = 80.0;
inline constexpr double kEarthRadiusMeters    = 6371008.7714150595;

// Horizontal barycentric height. (pe, pn) and the triangle are in metres in
// the same local east/north frame. Near-vertical triangles (terrain skirts)
// have ~0 horizontal area and are rejected. On a hit, outHeight is the
// interpolated ellipsoid height in metres.
inline bool horizontalTriangleHeight(double pe, double pn,
                                     double e0, double n0, float h0,
                                     double e1, double n1, float h1,
                                     double e2, double n2, float h2,
                                     float& outHeight) {
  const double v0e = e1 - e0;
  const double v0n = n1 - n0;
  const double v1e = e2 - e0;
  const double v1n = n2 - n0;
  const double v2e = pe - e0;
  const double v2n = pn - n0;

  const double dot00 = v0e * v0e + v0n * v0n;
  const double dot01 = v0e * v1e + v0n * v1n;
  const double dot11 = v1e * v1e + v1n * v1n;
  const double dot02 = v0e * v2e + v0n * v2n;
  const double dot12 = v1e * v2e + v1n * v2n;

  // denom = (2 * signed horizontal area)^2.
  const double denom = dot00 * dot11 - dot01 * dot01;
  if (std::abs(denom) < 1.0e-12) return false;

  const double e20 = e2 - e1;
  const double n20 = n2 - n1;
  const double edge2 = e20 * e20 + n20 * n20;
  const double longest = std::sqrt(std::max(dot00, std::max(dot11, edge2)));
  const double twiceArea = std::sqrt(std::abs(denom));
  // Width of the triangle across its longest edge. Terrain skirts are
  // vertical, so float error leaves them only millimetres wide.
  if (longest < 1.0e-6 || twiceArea / longest < 0.05) return false;

  const double inv = 1.0 / denom;
  const double u = (dot11 * dot02 - dot01 * dot12) * inv;
  const double v = (dot00 * dot12 - dot01 * dot02) * inv;
  const double w = 1.0 - u - v;

  constexpr double kEdge = -1.0e-4;
  if (u < kEdge || v < kEdge || w < kEdge) return false;

  outHeight = static_cast<float>(w * static_cast<double>(h0) +
                                 u * static_cast<double>(h1) +
                                 v * static_cast<double>(h2));
  return true;
}

// Fresh each frame. The higher of the nadir sample and the one-frame lead.
// A lower lead (past a crest) does not keep the previous peak. A higher lead
// (valley turning uphill) raises the floor in this frame. No memory.
inline bool resolveSurfaceEllipsoid(bool hasNadir, float nadir,
                                    bool hasAhead, float ahead,
                                    float& outHeight) {
  if (hasNadir && hasAhead) {
    outHeight = std::max(nadir, ahead);
    return true;
  }
  if (hasNadir) {
    outHeight = nadir;
    return true;
  }
  if (hasAhead) {
    outHeight = ahead;
    return true;
  }
  return false;
}

// One frame ahead of (latDeg, lonDeg) along the last step. Returns false when
// there is no distinct lead; out* are then equal to the current position.
inline bool terrainLeadLatLon(double latDeg, double lonDeg,
                              double prevLatDeg, double prevLonDeg,
                              bool hasPrev,
                              double& outLatDeg, double& outLonDeg) {
  outLatDeg = latDeg;
  outLonDeg = lonDeg;
  if (!hasPrev) return false;

  const double dLat = latDeg - prevLatDeg;
  double dLon = lonDeg - prevLonDeg;
  dLon = std::fmod(dLon + 540.0, 360.0) - 180.0;

  const double latRad = latDeg * (3.14159265358979323846 / 180.0);
  const double northM = dLat * (3.14159265358979323846 / 180.0) * kEarthRadiusMeters;
  const double eastM =
      dLon * (3.14159265358979323846 / 180.0) * kEarthRadiusMeters * std::cos(latRad);
  const double dist = std::hypot(eastM, northM);
  if (dist < kTerrainLeadMinMeters || dist > kTerrainLeadMaxMeters) return false;

  outLatDeg = latDeg + dLat;
  outLonDeg = lonDeg + dLon;
  return true;
}

} // namespace reactnativecesium
