#version 450

layout(set = 0, binding = 0) uniform SkyUniforms {
  mat4 invVP;
  vec4 cameraEcef;
  vec4 lightDir;
} u;

layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 outColor;

const float ER = 6371000.0, AR = ER + 111000.0;
const vec3 RAY = vec3(3.2e-6, 8.5e-6, 38e-6), MIE3 = vec3(1.8e-6, 1.8e-6, 1.8e-6);
const float MG = 0.76, LINT = 18.0;

vec2 rsi(vec3 o, vec3 d, float r) {
  float a = dot(d, d), b = 2.0 * dot(d, o), c = dot(o, o) - r * r, disc = b * b - 4.0 * a * c;
  if (disc < 0.0) return vec2(-1.0);
  float sq = sqrt(disc);
  return vec2((-b - sq) / (2.0 * a), (-b + sq) / (2.0 * a));
}

void main() {
  vec4 wn = u.invVP * vec4(ndc, -1, 1), wf = u.invVP * vec4(ndc, 1, 1);
  vec3 near3 = wn.xyz / wn.w, far3 = wf.xyz / wf.w;
  vec3 rd = normalize(far3 - near3);
  vec3 org = u.cameraEcef.xyz;

  vec2 at = rsi(org, rd, AR);
  if (at.y < 0.0) { outColor = vec4(0, 0, 0, 1); return; }

  // No earth-surface tmax clamp — that creates a visible band discontinuity
  // at the geometric horizon (rays below horizon get short atmospheric path,
  // showing as lighter sky between the orange horizon glow and terrain).
  // Instead integrate full atmospheric path and clamp underground sample
  // heights to zero so they contribute ground-level density.
  float tmax = at.y;
  float tmin = max(at.x, 0.0);
  if (tmin >= tmax) { outColor = vec4(0, 0, 0, 1); return; }

  // Sample counts reduced 16/4 -> 8/2 -> 6/2 — visually negligible at typical
  // phone DPI / viewport sizes (a 24-bit screen can't resolve the difference
  // in atmospheric scattering past ~6 primary samples), and cuts the expensive
  // per-pixel `exp` calls further (helps weak/software GPUs most).
  const int S = 6, L = 2;
  float ss = (tmax - tmin) / float(S);
  vec3 rayl = vec3(0), miel = vec3(0);
  float oR = 0.0, oM = 0.0;

  for (int i = 0; i < S; ++i) {
    float t = tmin + (float(i) + 0.5) * ss;
    vec3 sp = org + rd * t;
    float h = max(0.0, length(sp) - ER);
    float hr = exp(-h / 10000.0) * ss, hm = exp(-h / 3200.0) * ss;
    oR += hr; oM += hm;

    vec2 lh = rsi(sp, u.lightDir.xyz, AR);
    if (lh.y > 0.0) {
      float ls = lh.y / float(L);
      float lR = 0.0, lM = 0.0;
      bool bl = false;
      for (int j = 0; j < L; ++j) {
        vec3 lsp = sp + u.lightDir.xyz * ((float(j) + 0.5) * ls);
        float lh2 = length(lsp) - ER;
        if (lh2 < 0.0) { bl = true; break; }
        lR += exp(-lh2 / 10000.0) * ls;
        lM += exp(-lh2 / 3200.0) * ls;
      }
      if (!bl) {
        vec3 tau = RAY * (oR + lR) + MIE3 * 1.1 * (oM + lM);
        vec3 att = exp(-tau);
        rayl += hr * att;
        miel += hm * att;
      }
    }
  }

  float cosT = dot(rd, u.lightDir.xyz), cos2 = cosT * cosT;
  float rPh = 0.75 * (1.0 + cos2);
  float g2 = MG * MG;
  float mPh = 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + cos2) / pow(1.0 + g2 - 2.0 * MG * cosT, 1.5);
  vec3 col = LINT * (rayl * RAY * rPh + miel * MIE3 * mPh);
  col = 1.0 - exp(-col * 1.1);
  col *= vec3(0.30, 0.55, 1.0);

  // Single-scatter atmosphere produces sunset-like yellow at horizons with
  // long path lengths (because blue out-scatters faster). Real daytime sky
  // appears whitish-blue at horizon thanks to multiple scattering, which we
  // approximate here by blending toward a natural blue based on the
  // total atmospheric path length traversed by the view ray.
  float pathLen = tmax - tmin;
  float hazeFactor = smoothstep(100000.0, 600000.0, pathLen);
  col = mix(col, vec3(0.35, 0.58, 0.90), hazeFactor * 0.65);

  outColor = vec4(col, 1.0);
}
