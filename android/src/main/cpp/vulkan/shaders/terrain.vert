#version 450

layout(set = 0, binding = 0) uniform TerrainUniforms {
  mat4 vpMatrix;
  vec4 cameraEcef;
} u;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec3 eyeRelPos;
layout(location = 1) out vec2 fragUV;

void main() {
  gl_Position = u.vpMatrix * vec4(inPosition, 1.0);
  eyeRelPos = inPosition;
  fragUV = inUV;
}
