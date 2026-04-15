#version 450

// RTC vertex shader.
// Vertex positions are tile-local (relative to the tile's RTC centre).
// The per-tile MVP matrix is computed on the CPU entirely in double precision,
// then cast to float32.  This eliminates the large camera↔tile-centre float32
// subtraction that caused ±1.6 m vertex precision errors at altitude.
layout(push_constant) uniform PC {
  mat4 mvpMatrix;   // per-draw RTC MVP (bytes 0-63)
} vpc;

layout(location = 0) in vec3  inPosition;  // tile-local position (float)
layout(location = 1) in vec2  inUV;
layout(location = 2) in float inAltitude;  // ellipsoid height metres (double source)

layout(location = 0) out vec3  localPos;
layout(location = 1) out vec2  fragUV;
layout(location = 2) out float fragAltitude;

void main() {
  gl_Position  = vpc.mvpMatrix * vec4(inPosition, 1.0);
  localPos     = inPosition;
  fragUV       = inUV;
  fragAltitude = inAltitude;
}
