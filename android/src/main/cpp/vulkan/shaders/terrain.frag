#version 450

layout(set = 0, binding = 0) uniform TerrainUniforms {
  mat4 vpMatrix;
  vec4 cameraEcef;
} u;

layout(push_constant) uniform OverlayParams {
  uint  hasOverlay;
  uint  isEllipsoidFallback;
  float translationX;
  float translationY;
  float scaleX;
  float scaleY;
} ov;

layout(set = 1, binding = 0) uniform sampler2D overlayTex;

layout(location = 0) in vec3 eyeRelPos;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

float ellipsoidHeightMeters(vec3 p) {
  const float a = 6378137.0, b = 6356752.31424518;
  const float e2 = 1.0 - (b * b) / (a * a), ep2 = (a * a - b * b) / (b * b);
  float pxy = sqrt(p.x * p.x + p.y * p.y);
  if (pxy < 1e-6) return abs(p.z) - b;
  float theta = atan(p.z * a, pxy * b);
  float st = sin(theta), ct = cos(theta);
  float lat = atan(p.z + ep2 * b * st * st * st, pxy - e2 * a * ct * ct * ct);
  float sinLat = sin(lat);
  float N = a / sqrt(1.0 - e2 * sinLat * sinLat);
  return pxy / cos(lat) - N;
}

vec3 hypsometricColor(float alt, float steep) {
  vec3 cDeep = vec3(0.03, 0.20, 0.46), cShallow = vec3(0.08, 0.44, 0.78);
  vec3 cLow = vec3(0.46, 0.54, 0.30), cFoot = vec3(0.62, 0.55, 0.32);
  vec3 cHigh = vec3(0.48, 0.38, 0.28), cAlp = vec3(0.52, 0.50, 0.48), cSnow = vec3(0.90, 0.92, 0.94);
  if (alt < 0.0) return mix(cDeep, cShallow, smoothstep(-4500.0, -20.0, alt));
  vec3 c = cLow;
  c = mix(c, cFoot, smoothstep(120.0, 900.0, alt));
  c = mix(c, cHigh, smoothstep(750.0, 2000.0, alt));
  c = mix(c, cAlp,  smoothstep(1700.0, 3600.0, alt));
  c = mix(c, cSnow, smoothstep(3200.0, 5200.0, alt));
  vec3 scree = vec3(0.38, 0.35, 0.32);
  float rock = clamp(steep * 1.08, 0.0, 1.0) * smoothstep(35.0, 140.0, alt);
  return mix(c, scree, rock);
}

void main() {
  if (ov.hasOverlay != 0u) {
    vec2 texUV = fragUV * vec2(ov.scaleX, ov.scaleY)
               + vec2(ov.translationX, ov.translationY);
    texUV.y = 1.0 - texUV.y;
    outColor = texture(overlayTex, texUV);
    return;
  }

  vec3 dpdx_v = dFdx(eyeRelPos), dpdy_v = dFdy(eyeRelPos);
  vec3 rawN = cross(dpdx_v, dpdy_v);
  float nLen = length(rawN);
  vec3 n = (nLen > 1e-8) ? rawN / nLen : vec3(0, 0, 1);

  vec3 wp = eyeRelPos + u.cameraEcef.xyz;
  vec3 vd = normalize(-eyeRelPos);
  if (dot(n, vd) < 0.0) n = -n;

  vec3 gu = normalize(wp);
  vec3 sun = normalize(gu + vec3(0.3, 0.2, 0.1));
  float diff = clamp(dot(n, sun), 0.0, 1.0);
  float amb = 0.18, rim = clamp(1.0 - dot(n, vd), 0.0, 1.0) * 0.06;
  float steep = 1.0 - clamp(abs(dot(n, gu)), 0.0, 1.0);
  float alt = (ov.isEllipsoidFallback != 0u) ? 0.0 : ellipsoidHeightMeters(wp);
  vec3 base = hypsometricColor(alt, steep);
  vec3 lit = base * (amb + diff * 0.72 + rim);

  float pxy = sqrt(wp.x * wp.x + wp.y * wp.y);
  float lat = atan(wp.z, pxy);
  float lon = atan(wp.y, wp.x);
  // Grid cell width ≈ 1 km at the equator (radians)
  const float gridLat = 1000.0 / 6371000.0;
  float cosLat = max(cos(lat), 0.001);
  float gridLon = gridLat / cosLat;
  float wLat = fwidth(lat) * 1.5;
  float wLon = fwidth(lon) * 1.5;
  // When a pixel covers more than one grid cell the smoothstep-based
  // anti-aliasing inverts: gridA → 1.0 everywhere, painting the whole tile
  // grey and creating Moiré circles / false "gaps" at high altitude.
  // gridVis fades the grid to zero as soon as it becomes sub-pixel.
  float gridVis = clamp(min(gridLat / max(wLat, 1e-9),
                            gridLon / max(wLon, 1e-9)), 0.0, 1.0);
  float latCell = lat - gridLat * floor(lat / gridLat);
  float lonCell = lon - gridLon * floor(lon / gridLon);
  float dLat = min(latCell, gridLat - latCell);
  float dLon = min(lonCell, gridLon - lonCell);
  float gridA = max(1.0 - smoothstep(0.0, wLat, dLat),
                    1.0 - smoothstep(0.0, wLon, dLon)) * gridVis;
  vec3 gridCol = vec3(0.15, 0.15, 0.15);
  lit = mix(lit, gridCol, gridA * 0.6);

  outColor = vec4(lit, 1.0);
}
