#pragma once

#include <glm/glm.hpp>

namespace reactnativecesium {

// NOTE: Metal float3 has 16-byte alignment in structs (same as float4).
// To avoid C++/Metal layout mismatch, each vec3+float pair is packed into vec4.
struct SceneUniforms {
  glm::mat4 viewProjection;
  glm::vec4 cameraPositionF;   // w = earthRadius
  glm::vec4 sunDirection;      // w = atmosphereHeight
  glm::vec4 atmosphereColor;   // w = (unused, pad)
};

// Metal float3x3 columns are each 16 bytes (same as float4).
// Store matrices as three float4 columns to avoid C++/Metal layout mismatch.
struct TileUniforms {
  glm::vec4 rtcCenterHigh;   // .w = 0 (pad)
  glm::vec4 rtcCenterLow;    // .w = 0 (pad)
  glm::vec4 rotCol0;         // rotation matrix col 0, .w = 0
  glm::vec4 rotCol1;         // rotation matrix col 1, .w = 0
  glm::vec4 rotCol2;         // rotation matrix col 2, .w = 0
  glm::vec4 normalCol0;      // normalMatrix col 0, .w = 0
  glm::vec4 normalCol1;      // normalMatrix col 1, .w = 0
  glm::vec4 normalCol2;      // normalMatrix col 2, .w = 0
};

} // namespace reactnativecesium
