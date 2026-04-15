// MetalBackend.mm — iOS Metal rendering backend
//
// Architecture:
//   • RTC (Relative-to-Centre) tile-local float positions on the GPU.
//     Vertex positions are stored relative to each tile's RTC centre so that
//     the values remain small (tile radius, not full ECEF magnitude), giving
//     full float32 precision for geometry at any altitude.
//   • Per-tile MVP matrix computed on the CPU in double precision:
//       MVP = P_double * R_double * T_double(rtcCentre − camera)
//     then cast to float32 for the GPU.  This avoids the large float32
//     cancellation error that would arise from computing the camera offset
//     on the GPU.
//   • Vertex altitude attribute (metres, computed from double-precision Bowring
//     formula on the CPU).  Passed straight to the fragment shader — eliminates
//     the ±50 m float32 catastrophic-cancellation error of
//     ellipsoidHeightMeters(wp) in the old shader.
//   • Single merged vertex + index buffer uploaded each frame.
//   • Reversed-Z infinite projection: depth clear = 0, compare = GREATER.
//   • Sky drawn first (no depth write), terrain drawn after.
//   • Triple-buffered persistent MTLBuffers: one slot per in-flight frame.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include "MetalBackend.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring> // memcpy
#include <algorithm> // std::max

// ── CPU-side uniform structs ──────────────────────────────────────────────────
// float3 in MSL is 16-byte aligned (same as float4), so we always use float[4]
// on the C++ side to keep sizes identical between CPU and GPU structs.

// Per-draw vertex uniforms: the tile's RTC MVP matrix (double-precision on CPU,
// cast to float32 for the GPU).
struct PerDrawVertexUniformsCPP {
  float mvpMatrix[16];  // column-major float4x4 (64 bytes)
};                      // total 64 bytes

struct SkyUniforms {
  float invVP[16];      // column-major float4x4  (64 bytes)
  float cameraEcef[4];  // xyz used, w=0          (16 bytes)
  float lightDir[4];    // xyz used, w=0          (16 bytes)
};                      // total 96 bytes

// Per-draw fragment uniforms: all overlay/water-mask parameters plus the RTC
// centre and camera-in-tile-space needed to reconstruct world position.
struct OverlayParamsCPP {
  uint32_t hasOverlay;           // offset  0
  uint32_t isEllipsoidFallback;  // offset  4
  uint32_t isOnlyWater;          // offset  8
  uint32_t hasWaterMask;         // offset 12
  float    translation[2];       // offset 16
  float    scale[2];             // offset 24
  float    wmWest;               // offset 32
  float    wmSouth;              // offset 36
  float    wmEast;               // offset 40
  float    wmNorth;              // offset 44
  float    wmTransX;             // offset 48
  float    wmTransY;             // offset 52
  float    wmScale;              // offset 56
  float    _pad1;                // offset 60
  // RTC centre in absolute ECEF (float3).  Fragment shader reconstructs world
  // position as:  wp = in.localPos + rtcCentre  (see shader).
  float    rtcCenterX;           // offset 64
  float    rtcCenterY;           // offset 68
  float    rtcCenterZ;           // offset 72
  float    _pad2;                // offset 76
  // Camera position in tile-local space (camera_ECEF − rtcCentre), cast to
  // float32.  Used for the view direction vector in lighting.
  float    cameraTilespaceX;     // offset 80
  float    cameraTilespaceY;     // offset 84
  float    cameraTilespaceZ;     // offset 88
  float    _pad3;                // offset 92
};                               // total 96 bytes

// ── Shader source ─────────────────────────────────────────────────────────────

static NSString* const kTerrainShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;

// ── Vertex shader ────────────────────────────────────────────────────────────
// RTC: vertex positions are stored relative to each tile's RTC centre (small
// floats).  The per-tile MVP matrix was computed on the CPU entirely in double
// precision, so the large camera↔centre translation is represented accurately.
struct PerDrawVertexUniforms {
  float4x4 mvpMatrix;  // per-tile MVP: P_d * R_d * T_d(rtcCentre − camera) → float
};

struct TerrainVOut {
  float4 position [[position]];
  float3 localPos;   // tile-local position (vertex − rtcCentre, float32)
  float  altitude;   // ellipsoid height metres from vertex attribute (double source)
  float2 uv;
};

vertex TerrainVOut terrainVertex(
    uint                              vid       [[vertex_id]],
    const device packed_float3*       pos       [[buffer(0)]],
    const device float*               altitudes [[buffer(1)]],
    constant PerDrawVertexUniforms&   u         [[buffer(2)]],
    const device packed_float2*       uvs       [[buffer(3)]])
{
  TerrainVOut o;
  float3 lp    = float3(pos[vid]);
  o.position   = u.mvpMatrix * float4(lp, 1.0f);
  o.localPos   = lp;
  o.altitude   = altitudes[vid];
  o.uv         = float2(uvs[vid]);
  return o;
}

// ── Fragment shader ──────────────────────────────────────────────────────────
struct OverlayParams {
  uint  hasOverlay;
  uint  isEllipsoidFallback;
  uint  isOnlyWater;
  uint  hasWaterMask;
  float translation[2];
  float scale[2];
  float wmWest, wmSouth, wmEast, wmNorth;
  float wmTransX, wmTransY;
  float wmScale;
  float _pad1;
  // RTC centre in ECEF (float).  wp = in.localPos + rtcCentre gives a world
  // position with ~0.75 m ULP (vs ~4 m for eyeRelPos + cameraEcef at altitude).
  float rtcCenterX, rtcCenterY, rtcCenterZ;
  float _pad2;
  // Camera position in tile-local space: camera_ECEF − rtcCentre.
  // Pre-computed on CPU in double, cast to float — used for lighting vd vector.
  float cameraTilespaceX, cameraTilespaceY, cameraTilespaceZ;
  float _pad3;
};

float3 hypsometricColor(float alt, float steep) {
  float3 cDeep=float3(.03f,.20f,.46f),cShallow=float3(.08f,.44f,.78f);
  float3 cLow=float3(.46f,.54f,.30f),cFoot=float3(.62f,.55f,.32f);
  float3 cHigh=float3(.48f,.38f,.28f),cAlp=float3(.52f,.50f,.48f),cSnow=float3(.90f,.92f,.94f);
  if(alt<0.f)return mix(cDeep,cShallow,smoothstep(-4500.f,-20.f,alt));
  float3 c=cLow;
  c=mix(c,cFoot,smoothstep(120.f,900.f,alt));
  c=mix(c,cHigh,smoothstep(750.f,2000.f,alt));
  c=mix(c,cAlp,smoothstep(1700.f,3600.f,alt));
  c=mix(c,cSnow,smoothstep(3200.f,5200.f,alt));
  float3 scree=float3(.38f,.35f,.32f);
  float rock=saturate(steep*1.08f)*smoothstep(35.f,140.f,alt);
  return mix(c,scree,rock);
}

fragment float4 terrainFragment(
    TerrainVOut              in           [[stage_in]],
    constant OverlayParams&  ov           [[buffer(0)]],
    texture2d<float>         overlayTex   [[texture(0)]],
    texture2d<float>         waterMaskTex [[texture(1)]])
{
  constexpr sampler s(filter::linear, address::clamp_to_edge);

  // ── World position (RTC reconstruction) ──────────────────────────────────
  // wp = tile-local position + RTC centre (both float32, sum at ~6.4 Mm).
  // Precision: ~0.75 m ULP — much better than eyeRelPos + cameraEcef which
  // had ~4 m ULP at typical camera altitudes.  Used for lat/lon computation
  // (water-mask UV) and lighting surface normal (up direction).
  float3 rtcCentre = float3(ov.rtcCenterX, ov.rtcCenterY, ov.rtcCenterZ);
  float3 wp  = in.localPos + rtcCentre;
  float  pxy = sqrt(wp.x*wp.x + wp.y*wp.y);
  float  lon = atan2(wp.y, wp.x);

  // Geodetic latitude (Bowring iteration in float32 — good to ~0.1 m for
  // water-mask UV, which is all we need here).
  const float a_e=6378137.f, b_e=6356752.31424518f;
  const float ep2=(a_e*a_e-b_e*b_e)/(b_e*b_e), e2=1.f-(b_e*b_e)/(a_e*a_e);
  float theta_g=atan2(wp.z*a_e, pxy*b_e);
  float sg=sin(theta_g), cg=cos(theta_g);
  float lat=atan2(wp.z+ep2*b_e*sg*sg*sg, pxy-e2*a_e*cg*cg*cg);

  // ── Water-mask UV ─────────────────────────────────────────────────────────
  // Cesium GEOGRAPHIC UV convention:
  //   U: 0=west … 1=east     (unchanged for Metal texture)
  //   V: 0=south … 1=north   (geographic, must be flipped for texture)
  float wmU   = (lon - ov.wmWest)  / max(ov.wmEast  - ov.wmWest,  1e-7f);
  float v_geo = (lat - ov.wmSouth) / max(ov.wmNorth - ov.wmSouth, 1e-7f);
  float2 geoUV = float2(wmU, v_geo) * ov.wmScale + float2(ov.wmTransX, ov.wmTransY);
  float2 wmUV  = clamp(float2(geoUV.x, 1.0f - geoUV.y), 0.0f, 1.0f);

  float waterVal = (ov.isOnlyWater != 0u)
      ? 1.0f
      : waterMaskTex.sample(s, wmUV).r;

  float3 lit;

  if (ov.hasOverlay != 0u) {
    float2 texUV = in.uv * float2(ov.scale[0], ov.scale[1])
                 + float2(ov.translation[0], ov.translation[1]);
    texUV.y = 1.0f - texUV.y;
    lit = overlayTex.sample(s, texUV).rgb;
  } else {
    // ── Lighting ──────────────────────────────────────────────────────────
    // Normals from screen-space derivatives of tile-local position.
    // (Using localPos rather than eyeRelPos makes no difference for the
    // direction of the normal — only the scale changes.)
    float3 dpdx = dfdx(in.localPos), dpdy = dfdy(in.localPos);
    float3 rawN  = cross(dpdx, dpdy);
    float  nLen  = length(rawN);
    float3 n     = (nLen > 1e-8f) ? rawN / nLen : float3(0.f, 0.f, 1.f);

    // View direction: camera−tile-centre minus vertex−tile-centre.
    float3 cameraLocal = float3(ov.cameraTilespaceX, ov.cameraTilespaceY, ov.cameraTilespaceZ);
    float3 vd = normalize(cameraLocal - in.localPos);
    if (dot(n, vd) < 0.f) n = -n;

    float3 gu  = normalize(wp);  // surface up (towards space)
    float3 sun = normalize(gu + float3(0.3f, 0.2f, 0.1f));
    float  diff  = saturate(dot(n, sun));
    float  amb   = 0.18f;
    float  rim   = saturate(1.f - dot(n, vd)) * 0.06f;
    float  steep = 1.f - saturate(abs(dot(n, gu)));

    // ── Altitude (from vertex attribute — double-precision source) ────────
    // In.altitude was computed on the CPU with Bowring formula in double
    // (sub-mm accuracy).  This completely avoids the ±50 m float32
    // catastrophic cancellation of the old  pxy/cos(lat) − N  in the shader.
    // The ellipsoid fallback is tessellated at −20 m → renders as ocean.
    float rawAlt = in.altitude;

    float3 oceanBlue = float3(0.04f, 0.26f, 0.52f);
    // When a real water-mask texture is present it controls ocean pixels via
    // waterVal, so clamp land altitude ≥ 0 to prevent residual float noise
    // from ocean vertices colouring land blue.  Without a water mask, use
    // raw altitude so bathymetric depth renders as ocean.
    float landAlt = (ov.hasWaterMask != 0u) ? max(rawAlt, 0.0f) : rawAlt;
    float3 landLit = hypsometricColor(landAlt, steep) * (amb + diff * 0.72f + rim);
    lit = mix(landLit, oceanBlue, waterVal);

    // 1 km lat/lon grid — fades when sub-pixel to suppress Moiré.
    float gridLat = 1000.f / 6371000.f;
    float cosLat  = max(cos(lat), 0.001f);
    float gridLon = gridLat / cosLat;
    float wLat = fwidth(lat) * 1.5f, wLon = fwidth(lon) * 1.5f;
    float gridVis = clamp(min(gridLat / max(wLat, 1e-9f),
                              gridLon / max(wLon, 1e-9f)), 0.f, 1.f);
    float latCell = lat - gridLat * floor(lat / gridLat);
    float lonCell = lon - gridLon * floor(lon / gridLon);
    float dLat = min(latCell, gridLat - latCell);
    float dLon = min(lonCell, gridLon - lonCell);
    float gridA = max(1.f - smoothstep(0.f, wLat, dLat),
                      1.f - smoothstep(0.f, wLon, dLon)) * gridVis;
    lit = mix(lit, float3(0.15f, 0.15f, 0.15f), gridA * 0.6f);
  }

  // Coastline outline from water mask (magenta for easy diagnostics).
  if (ov.isEllipsoidFallback == 0u) {
    float dWater  = fwidth(waterVal);
    float coastPx = abs(waterVal - 0.5f) / max(dWater * 0.5f, 0.01f);
    float coastA  = 1.f - smoothstep(0.f, 1.5f, coastPx);
    lit = mix(lit, float3(0.18f, 0.14f, 0.10f), coastA * 0.7f);
  }

  return float4(lit, 1.f);
}
)MSL";

static NSString* const kSkyShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SkyUniforms {
  float4x4 invVP;
  float4   cameraEcef; // xyz used, w=0
  float4   lightDir;   // xyz used, w=0
};
struct SkyVOut {
  float4 position [[position]];
  float2 ndc;
};

vertex SkyVOut skyVertex(uint vid [[vertex_id]],
                         constant SkyUniforms& u [[buffer(0)]])
{
  float2 ps[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
  SkyVOut o;
  o.position=float4(ps[vid],0,1);
  o.ndc=ps[vid];
  return o;
}

constant float ER=6371000.f,AR=ER+111000.f;
constant float3 RAY=float3(3.2e-6f,8.5e-6f,38e-6f),MIE3=float3(1.8e-6f,1.8e-6f,1.8e-6f);
constant float MG=.76f,LINT=18.f;
float2 rsi(float3 o,float3 d,float r){
  float a=dot(d,d),b=2*dot(d,o),c=dot(o,o)-r*r,disc=b*b-4*a*c;
  if(disc<0)return float2(-1);
  float sq=sqrt(disc);return float2((-b-sq)/(2*a),(-b+sq)/(2*a));
}
fragment float4 skyFragment(SkyVOut in [[stage_in]],
                            constant SkyUniforms& u [[buffer(0)]])
{
  float4 wn=u.invVP*float4(in.ndc,-1,1),wf=u.invVP*float4(in.ndc,1,1);
  float3 near3=wn.xyz/wn.w,far3=wf.xyz/wf.w;
  float3 rd=normalize(far3-near3);
  float3 org=u.cameraEcef.xyz;
  float2 at=rsi(org,rd,AR);
  if(at.y<0)return float4(0,0,0,1);
  float2 er=rsi(org,rd,ER);
  float tmax=at.y;if(er.x>0)tmax=min(tmax,er.x);
  float tmin=max(at.x,0.f);if(tmin>=tmax)return float4(0,0,0,1);
  const int S=16,L=4;
  float ss=(tmax-tmin)/float(S);
  float3 rayl=0,miel=0;float oR=0,oM=0;
  for(int i=0;i<S;++i){
    float t=tmin+(float(i)+.5f)*ss;
    float3 sp=org+rd*t;float h=length(sp)-ER;
    float hr=exp(-h/10000.f)*ss,hm=exp(-h/3200.f)*ss;
    oR+=hr;oM+=hm;
    float2 lh=rsi(sp,u.lightDir.xyz,AR);
    if(lh.y>0){
      float ls=lh.y/float(L);float lR=0,lM=0;bool bl=false;
      for(int j=0;j<L;++j){
        float3 lsp=sp+u.lightDir.xyz*((float(j)+.5f)*ls);
        float lh2=length(lsp)-ER;if(lh2<0){bl=true;break;}
        lR+=exp(-lh2/10000.f)*ls;lM+=exp(-lh2/3200.f)*ls;
      }
      if(!bl){float3 tau=RAY*(oR+lR)+MIE3*1.1f*(oM+lM);float3 att=exp(-tau);rayl+=hr*att;miel+=hm*att;}
    }
  }
  float cosT=dot(rd,u.lightDir.xyz),cos2=cosT*cosT;
  float rPh=.75f*(1+cos2);
  float g2=MG*MG,mPh=1.5f*((1-g2)/(2+g2))*(1+cos2)/pow(1+g2-2*MG*cosT,1.5f);
  float3 col=LINT*(rayl*RAY*rPh+miel*MIE3*mPh);
  col=1-exp(-col*1.1f);col*=float3(.18f,.45f,1.f);
  float3 nu=normalize(org);float up=saturate(dot(rd,nu));
  float camH=max(0.f,length(org)-ER);
  float hStr=saturate(1-camH/35000.f);
  col=mix(col,float3(.40f,.62f,.96f),(1-smoothstep(.05f,.45f,up))*.40f*hStr);
  return float4(col,1.f);
}
)MSL";

// ── Helper: ensure a persistent buffer is large enough, then memcpy into it ──
// Grows by doubling (rounded up to 4 KB) to amortise reallocation cost.
// Returns a non-owning bridge pointer to the buffer (ownership in *pBuf).
static id<MTLBuffer> ensureBuffer(void** pBuf, size_t* pCap,
                                   size_t needed, const void* data,
                                   id<MTLDevice> dev) {
  if (needed == 0) return nil;
  if (needed > *pCap) {
    size_t newCap = std::max(needed, *pCap > 0 ? *pCap * 2 : needed);
    newCap = (newCap + 4095UL) & ~4095UL; // round up to 4 KB page
    if (*pBuf) CFRelease(*pBuf);
    id<MTLBuffer> b = [dev newBufferWithLength:newCap
                                       options:MTLResourceStorageModeShared];
    *pBuf = (__bridge_retained void*)b;
    *pCap = newCap;
  }
  id<MTLBuffer> b = (__bridge id<MTLBuffer>)*pBuf;
  memcpy(b.contents, data, needed);
  return b;
}

namespace reactnativecesium {

MetalBackend::MetalBackend()
    : device_(nullptr), commandQueue_(nullptr), metalLayer_(nullptr),
      terrainPipeline_(nullptr), skyPipeline_(nullptr),
      terrainDepthState_(nullptr), skyDepthState_(nullptr),
      depthTexture_(nullptr), fallbackTexture_(nullptr),
      fallbackWaterMaskTexture_(nullptr),
      currentDrawable_(nullptr), currentCommandBuffer_(nullptr),
      currentEncoder_(nullptr),
      frameSemaphore_(nullptr), frameIndex_(0) {
  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    vtxBufs_[i] = idxBufs_[i] = uvBufs_[i] = altBufs_[i] = nullptr;
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = altCaps_[i] = 0;
  }
}

MetalBackend::~MetalBackend() { shutdown(); }

void MetalBackend::initialize(void* nativeSurface, int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;

  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  device_       = (__bridge_retained void*)dev;
  commandQueue_ = (__bridge_retained void*)[dev newCommandQueue];
  metalLayer_   = nativeSurface; // weak — owned by CAMetalLayer

  // Semaphore starts at kMaxFramesInFlight; each beginFrame decrements it and
  // each GPU completion handler increments it.
  dispatch_semaphore_t sem = dispatch_semaphore_create(kMaxFramesInFlight);
  frameSemaphore_ = (__bridge_retained void*)sem;

  CAMetalLayer* layer = (__bridge CAMetalLayer*)metalLayer_;
  layer.device        = dev;
  layer.framebufferOnly = YES;

  buildRenderPipelines();
  createDepthTexture();
  createFallbackTexture();
  createWaterMaskFallback();
}

void MetalBackend::buildRenderPipelines() {
  if (!device_) return;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;
  CAMetalLayer* layer = (__bridge CAMetalLayer*)metalLayer_;
  const MTLPixelFormat colorFmt = layer.pixelFormat;

  NSError* err = nil;

  if (terrainPipeline_) {
    CFRelease(terrainPipeline_);
    terrainPipeline_ = nullptr;
  }
  if (skyPipeline_) {
    CFRelease(skyPipeline_);
    skyPipeline_ = nullptr;
  }

  id<MTLLibrary> tLib = [dev newLibraryWithSource:kTerrainShaderSrc
                                          options:nil error:&err];
  if (err) NSLog(@"[MetalBackend] terrain shader error: %@", err);

  MTLRenderPipelineDescriptor* tDesc = [MTLRenderPipelineDescriptor new];
  tDesc.vertexFunction   = [tLib newFunctionWithName:@"terrainVertex"];
  tDesc.fragmentFunction = [tLib newFunctionWithName:@"terrainFragment"];
  tDesc.colorAttachments[0].pixelFormat = colorFmt;
  tDesc.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
  tDesc.rasterSampleCount               = (NSUInteger)std::max(1, sampleCount_);
  terrainPipeline_ = (__bridge_retained void*)
      [dev newRenderPipelineStateWithDescriptor:tDesc error:&err];
  if (err) NSLog(@"[MetalBackend] terrain pipeline error: %@", err);

  id<MTLLibrary> sLib = [dev newLibraryWithSource:kSkyShaderSrc
                                          options:nil error:&err];
  if (err) NSLog(@"[MetalBackend] sky shader error: %@", err);

  MTLRenderPipelineDescriptor* sDesc = [MTLRenderPipelineDescriptor new];
  sDesc.vertexFunction   = [sLib newFunctionWithName:@"skyVertex"];
  sDesc.fragmentFunction = [sLib newFunctionWithName:@"skyFragment"];
  sDesc.colorAttachments[0].pixelFormat = colorFmt;
  sDesc.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
  sDesc.rasterSampleCount               = (NSUInteger)std::max(1, sampleCount_);
  skyPipeline_ = (__bridge_retained void*)
      [dev newRenderPipelineStateWithDescriptor:sDesc error:&err];
  if (err) NSLog(@"[MetalBackend] sky pipeline error: %@", err);

  if (!terrainDepthState_) {
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionGreater;
    dd.depthWriteEnabled    = YES;
    terrainDepthState_ = (__bridge_retained void*)
        [dev newDepthStencilStateWithDescriptor:dd];
  }
  if (!skyDepthState_) {
    MTLDepthStencilDescriptor* sd = [MTLDepthStencilDescriptor new];
    sd.depthCompareFunction = MTLCompareFunctionAlways;
    sd.depthWriteEnabled    = NO;
    skyDepthState_ = (__bridge_retained void*)
        [dev newDepthStencilStateWithDescriptor:sd];
  }
}

void MetalBackend::setMsaaSampleCount(int sc) {
  int n = sc;
  if (n != 2 && n != 4) n = 1;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;
  if (dev && n > 1 && ![dev supportsTextureSampleCount:(NSUInteger)n]) n = 1;
  if (n == sampleCount_) return;

  if (frameSemaphore_) {
    dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)frameSemaphore_;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    }
  }

  sampleCount_ = n;
  buildRenderPipelines();
  createDepthTexture();

  if (frameSemaphore_) {
    dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)frameSemaphore_;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      dispatch_semaphore_signal(sem);
    }
  }
}

void MetalBackend::resize(int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;
  createDepthTexture();
}

void MetalBackend::shutdown() {
  // Drain in-flight frames: wait until all GPU completion handlers have fired
  // before releasing the semaphore and the persistent buffers they reference.
  if (frameSemaphore_) {
    dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)frameSemaphore_;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    }
    CFRelease(frameSemaphore_);
    frameSemaphore_ = nullptr;
  }

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (vtxBufs_[i]) { CFRelease(vtxBufs_[i]); vtxBufs_[i] = nullptr; }
    if (idxBufs_[i]) { CFRelease(idxBufs_[i]); idxBufs_[i] = nullptr; }
    if (uvBufs_[i])  { CFRelease(uvBufs_[i]);  uvBufs_[i]  = nullptr; }
    if (altBufs_[i]) { CFRelease(altBufs_[i]); altBufs_[i] = nullptr; }
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = altCaps_[i] = 0;
  }

  if (device_)                   { CFRelease(device_);                   device_                  = nullptr; }
  if (commandQueue_)             { CFRelease(commandQueue_);             commandQueue_            = nullptr; }
  if (terrainPipeline_)          { CFRelease(terrainPipeline_);          terrainPipeline_         = nullptr; }
  if (skyPipeline_)              { CFRelease(skyPipeline_);              skyPipeline_             = nullptr; }
  if (terrainDepthState_)        { CFRelease(terrainDepthState_);        terrainDepthState_       = nullptr; }
  if (skyDepthState_)            { CFRelease(skyDepthState_);            skyDepthState_           = nullptr; }
  if (depthTexture_)             { CFRelease(depthTexture_);             depthTexture_            = nullptr; }
  if (fallbackTexture_)          { CFRelease(fallbackTexture_);          fallbackTexture_         = nullptr; }
  if (fallbackWaterMaskTexture_) { CFRelease(fallbackWaterMaskTexture_); fallbackWaterMaskTexture_= nullptr; }
}

void MetalBackend::createDepthTexture() {
  if (viewportWidth_ <= 0 || viewportHeight_ <= 0 || !device_) return;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;

  if (depthTexture_) { CFRelease(depthTexture_); depthTexture_ = nullptr; }

  MTLTextureDescriptor* dd =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                         width:viewportWidth_
                                                        height:viewportHeight_
                                                     mipmapped:NO];
  const int sc = std::max(1, sampleCount_);
  if (sc > 1) {
    dd.textureType = MTLTextureType2DMultisample;
    dd.sampleCount = (NSUInteger)sc;
  }
  dd.storageMode = MTLStorageModePrivate;
  dd.usage       = MTLTextureUsageRenderTarget;
  depthTexture_  = (__bridge_retained void*)[dev newTextureWithDescriptor:dd];
}

void MetalBackend::createFallbackTexture() {
  if (!device_) return;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;

  MTLTextureDescriptor* td =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  td.usage       = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

  const uint8_t white[4] = {255, 255, 255, 255};
  [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
         mipmapLevel:0
           withBytes:white
         bytesPerRow:4];
  fallbackTexture_ = (__bridge_retained void*)tex;
}

void MetalBackend::createWaterMaskFallback() {
  if (!device_) return;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;

  if (fallbackWaterMaskTexture_) {
    CFRelease(fallbackWaterMaskTexture_);
    fallbackWaterMaskTexture_ = nullptr;
  }

  MTLTextureDescriptor* td =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  td.usage       = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

  // All-zero = land; fwidth(0.0) = 0.0 so coastA = 0 for solid-land tiles.
  const uint8_t zero[4] = {0, 0, 0, 255};
  [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
         mipmapLevel:0
           withBytes:zero
         bytesPerRow:4];
  fallbackWaterMaskTexture_ = (__bridge_retained void*)tex;
}

void* MetalBackend::createRasterTexture(const uint8_t* pixels,
                                         int32_t width,
                                         int32_t height) {
  if (!device_ || !pixels || width <= 0 || height <= 0) return nullptr;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;

  MTLTextureDescriptor* td =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:(NSUInteger)width
                                                        height:(NSUInteger)height
                                                     mipmapped:NO];
  td.usage       = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;

  id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
  if (!tex) return nullptr;

  [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
         mipmapLevel:0
           withBytes:pixels
         bytesPerRow:(NSUInteger)(width * 4)];

  return (__bridge_retained void*)tex;
}

// ── Per-frame ─────────────────────────────────────────────────────────────────

void MetalBackend::beginFrame(const FrameParams& /*params*/) {
  // Acquire a frame slot.  Blocks if kMaxFramesInFlight frames are already
  // queued on the GPU.  The completion handler in endFrame() signals the
  // semaphore to release the slot.
  dispatch_semaphore_wait((__bridge dispatch_semaphore_t)frameSemaphore_,
                          DISPATCH_TIME_FOREVER);
  frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;

  CAMetalLayer* layer    = (__bridge CAMetalLayer*)metalLayer_;
  id<MTLCommandQueue> cq = (__bridge id<MTLCommandQueue>)commandQueue_;

  // Sync depth texture with actual drawable size if it changed.
  CGSize drawSize = layer.drawableSize;
  int dw = (int)drawSize.width, dh = (int)drawSize.height;
  if (dw > 0 && dh > 0) {
    bool needsCreate = !depthTexture_;
    if (depthTexture_) {
      id<MTLTexture> dt = (__bridge id<MTLTexture>)depthTexture_;
      needsCreate = ((int)dt.width != dw || (int)dt.height != dh ||
                     (int)dt.sampleCount != std::max(1, sampleCount_));
    }
    if (needsCreate) {
      viewportWidth_  = dw;
      viewportHeight_ = dh;
      createDepthTexture();
    }
  }

  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (!drawable) {
    // No drawable available — restore semaphore slot and bail.
    dispatch_semaphore_signal((__bridge dispatch_semaphore_t)frameSemaphore_);
    currentDrawable_ = nullptr;
    currentCommandBuffer_ = nullptr;
    return;
  }
  currentDrawable_ = (__bridge_retained void*)drawable;

  id<MTLCommandBuffer> cb = [cq commandBuffer];
  currentCommandBuffer_ = (__bridge_retained void*)cb;

  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
  rp.colorAttachments[0].texture     = drawable.texture;
  rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  rp.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);

  if (depthTexture_) {
    rp.depthAttachment.texture     = (__bridge id<MTLTexture>)depthTexture_;
    rp.depthAttachment.loadAction  = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth  = 0.0; // reversed-Z: 0 = far
  }

  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:rp];
  currentEncoder_ = (__bridge_retained void*)enc;
}

void MetalBackend::drawScene(const FrameResult& frame) {
  if (!currentEncoder_ || !currentCommandBuffer_ || !currentDrawable_) return;

  id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)currentEncoder_;
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;

  // ── Sky pass ──────────────────────────────────────────────────────────────
  if (skyPipeline_ && skyDepthState_) {
    SkyUniforms skyU{};
    const float* iv = glm::value_ptr(frame.invVP);
    for (int i = 0; i < 16; ++i) skyU.invVP[i] = iv[i];
    skyU.cameraEcef[0] = frame.cameraEcef.x;
    skyU.cameraEcef[1] = frame.cameraEcef.y;
    skyU.cameraEcef[2] = frame.cameraEcef.z;
    float cl = sqrtf(frame.cameraEcef.x * frame.cameraEcef.x +
                     frame.cameraEcef.y * frame.cameraEcef.y +
                     frame.cameraEcef.z * frame.cameraEcef.z);
    if (cl > 0.0f) {
      skyU.lightDir[0] = frame.cameraEcef.x / cl;
      skyU.lightDir[1] = frame.cameraEcef.y / cl;
      skyU.lightDir[2] = frame.cameraEcef.z / cl;
    }

    [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)skyPipeline_];
    [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)skyDepthState_];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBytes:&skyU length:sizeof(skyU) atIndex:0];
    [enc setFragmentBytes:&skyU length:sizeof(skyU) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  // ── Terrain pass ──────────────────────────────────────────────────────────
  if (frame.draws.empty() || frame.localPositions.empty() || frame.indices.empty() ||
      !terrainPipeline_ || !terrainDepthState_) {
    return;
  }

  const int fi = frameIndex_;

  // Upload merged geometry to the current frame's persistent Metal buffers.
  const size_t vtxBytes = frame.localPositions.size() * sizeof(float);
  const size_t idxBytes = frame.indices.size()        * sizeof(uint32_t);
  const size_t uvBytes  = frame.uvs.size()            * sizeof(float);
  const size_t altBytes = frame.altitudes.size()      * sizeof(float);

  id<MTLBuffer> vtxBuf = ensureBuffer(&vtxBufs_[fi], &vtxCaps_[fi],
                                       vtxBytes, frame.localPositions.data(), dev);
  id<MTLBuffer> idxBuf = ensureBuffer(&idxBufs_[fi], &idxCaps_[fi],
                                       idxBytes, frame.indices.data(), dev);
  id<MTLBuffer> uvBuf  = ensureBuffer(&uvBufs_[fi],  &uvCaps_[fi],
                                       uvBytes,  frame.uvs.data(), dev);
  id<MTLBuffer> altBuf = ensureBuffer(&altBufs_[fi], &altCaps_[fi],
                                       altBytes, frame.altitudes.data(), dev);

  if (!vtxBuf || !idxBuf || !altBuf) return;

  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)terrainPipeline_];
  [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)terrainDepthState_];
  [enc setFrontFacingWinding:MTLWindingCounterClockwise];
  [enc setCullMode:MTLCullModeBack];

  // Set vertex buffers that are shared across all draws.
  // buffer(0): tile-local positions, buffer(1): altitudes, buffer(3): UVs.
  // buffer(2) is the per-draw MVP set in the loop below.
  [enc setVertexBuffer:vtxBuf offset:0 atIndex:0];
  [enc setVertexBuffer:altBuf offset:0 atIndex:1];
  if (uvBuf) [enc setVertexBuffer:uvBuf offset:0 atIndex:3];

  id<MTLTexture> fallbackTex   = (__bridge id<MTLTexture>)fallbackTexture_;
  id<MTLTexture> fallbackWMTex = (__bridge id<MTLTexture>)fallbackWaterMaskTexture_;

  if (fallbackWMTex) [enc setFragmentTexture:fallbackWMTex atIndex:1];

  for (const auto& draw : frame.draws) {
    if (draw.indexCount == 0) continue;

    // ── Per-draw vertex uniforms: the tile's RTC MVP matrix ───────────────
    PerDrawVertexUniformsCPP vdu{};
    const float* mp = glm::value_ptr(draw.mvpMatrix);
    for (int i = 0; i < 16; ++i) vdu.mvpMatrix[i] = mp[i];
    [enc setVertexBytes:&vdu length:sizeof(vdu) atIndex:2];

    // ── Per-draw fragment uniforms ─────────────────────────────────────────
    OverlayParamsCPP ov{};
    ov.hasOverlay          = (draw.overlayTexture && draw.hasUVs) ? 1u : 0u;
    ov.isEllipsoidFallback = draw.isEllipsoidFallback ? 1u : 0u;
    ov.isOnlyWater         = draw.isOnlyWater         ? 1u : 0u;
    ov.hasWaterMask        = draw.waterMaskTexture     ? 1u : 0u;
    ov.translation[0]      = draw.overlayTranslation.x;
    ov.translation[1]      = draw.overlayTranslation.y;
    ov.scale[0]            = draw.overlayScale.x;
    ov.scale[1]            = draw.overlayScale.y;
    ov.wmWest              = draw.wmTileBounds.x;
    ov.wmSouth             = draw.wmTileBounds.y;
    ov.wmEast              = draw.wmTileBounds.z;
    ov.wmNorth             = draw.wmTileBounds.w;
    ov.wmTransX            = draw.wmTranslation.x;
    ov.wmTransY            = draw.wmTranslation.y;
    ov.wmScale             = draw.wmScale;
    // RTC: tile centre and camera-in-tile-space for fragment world-position
    // reconstruction and view-direction lighting.
    ov.rtcCenterX          = draw.rtcCenterEcef.x;
    ov.rtcCenterY          = draw.rtcCenterEcef.y;
    ov.rtcCenterZ          = draw.rtcCenterEcef.z;
    // cameraTilespace = cameraEcef − rtcCentre.  Calculated here to keep the
    // fragment shader free of subtraction of two large float3 values.
    ov.cameraTilespaceX    = frame.cameraEcef.x - draw.rtcCenterEcef.x;
    ov.cameraTilespaceY    = frame.cameraEcef.y - draw.rtcCenterEcef.y;
    ov.cameraTilespaceZ    = frame.cameraEcef.z - draw.rtcCenterEcef.z;
    [enc setFragmentBytes:&ov length:sizeof(ov) atIndex:0];

    id<MTLTexture> tex = (draw.overlayTexture && draw.hasUVs)
        ? (__bridge id<MTLTexture>)draw.overlayTexture
        : fallbackTex;
    if (tex) [enc setFragmentTexture:tex atIndex:0];

    id<MTLTexture> wmTex = draw.waterMaskTexture
        ? (__bridge id<MTLTexture>)draw.waterMaskTexture
        : fallbackWMTex;
    if (wmTex) [enc setFragmentTexture:wmTex atIndex:1];

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:draw.indexCount
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:idxBuf
             indexBufferOffset:draw.indexByteOffset];
  }
}

void MetalBackend::endFrame() {
  if (!currentCommandBuffer_ || !currentDrawable_) return;

  id<MTLRenderCommandEncoder> enc =
      (__bridge_transfer id<MTLRenderCommandEncoder>)currentEncoder_;
  [enc endEncoding];
  currentEncoder_ = nullptr;

  id<CAMetalDrawable> drawable =
      (__bridge_transfer id<CAMetalDrawable>)currentDrawable_;
  id<MTLCommandBuffer> cb =
      (__bridge_transfer id<MTLCommandBuffer>)currentCommandBuffer_;

  // Signal the frame semaphore when the GPU is done with this frame's buffers.
  void* semPtr = frameSemaphore_;
  [cb addCompletedHandler:^(id<MTLCommandBuffer> __unused) {
    dispatch_semaphore_signal((__bridge dispatch_semaphore_t)semPtr);
  }];

  [cb presentDrawable:drawable];
  [cb commit];
  currentDrawable_      = nullptr;
  currentCommandBuffer_ = nullptr;
}

} // namespace reactnativecesium
