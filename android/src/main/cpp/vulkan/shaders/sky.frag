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

  vec2 er = rsi(org, rd, ER);
  float tmax = at.y;
  if (er.x > 0.0) tmax = min(tmax, er.x);
  float tmin = max(at.x, 0.0);
  if (tmin >= tmax) { outColor = vec4(0, 0, 0, 1); return; }

  const int S = 16, L = 4;
  float ss = (tmax - tmin) / float(S);
  vec3 rayl = vec3(0), miel = vec3(0);
  float oR = 0.0, oM = 0.0;

  for (int i = 0; i < S; ++i) {
    float t = tmin + (float(i) + 0.5) * ss;
    vec3 sp = org + rd * t;
    float h = length(sp) - ER;
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
  col *= vec3(0.18, 0.45, 1.0);

  vec3 nu = normalize(org);
  float up = clamp(dot(rd, nu), 0.0, 1.0);
  float camH = max(0.0, length(org) - ER);
  float hStr = clamp(1.0 - camH / 35000.0, 0.0, 1.0);
  col = mix(col, vec3(0.40, 0.62, 0.96), (1.0 - smoothstep(0.05, 0.45, up)) * 0.40 * hStr);

  outColor = vec4(col, 1.0);
}
