#include <metal_stdlib>
using namespace metal;

// ── Terrain ─────────────────────────────────────────────────────────────────

struct TerrainUniforms {
  float4x4 vpMatrix;   // flipY(reversedZProj) * viewRotation
  float3   cameraEcef; // camera world position (float, for altitude calc)
  uint     _pad;
};

struct TerrainVOut {
  float4 position [[position]];
  float3 eyeRelPos;
};

/// WGS84 ellipsoidal height above the surface (meters).
float ellipsoidHeightMeters(float3 p) {
  const float a  = 6378137.0f;
  const float b  = 6356752.31424518f;
  const float e2 = 1.0f - (b * b) / (a * a);
  const float ep2 = (a * a - b * b) / (b * b);
  float pxy = sqrt(p.x * p.x + p.y * p.y);
  if (pxy < 1e-6f) return abs(p.z) - b;
  float theta = atan2(p.z * a, pxy * b);
  float st = sin(theta), ct = cos(theta);
  float lat = atan2(p.z + ep2 * b * st * st * st,
                    pxy - e2 * a * ct * ct * ct);
  float sinLat = sin(lat);
  float N = a / sqrt(1.0f - e2 * sinLat * sinLat);
  return pxy / cos(lat) - N;
}

/// Hypsometric tint (no imagery).
float3 hypsometricColor(float alt, float steep) {
  float3 cDeep    = float3(0.03f, 0.20f, 0.46f);
  float3 cShallow = float3(0.08f, 0.44f, 0.78f);
  float3 cLow     = float3(0.46f, 0.54f, 0.30f);
  float3 cFoot    = float3(0.62f, 0.55f, 0.32f);
  float3 cHigh    = float3(0.48f, 0.38f, 0.28f);
  float3 cAlpine  = float3(0.52f, 0.50f, 0.48f);
  float3 cSnow    = float3(0.90f, 0.92f, 0.94f);

  if (alt < 0.0f)
    return mix(cDeep, cShallow, smoothstep(-4500.0f, -20.0f, alt));

  float3 c = cLow;
  c = mix(c, cFoot,   smoothstep( 120.0f,  900.0f, alt));
  c = mix(c, cHigh,   smoothstep( 750.0f, 2000.0f, alt));
  c = mix(c, cAlpine, smoothstep(1700.0f, 3600.0f, alt));
  c = mix(c, cSnow,   smoothstep(3200.0f, 5200.0f, alt));

  float3 scree   = float3(0.38f, 0.35f, 0.32f);
  float rockAmt  = saturate(steep * 1.08f) * smoothstep(35.0f, 140.0f, alt);
  return mix(c, scree, rockAmt);
}

vertex TerrainVOut terrainVertex(
    uint                          vertexID  [[vertex_id]],
    const device packed_float3*   positions [[buffer(0)]],
    constant TerrainUniforms&     u         [[buffer(1)]])
{
  TerrainVOut out;
  float3 ep   = float3(positions[vertexID]);
  out.position = u.vpMatrix * float4(ep, 1.0f);
  out.eyeRelPos = ep;
  return out;
}

fragment float4 terrainFragment(
    TerrainVOut             in [[stage_in]],
    constant TerrainUniforms& u [[buffer(0)]])
{
  // Face normal computed from screen-space derivatives of eye-relative position.
  float3 dpdx_v  = dfdx(in.eyeRelPos);
  float3 dpdy_v  = dfdy(in.eyeRelPos);
  float3 rawN    = cross(dpdx_v, dpdy_v);
  float  nLen    = length(rawN);
  float3 normal  = (nLen > 1e-8f) ? rawN / nLen : float3(0.0f, 0.0f, 1.0f);

  // Reconstruct world position (float precision acceptable for shading).
  float3 worldPos = in.eyeRelPos + u.cameraEcef;

  // Ensure normal faces the viewer.
  float3 viewDir = normalize(-in.eyeRelPos);
  if (dot(normal, viewDir) < 0.0f) normal = -normal;

  float3 globeUp  = normalize(worldPos);
  float3 sunDir   = normalize(globeUp + float3(0.3f, 0.2f, 0.1f));

  float diffuse = saturate(dot(normal, sunDir));
  float ambient = 0.18f;
  float rim     = saturate(1.0f - dot(normal, viewDir)) * 0.06f;

  float alt   = ellipsoidHeightMeters(worldPos);
  float steep = 1.0f - saturate(abs(dot(normal, globeUp)));
  float3 base = hypsometricColor(alt, steep);

  float shade = ambient + diffuse * 0.72f + rim;
  return float4(base * shade, 1.0f);
}

// ── Sky (full-screen triangle) ───────────────────────────────────────────────

struct SkyUniforms {
  float4x4 invVP;
  float3   cameraEcef;
  float    _pad0;
  float3   lightDir;
  float    _pad1;
};

struct SkyVOut {
  float4 position [[position]];
  float2 ndc;
};

vertex SkyVOut skyVertex(uint vid [[vertex_id]],
                         constant SkyUniforms& u [[buffer(0)]])
{
  // Full-screen triangle: vertex 0,1,2 cover [-1,1]² NDC.
  const float2 pos[3] = {
    float2(-1.0f, -1.0f),
    float2( 3.0f, -1.0f),
    float2(-1.0f,  3.0f)
  };
  SkyVOut out;
  out.position = float4(pos[vid], 0.0f, 1.0f);
  out.ndc      = pos[vid];
  return out;
}

// Minimal single-scatter atmospheric sky.
constant float EARTH_R    = 6371000.0f;
constant float ATMO_R     = EARTH_R + 111000.0f;
constant float3 RAYLEIGH  = float3(3.2e-6f, 8.5e-6f, 38.0e-6f);
constant float3 MIE       = float3(1.8e-6f, 1.8e-6f, 1.8e-6f);
constant float  MIE_G     = 0.76f;
constant float  INTENSITY = 18.0f;

float2 raySphereHit(float3 o, float3 d, float r) {
  float a = dot(d, d);
  float b = 2.0f * dot(d, o);
  float c = dot(o, o) - r * r;
  float disc = b * b - 4.0f * a * c;
  if (disc < 0.0f) return float2(-1.0f);
  float sq = sqrt(disc);
  return float2((-b - sq) / (2.0f * a), (-b + sq) / (2.0f * a));
}

fragment float4 skyFragment(SkyVOut in [[stage_in]],
                            constant SkyUniforms& u [[buffer(0)]])
{
  // Reconstruct world-space ray direction from NDC.
  float4 wNear = u.invVP * float4(in.ndc, -1.0f, 1.0f);
  float4 wFar  = u.invVP * float4(in.ndc,  1.0f, 1.0f);
  float3 near3 = wNear.xyz / wNear.w;
  float3 far3  = wFar.xyz  / wFar.w;
  float3 rayDir = normalize(far3 - near3);

  float3 origin = u.cameraEcef;
  float2 atmo   = raySphereHit(origin, rayDir, ATMO_R);
  if (atmo.y < 0.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

  float2 earth  = raySphereHit(origin, rayDir, EARTH_R);
  float tMax    = atmo.y;
  if (earth.x > 0.0f) tMax = min(tMax, earth.x);
  float tMin    = max(atmo.x, 0.0f);
  if (tMin >= tMax) return float4(0.0f, 0.0f, 0.0f, 1.0f);

  const int STEPS  = 16;
  const int LSTEPS = 4;
  float stepSz = (tMax - tMin) / float(STEPS);
  float3 rayl  = 0.0f, miel = 0.0f;
  float optR = 0.0f, optM = 0.0f;

  for (int i = 0; i < STEPS; ++i) {
    float  t    = tMin + (float(i) + 0.5f) * stepSz;
    float3 sp   = origin + rayDir * t;
    float  h    = length(sp) - EARTH_R;
    float  hr   = exp(-h / 10000.0f) * stepSz;
    float  hm   = exp(-h /  3200.0f) * stepSz;
    optR += hr; optM += hm;

    float2 lHit = raySphereHit(sp, u.lightDir, ATMO_R);
    if (lHit.y > 0.0f) {
      float lSz = lHit.y / float(LSTEPS);
      float lR  = 0.0f, lM = 0.0f;
      bool blocked = false;
      for (int j = 0; j < LSTEPS; ++j) {
        float3 ls = sp + u.lightDir * ((float(j) + 0.5f) * lSz);
        float  lh = length(ls) - EARTH_R;
        if (lh < 0.0f) { blocked = true; break; }
        lR += exp(-lh / 10000.0f) * lSz;
        lM += exp(-lh /  3200.0f) * lSz;
      }
      if (!blocked) {
        float3 tau  = RAYLEIGH * (optR + lR) + MIE * 1.1f * (optM + lM);
        float3 att  = exp(-tau);
        rayl += hr * att;
        miel += hm * att;
      }
    }
  }

  float cosT = dot(rayDir, u.lightDir);
  float cos2 = cosT * cosT;
  float rPh  = 0.75f * (1.0f + cos2);
  float g2   = MIE_G * MIE_G;
  float mPh  = 1.5f * ((1.0f - g2) / (2.0f + g2)) *
               (1.0f + cos2) / pow(1.0f + g2 - 2.0f * MIE_G * cosT, 1.5f);

  float3 color = INTENSITY * (rayl * RAYLEIGH * rPh + miel * MIE * mPh);
  color = 1.0f - exp(-color * 0.85f);
  color *= float3(0.62f, 0.78f, 1.0f);

  float3 nUp     = normalize(origin);
  float  upness  = saturate(dot(rayDir, nUp));
  float3 haze    = float3(0.92f, 0.93f, 0.95f);
  float  camH    = max(0.0f, length(origin) - EARTH_R);
  float  hStr    = saturate(1.0f - camH / 35000.0f);
  color = mix(color, haze, (1.0f - smoothstep(0.03f, 0.38f, upness)) * 0.55f * hStr);

  return float4(color, 1.0f);
}
