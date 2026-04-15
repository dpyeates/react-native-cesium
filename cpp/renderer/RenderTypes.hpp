#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace reactnativecesium {

// ── CPU-side tile resources ─────────────────────────────────────────────────
// RTC (Relative-to-Center) layout:
//   • rtcCenter   – tile centre in absolute ECEF (double precision)
//   • localPositions – vertex positions relative to rtcCenter (float32)
//     Keeping positions small (~tile radius) preserves float32 precision even
//     at planetary scale.  The large camera-to-centre offset is baked into the
//     per-tile MVP matrix using double-precision arithmetic on the CPU; only the
//     final cast to float32 (which happens at the end of double-precision maths)
//     introduces rounding, not the original position data.
//   • altitudes   – ellipsoid height in metres per vertex, computed on the CPU
//     in double precision (Bowring formula in double gives sub-mm accuracy).
//     Passed as a vertex attribute so the fragment shader never has to call
//     ellipsoidHeightMeters(wp) — that path catastrophically cancels two ~6.4 Mm
//     float32 values and produces ±50 m errors.

struct TilePrimitive {
  glm::dvec3              rtcCenter;       // tile RTC centre in absolute ECEF (double)
  std::vector<glm::vec3>  localPositions;  // vertex positions relative to rtcCenter (float)
  std::vector<float>      altitudes;       // ellipsoid height metres per vertex (float, from double)
  std::vector<glm::vec2>  uvs;             // overlay UV coords (_CESIUMOVERLAY_0); empty if unavailable
  std::vector<uint32_t>   indices;         // triangle list (uint32)
};

struct TileGPUResources {
  std::vector<TilePrimitive> primitives;
  uint64_t lastUsedFrame  = 0;
  void*    overlayTexture = nullptr;
  glm::vec2 overlayTranslation{0.0f, 0.0f};
  glm::vec2 overlayScale{1.0f, 1.0f};

  // Water mask — extracted from GLTF extras in the load thread, then converted
  // to a GPU texture on the main thread by the platform waterMaskCreator_.
  std::vector<uint8_t> waterMaskPixels;
  void* waterMaskTexture = nullptr;
  bool      isOnlyWater      = false;
  glm::vec2 wmTranslation{0.0f, 0.0f};
  float     wmScale = 1.0f;
};

// ── Per-frame result passed from engine to renderer ─────────────────────────

struct DrawPrimitive {
  uint32_t  indexByteOffset      = 0;
  uint32_t  indexCount           = 0;
  bool      hasUVs               = false;
  bool      isEllipsoidFallback  = false;
  void*     overlayTexture       = nullptr;
  glm::vec2 overlayTranslation{0.0f, 0.0f};
  glm::vec2 overlayScale{1.0f, 1.0f};
  void*     waterMaskTexture     = nullptr;
  bool      isOnlyWater          = false;
  glm::vec4 wmTileBounds{0.0f, 0.0f, 0.0f, 0.0f};
  glm::vec2 wmTranslation{0.0f, 0.0f};
  float     wmScale = 1.0f;

  // RTC: per-tile MVP matrix (computed in double precision on the CPU,
  // then cast to float32).  Used by the vertex shader instead of the global
  // rotation-only VP, so that the large camera↔tile-centre offset is encoded
  // accurately without a float32 subtraction in the vertex shader.
  glm::mat4 mvpMatrix{1.0f};

  // Tile RTC centre in ECEF (float3).  Passed to the fragment shader so it can
  // reconstruct the world position as  wp = localPos + rtcCenterEcef,  which
  // avoids the catastrophic cancellation of eyeRelPos + cameraEcef at altitude.
  glm::vec3 rtcCenterEcef{0.0f};
};

struct FrameResult {
  // Merged geometry — one flat buffer, globally-offset indices.
  // localPositions: vertex positions relative to each tile's RTC centre (float).
  // altitudes:      ellipsoid height metres per vertex (float, double-precision source).
  std::vector<float>    localPositions; // xyz per vertex (tile-local float)
  std::vector<float>    altitudes;      // 1 float per vertex, ellipsoid height metres
  std::vector<float>    uvs;            // uv per vertex (2 floats)
  std::vector<uint32_t> indices;

  std::vector<DrawPrimitive> draws;

  // Global rotation-only VP (no translation) — used by the sky shader and for
  // reference.  Terrain tiles use per-draw mvpMatrix instead.
  glm::mat4 vpMatrix;
  glm::vec3 cameraEcef;   // camera ECEF (float) — for sky shader
  glm::mat4 invVP;

  int tilesRendered  = 0;
  int tilesLoading   = 0;
  int tilesVisited   = 0;
  double cameraLat   = 0.0;
  double cameraLon   = 0.0;
  double cameraAlt   = 0.0;

  bool                     ionTokenConfigured = false;
  bool                     tilesetActive      = false;
  double                   verticalFovDeg     = 60.0;
  std::vector<std::string> creditHtmlLines;
};

// ── Frame setup ─────────────────────────────────────────────────────────────

struct FrameParams {
  glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace reactnativecesium
