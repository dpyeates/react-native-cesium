// MetalBackend.mm — Metal rendering backend.
//
// Architecture:
//   • Eye-relative float positions computed on CPU (double precision).
//   • Single merged vertex + index buffer uploaded each frame.
//   • Reversed-Z infinite projection: depth clear = 0, compare = GREATER.
//   • Sky drawn first (no depth write), terrain drawn after.
//   • Imagery overlays: UV buffer alongside positions; each draw primitive can
//     bind an optional overlay MTLTexture sampled in the shader.
//   • Triple-buffered persistent MTLBuffers: one slot per in-flight frame,
//     guarded by a dispatch_semaphore.  This eliminates per-frame GPU-memory
//     allocation while keeping CPU-GPU safety.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include "MetalBackend.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring> // memcpy
#include <algorithm> // std::max

// Uniforms — float3 in MSL is 16-byte aligned (same as float4), so we always
// use float[4] on the C++ side to keep sizes identical.
struct TerrainUniforms {
  float vpMatrix[16];   // column-major float4x4  (64 bytes)
  float cameraEcef[4];  // xyz used, w=0          (16 bytes)
};                      // total 80 bytes

struct SkyUniforms {
  float invVP[16];      // column-major float4x4  (64 bytes)
  float cameraEcef[4];  // xyz used, w=0          (16 bytes)
  float lightDir[4];    // xyz used, w=0          (16 bytes)
};                      // total 96 bytes

// ── Shader source ─────────────────────────────────────────────────────────────

static NSString* const kTerrainShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct TerrainUniforms {
  float4x4 vpMatrix;
  float4   cameraEcef; // xyz used, w=0 — float4 avoids float3 alignment surprise
};
struct TerrainVOut {
  float4 position [[position]];
  float3 eyeRelPos;
  float2 uv;           // overlay UV coordinates
};

float ellipsoidHeightMeters(float3 p) {
  const float a=6378137.f, b=6356752.31424518f;
  const float e2=1.f-(b*b)/(a*a), ep2=(a*a-b*b)/(b*b);
  float pxy=sqrt(p.x*p.x+p.y*p.y);
  if(pxy<1e-6f)return abs(p.z)-b;
  float theta=atan2(p.z*a,pxy*b);
  float st=sin(theta),ct=cos(theta);
  float lat=atan2(p.z+ep2*b*st*st*st,pxy-e2*a*ct*ct*ct);
  float sinLat=sin(lat);
  float N=a/sqrt(1.f-e2*sinLat*sinLat);
  return pxy/cos(lat)-N;
}

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

vertex TerrainVOut terrainVertex(
    uint vid [[vertex_id]],
    const device packed_float3* pos  [[buffer(0)]],
    constant TerrainUniforms& u      [[buffer(1)]],
    const device packed_float2* uvs  [[buffer(2)]])
{
  TerrainVOut o;
  float3 ep=float3(pos[vid]);
  o.position=u.vpMatrix*float4(ep,1.f);
  o.eyeRelPos=ep;
  o.uv=float2(uvs[vid]);
  return o;
}

fragment float4 terrainFragment(
    TerrainVOut in                      [[stage_in]],
    constant TerrainUniforms& u         [[buffer(0)]],
    constant uint& hasOverlay           [[buffer(1)]],
    texture2d<float> overlayTex         [[texture(0)]])
{
  if (hasOverlay != 0u) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return overlayTex.sample(s, in.uv);
  }
  float3 dpdx=dfdx(in.eyeRelPos), dpdy=dfdy(in.eyeRelPos);
  float3 rawN=cross(dpdx,dpdy);
  float nLen=length(rawN);
  float3 n=(nLen>1e-8f)?rawN/nLen:float3(0,0,1);
  float3 wp=in.eyeRelPos+u.cameraEcef.xyz;
  float3 vd=normalize(-in.eyeRelPos);
  if(dot(n,vd)<0.f)n=-n;
  float3 gu=normalize(wp);
  float3 sun=normalize(gu+float3(.3f,.2f,.1f));
  float diff=saturate(dot(n,sun));
  float amb=.18f, rim=saturate(1.f-dot(n,vd))*.06f;
  float alt=ellipsoidHeightMeters(wp);
  float steep=1.f-saturate(abs(dot(n,gu)));
  float3 base=hypsometricColor(alt,steep);
  return float4(base*(amb+diff*.72f+rim),1.f);
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
      currentDrawable_(nullptr), currentCommandBuffer_(nullptr),
      currentEncoder_(nullptr),
      frameSemaphore_(nullptr), frameIndex_(0) {
  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    vtxBufs_[i] = idxBufs_[i] = uvBufs_[i] = nullptr;
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = 0;
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

  const MTLPixelFormat colorFmt = layer.pixelFormat;

  // ── Terrain pipeline ──────────────────────────────────────────────────────
  NSError* err = nil;
  id<MTLLibrary> tLib = [dev newLibraryWithSource:kTerrainShaderSrc
                                          options:nil error:&err];
  if (err) NSLog(@"[MetalBackend] terrain shader error: %@", err);

  MTLRenderPipelineDescriptor* tDesc = [MTLRenderPipelineDescriptor new];
  tDesc.vertexFunction   = [tLib newFunctionWithName:@"terrainVertex"];
  tDesc.fragmentFunction = [tLib newFunctionWithName:@"terrainFragment"];
  tDesc.colorAttachments[0].pixelFormat = colorFmt;
  tDesc.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
  terrainPipeline_ = (__bridge_retained void*)
      [dev newRenderPipelineStateWithDescriptor:tDesc error:&err];
  if (err) NSLog(@"[MetalBackend] terrain pipeline error: %@", err);

  // ── Sky pipeline ──────────────────────────────────────────────────────────
  id<MTLLibrary> sLib = [dev newLibraryWithSource:kSkyShaderSrc
                                          options:nil error:&err];
  if (err) NSLog(@"[MetalBackend] sky shader error: %@", err);

  MTLRenderPipelineDescriptor* sDesc = [MTLRenderPipelineDescriptor new];
  sDesc.vertexFunction   = [sLib newFunctionWithName:@"skyVertex"];
  sDesc.fragmentFunction = [sLib newFunctionWithName:@"skyFragment"];
  sDesc.colorAttachments[0].pixelFormat = colorFmt;
  sDesc.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
  skyPipeline_ = (__bridge_retained void*)
      [dev newRenderPipelineStateWithDescriptor:sDesc error:&err];
  if (err) NSLog(@"[MetalBackend] sky pipeline error: %@", err);

  // ── Depth states ──────────────────────────────────────────────────────────
  MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
  dd.depthCompareFunction = MTLCompareFunctionGreater; // reversed-Z
  dd.depthWriteEnabled    = YES;
  terrainDepthState_ = (__bridge_retained void*)
      [dev newDepthStencilStateWithDescriptor:dd];

  MTLDepthStencilDescriptor* sd = [MTLDepthStencilDescriptor new];
  sd.depthCompareFunction = MTLCompareFunctionAlways;
  sd.depthWriteEnabled    = NO;
  skyDepthState_ = (__bridge_retained void*)
      [dev newDepthStencilStateWithDescriptor:sd];

  createDepthTexture();
  createFallbackTexture();
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
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = 0;
  }

  if (device_)            { CFRelease(device_);            device_           = nullptr; }
  if (commandQueue_)      { CFRelease(commandQueue_);      commandQueue_     = nullptr; }
  if (terrainPipeline_)   { CFRelease(terrainPipeline_);   terrainPipeline_  = nullptr; }
  if (skyPipeline_)       { CFRelease(skyPipeline_);       skyPipeline_      = nullptr; }
  if (terrainDepthState_) { CFRelease(terrainDepthState_); terrainDepthState_= nullptr; }
  if (skyDepthState_)     { CFRelease(skyDepthState_);     skyDepthState_    = nullptr; }
  if (depthTexture_)      { CFRelease(depthTexture_);      depthTexture_     = nullptr; }
  if (fallbackTexture_)   { CFRelease(fallbackTexture_);   fallbackTexture_  = nullptr; }
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
      needsCreate = ((int)dt.width != dw || (int)dt.height != dh);
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
  if (frame.draws.empty() || frame.eyeRelPositions.empty() || frame.indices.empty() ||
      !terrainPipeline_ || !terrainDepthState_) {
    return;
  }

  const int fi = frameIndex_;

  // Write CPU data into the current frame's persistent Metal buffers.
  // ensureBuffer grows the buffer if needed (amortised doubling) without
  // allocating a new MTLBuffer on every frame once at steady-state.
  const size_t vtxBytes = frame.eyeRelPositions.size() * sizeof(float);
  const size_t idxBytes = frame.indices.size()         * sizeof(uint32_t);
  const size_t uvBytes  = frame.uvs.size()             * sizeof(float);

  id<MTLBuffer> vtxBuf = ensureBuffer(&vtxBufs_[fi], &vtxCaps_[fi],
                                       vtxBytes, frame.eyeRelPositions.data(), dev);
  id<MTLBuffer> idxBuf = ensureBuffer(&idxBufs_[fi], &idxCaps_[fi],
                                       idxBytes, frame.indices.data(), dev);
  id<MTLBuffer> uvBuf  = ensureBuffer(&uvBufs_[fi],  &uvCaps_[fi],
                                       uvBytes,  frame.uvs.data(), dev);

  if (!vtxBuf || !idxBuf) return;

  TerrainUniforms terrU{};
  const float* vp = glm::value_ptr(frame.vpMatrix);
  for (int i = 0; i < 16; ++i) terrU.vpMatrix[i] = vp[i];
  terrU.cameraEcef[0] = frame.cameraEcef.x;
  terrU.cameraEcef[1] = frame.cameraEcef.y;
  terrU.cameraEcef[2] = frame.cameraEcef.z;

  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)terrainPipeline_];
  [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)terrainDepthState_];
  [enc setCullMode:MTLCullModeNone];

  [enc setVertexBuffer:vtxBuf offset:0 atIndex:0];
  [enc setVertexBytes:&terrU length:sizeof(terrU) atIndex:1];
  if (uvBuf) [enc setVertexBuffer:uvBuf offset:0 atIndex:2];
  [enc setFragmentBytes:&terrU length:sizeof(terrU) atIndex:0];

  id<MTLTexture> fallbackTex = (__bridge id<MTLTexture>)fallbackTexture_;

  for (const auto& draw : frame.draws) {
    if (draw.indexCount == 0) continue;

    const uint32_t hasOverlay =
        (draw.overlayTexture && draw.hasUVs) ? 1u : 0u;
    [enc setFragmentBytes:&hasOverlay length:sizeof(uint32_t) atIndex:1];

    id<MTLTexture> tex = (draw.overlayTexture && draw.hasUVs)
        ? (__bridge id<MTLTexture>)draw.overlayTexture
        : fallbackTex;
    if (tex) [enc setFragmentTexture:tex atIndex:0];

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
