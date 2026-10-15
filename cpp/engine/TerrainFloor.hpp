#pragma once

#include "CameraIntegrator.hpp"
#include "CesiumEngine.hpp"
#include "GeoidConverter.hpp"
#include "TerrainHeight.hpp"

#include <algorithm>

namespace reactnativecesium {

// Previous horizontal position and whether a real mesh sample has landed.
// Shared by the iOS and Android render loops so both follow the same floor.
struct TerrainLeadState {
  bool hasPrev = false;
  bool latched = false;
  double prevLatDeg = 0.0;
  double prevLonDeg = 0.0;
};

// A position slew drops the old floor and the one-frame lead. The next
// rendered frame samples terrain at the new position.
inline void noteTerrainSlew(CameraIntegrator& integrator, TerrainLeadState& lead) {
  integrator.clearTerrainFloor();
  lead = {};
}

// Samples the resident mesh at the stepped camera and one frame ahead, then
// holds the rendered altitude to surface + clearance before the frame is drawn.
// Demand is not modified. Returns the camera to render.
inline CameraParams applyTerrainFloor(CesiumEngine& engine,
                                      CameraIntegrator& integrator,
                                      const CameraParams& stepped,
                                      TerrainLeadState& lead,
                                      double clearanceMeters) {
  const double clearance = std::max(0.0, clearanceMeters);

  double leadLat = stepped.latitude;
  double leadLon = stepped.longitude;
  const bool sampleLead = terrainLeadLatLon(
      stepped.latitude, stepped.longitude,
      lead.prevLatDeg, lead.prevLonDeg, lead.hasPrev,
      leadLat, leadLon);

  float nadir = 0.0f;
  float ahead = 0.0f;
  const bool hasNadir =
      engine.sampleTerrainEllipsoidMeters(stepped.latitude, stepped.longitude, nadir);
  const bool hasAhead = sampleLead &&
      engine.sampleTerrainEllipsoidMeters(leadLat, leadLon, ahead);

  float surface = 0.0f;
  const bool haveMesh = resolveSurfaceEllipsoid(hasNadir, nadir, hasAhead, ahead, surface);
  const double geoidN = mslToEllipsoidMeters(stepped.latitude, stepped.longitude, 0.0);
  if (haveMesh) {
    integrator.setTerrainFloor(static_cast<double>(surface) - geoidN + clearance);
    lead.latched = true;
  } else if (!lead.latched) {
    // No mesh yet: stay at or above the ellipsoid. Once a mesh sample has
    // landed, a later miss keeps the last floor instead of dropping to sea level.
    integrator.setTerrainFloor(-geoidN + clearance);
  }

  lead.prevLatDeg = stepped.latitude;
  lead.prevLonDeg = stepped.longitude;
  lead.hasPrev = true;
  return integrator.getActual();
}

} // namespace reactnativecesium
