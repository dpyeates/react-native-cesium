#version 450

// Per-frame UBO (set 0): just the camera position in ECEF.
// Vertex-stage MVP is now in push constants, so the UBO is fragment-only.
layout(set = 0, binding = 0) uniform TerrainUBO {
  vec4 cameraEcef;  // xyz = camera ECEF (float), w unused
} u;

// Push constants — 128 bytes split between vertex (bytes 0-63) and fragment
// (bytes 64-127).  The leading mat4 mirrors the vertex-shader declaration so
// both stages agree on the std430 layout; only the fragment-specific fields
// after it are actually read here.
layout(push_constant) uniform PC {
  mat4  _mvp;               // bytes   0-63  (vertex only, ignored here)
  uint  hasOverlay;         // byte   64
  uint  isEllipsoidFallback;// byte   68
  uint  isOnlyWater;        // byte   72
  uint  hasWaterMask;       // byte   76
  float wmWest;             // byte   80
  float wmSouth;            // byte   84
  float wmEast;             // byte   88
  float wmNorth;            // byte   92
  float rtcCenterX;         // byte   96
  float rtcCenterY;         // byte  100
  float rtcCenterZ;         // byte  104
  float translationX;       // byte  108
  float translationY;       // byte  112
  float scaleX;             // byte  116
  float scaleY;             // byte  120
  float _pad;               // byte  124
} pc;

layout(set = 1, binding = 0) uniform sampler2D overlayTex;
layout(set = 2, binding = 0) uniform sampler2D waterMaskTex;

layout(location = 0) in vec3  localPos;
layout(location = 1) in vec2  fragUV;
layout(location = 2) in float fragAltitude;

layout(location = 0) out vec4 outColor;

vec3 hypsometricColor(float alt, float steep) {
  vec3 cDeep    = vec3(0.03, 0.20, 0.46), cShallow = vec3(0.08, 0.44, 0.78);
  vec3 cLow     = vec3(0.46, 0.54, 0.30), cFoot    = vec3(0.62, 0.55, 0.32);
  vec3 cHigh    = vec3(0.48, 0.38, 0.28), cAlp     = vec3(0.52, 0.50, 0.48);
  vec3 cSnow    = vec3(0.90, 0.92, 0.94);
  if (alt < 0.0) return mix(cDeep, cShallow, smoothstep(-4500.0, -20.0, alt));
  vec3 c = cLow;
  c = mix(c, cFoot,  smoothstep( 120.0,  900.0, alt));
  c = mix(c, cHigh,  smoothstep( 750.0, 2000.0, alt));
  c = mix(c, cAlp,   smoothstep(1700.0, 3600.0, alt));
  c = mix(c, cSnow,  smoothstep(3200.0, 5200.0, alt));
  vec3  scree = vec3(0.38, 0.35, 0.32);
  float rock  = clamp(steep * 1.08, 0.0, 1.0) * smoothstep(35.0, 140.0, alt);
  return mix(c, scree, rock);
}

void main() {
  // ── World position (RTC reconstruction) ────────────────────────────────────
  // wp = tile-local pos + RTC centre (both float32 at ~6.4 Mm scale).
  // ~0.75 m ULP — far better than eyeRelPos + cameraEcef at altitude.
  vec3  rtcCentre = vec3(pc.rtcCenterX, pc.rtcCenterY, pc.rtcCenterZ);
  vec3  wp        = localPos + rtcCentre;
  float pxy       = sqrt(wp.x * wp.x + wp.y * wp.y);
  float lon       = atan(wp.y, wp.x);

  // Geodetic latitude (Bowring iteration, float32 — good to ~0.1 m for UV).
  const float a_e = 6378137.0, b_e = 6356752.31424518;
  const float ep2 = (a_e * a_e - b_e * b_e) / (b_e * b_e);
  const float e2  = 1.0 - (b_e * b_e) / (a_e * a_e);
  float theta_g   = atan(wp.z * a_e, pxy * b_e);
  float sg        = sin(theta_g), cg = cos(theta_g);
  float lat       = atan(wp.z + ep2 * b_e * sg * sg * sg,
                         pxy  - e2  * a_e * cg * cg * cg);

  // ── Water-mask UV from geographic lat/lon + tile bounds ───────────────────
  float wmU   = (lon - pc.wmWest)  / max(pc.wmEast  - pc.wmWest,  1e-7);
  float v_geo = (lat - pc.wmSouth) / max(pc.wmNorth - pc.wmSouth, 1e-7);
  // Flip V: Cesium geographic convention (V=0 south) vs texture (V=0 north).
  vec2  wmUV  = clamp(vec2(wmU, 1.0 - v_geo), 0.0, 1.0);

  float waterVal = (pc.isOnlyWater != 0u) ? 1.0 : texture(waterMaskTex, wmUV).r;

  // ── Imagery overlay path ──────────────────────────────────────────────────
  if (pc.hasOverlay != 0u) {
    vec2 texUV = fragUV * vec2(pc.scaleX, pc.scaleY)
               + vec2(pc.translationX, pc.translationY);
    texUV.y = 1.0 - texUV.y;
    outColor = texture(overlayTex, texUV);
    return;
  }

  // ── Hypsometric path ──────────────────────────────────────────────────────
  vec3  dpdx_v = dFdx(localPos), dpdy_v = dFdy(localPos);
  vec3  rawN   = cross(dpdx_v, dpdy_v);
  float nLen   = length(rawN);
  vec3  n      = (nLen > 1e-8) ? rawN / nLen : vec3(0.0, 0.0, 1.0);

  // View direction in tile-local space.
  // Camera-in-tile-space = cameraEcef − rtcCentre (float32, acceptable for lighting).
  vec3 cameraTilespace = u.cameraEcef.xyz - rtcCentre;
  vec3 vd = normalize(cameraTilespace - localPos);
  if (dot(n, vd) < 0.0) n = -n;

  vec3  gu   = normalize(wp);
  vec3  sun  = normalize(gu + vec3(0.3, 0.2, 0.1));
  float diff  = clamp(dot(n, sun), 0.0, 1.0);
  float amb   = 0.18;
  float rim   = clamp(1.0 - dot(n, vd), 0.0, 1.0) * 0.06;
  float steep = 1.0 - clamp(abs(dot(n, gu)), 0.0, 1.0);

  // Use vertex altitude directly — computed on the CPU in double (sub-mm).
  // Avoids the ±50 m catastrophic cancellation of float32 pxy/cos(lat) − N.
  float rawAlt    = fragAltitude;
  vec3  oceanBlue = vec3(0.04, 0.26, 0.52);
  float landAlt   = (pc.hasWaterMask != 0u) ? max(rawAlt, 0.0) : rawAlt;
  vec3  landLit   = hypsometricColor(landAlt, steep) * (amb + diff * 0.72 + rim);
  vec3  lit       = mix(landLit, oceanBlue, waterVal);

  // 1 km lat/lon grid — fades when sub-pixel to suppress Moiré.
  const float gridLat = 1000.0 / 6371000.0;
  float cosLat  = max(cos(lat), 0.001);
  float gridLon = gridLat / cosLat;
  float wLat    = fwidth(lat) * 1.5, wLon = fwidth(lon) * 1.5;
  float gridVis = clamp(min(gridLat / max(wLat, 1e-9),
                            gridLon / max(wLon, 1e-9)), 0.0, 1.0);
  float latCell = lat - gridLat * floor(lat / gridLat);
  float lonCell = lon - gridLon * floor(lon / gridLon);
  float dLat    = min(latCell, gridLat - latCell);
  float dLon    = min(lonCell, gridLon - lonCell);
  float gridA   = max(1.0 - smoothstep(0.0, wLat, dLat),
                      1.0 - smoothstep(0.0, wLon, dLon)) * gridVis;
  lit = mix(lit, vec3(0.15, 0.15, 0.15), gridA * 0.6);

  // Coastline outline from water mask.
  if (pc.isEllipsoidFallback == 0u) {
    float dWater  = fwidth(waterVal);
    float coastPx = abs(waterVal - 0.5) / max(dWater * 0.5, 0.01);
    float coastA  = 1.0 - smoothstep(0.0, 1.5, coastPx);
    lit = mix(lit, vec3(0.18, 0.14, 0.10), coastA * 0.7);
  }

  outColor = vec4(lit, 1.0);
}
