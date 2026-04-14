#version 450

layout(location = 0) out vec2 ndc;

void main() {
  vec2 ps[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
  gl_Position = vec4(ps[gl_VertexIndex], 0, 1);
  ndc = ps[gl_VertexIndex];
}
