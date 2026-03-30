#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace reactnativecesium {

// ── CPU-side tile resources ─────────────────────────────────────────────────
// Vertices stored as absolute ECEF in double precision (transformed at load
// time). At render time we subtract the camera position to get camera-relative
// float positions, which avoids single-precision floating-point cancellation
// for large Earth-radius coordinates.

struct TilePrimitive {
  std::vector<glm::dvec3> positionsEcef; // absolute ECEF world positions
  std::vector<glm::vec2>  uvs;           // overlay UV coords (_CESIUMOVERLAY_0); empty if unavailable
  std::vector<uint32_t>   indices;       // triangle list (uint32)
};

struct TileGPUResources {
  std::vector<TilePrimitive> primitives;
  uint64_t lastUsedFrame  = 0;
  void*    overlayTexture = nullptr; // id<MTLTexture>; set by attachRasterInMainThread (main thread only)
};

// ── Per-frame result passed from engine to renderer ─────────────────────────

struct DrawPrimitive {
  uint32_t indexByteOffset = 0;
  uint32_t indexCount      = 0;
  bool     hasUVs          = false;  // true if this primitive has valid overlay UVs
  void*    overlayTexture  = nullptr; // id<MTLTexture> or nullptr
};

struct FrameResult {
  // Merged geometry — one flat buffer, globally-offset indices.
  std::vector<float>    eyeRelPositions; // xyz per vertex (camera-relative float)
  std::vector<float>    uvs;             // uv per vertex (2 floats); parallel to eyeRelPositions
  std::vector<uint32_t> indices;

  // One entry per tile primitive — indices into the merged index buffer.
  std::vector<DrawPrimitive> draws;

  // Matrices (float, ready for GPU uniforms).
  glm::mat4 vpMatrix;      // flipY(projection) * viewRotation (no translation)
  glm::vec3 cameraEcef;    // camera position in ECEF (float)

  // Sky rendering helpers.
  glm::mat4 invVP;         // inverse of vpMatrix (for sky ray reconstruction)

  // Debug counters.
  int tilesRendered  = 0;
  int tilesLoading   = 0;
  int tilesVisited   = 0;
  double cameraLat   = 0.0;
  double cameraLon   = 0.0;
  double cameraAlt   = 0.0;

  // Debug / overlay / metrics (filled by CesiumEngine::updateFrame).
  bool                     ionTokenConfigured = false;
  bool                     tilesetActive      = false;
  double                   verticalFovDeg     = 60.0;
  std::vector<std::string> creditHtmlLines; // raw HTML from Cesium CreditSystem
};

// ── Frame setup ─────────────────────────────────────────────────────────────

struct FrameParams {
  glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace reactnativecesium
