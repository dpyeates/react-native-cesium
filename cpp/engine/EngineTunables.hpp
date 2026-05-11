#pragma once

#include <cstddef>
#include <cstdint>

namespace reactnativecesium {

// Centralised default values that previously lived as anonymous magic numbers
// scattered across the engine and platform backends. Keep this header tiny and
// dependency-free so it is safe to include from any translation unit.
namespace tunables {

// CPU pipeline stages allowed to be in flight at once on the GPU. Both Metal
// (triple-buffered MTLBuffer slots) and Vulkan (in-flight fences / command
// buffers) use this constant as their core depth, so changing it here
// propagates to every backend.
inline constexpr int kMaxFramesInFlight = 3;

// Upper bound on simultaneously live raster overlay textures held by the
// Vulkan descriptor pool. Cesium can cache many hundreds of tiles at high
// zoom; 2048 gives substantial headroom without exhausting GPU descriptor
// state.
inline constexpr int kMaxRasterTextures = 2048;

// Cesium Native tileset cache (decoded geometry / textures kept in RAM).
inline constexpr int64_t kDefaultMaxCachedBytes = 256LL * 1024LL * 1024LL;

// SqliteCache row budget — number of cached HTTP responses Cesium keeps on
// disk between sessions. 4k rows is a reasonable phone default; tablets can
// happily go to 16k.
inline constexpr int32_t kDefaultSqliteCacheMaxRows = 4096;

// FrameResult initial reservations (CPU-side merged geometry buffer) used to
// avoid reallocations during the first few frames of a session. The buffers
// are then re-used (only re-grown if visible geometry exceeds capacity).
inline constexpr std::size_t kFrameResultPositionFloatReserve = 512u * 1024u;
inline constexpr std::size_t kFrameResultIndexUint32Reserve   = 3u * 1024u * 1024u;

// Tileset selection knobs (mirrored to TilesetOptions defaults).
inline constexpr double  kDefaultMaximumScreenSpaceError      = 32.0;
inline constexpr int32_t kDefaultMaximumSimultaneousTileLoads = 12;
inline constexpr int32_t kDefaultLoadingDescendantLimit       = 20;

// Camera smoothing constants — shared between platform bridges so motion
// feel is identical on iOS and Android.
//
// Every scalar DoF (lat, lon, alt, heading, pitch, roll) is driven by an
// α-β (constant-velocity predictor) tracker inside CameraIntegrator:
//
//   on measurement z at time t_z:
//     pred       = value + velocity * (t_z - t_state)
//     residual   = wrap(z - pred)                  // wrap only for angles
//     value     += α * residual
//     velocity  += (β / max(t_z - t_state, ε)) * residual
//
//   on render frame at time t_now:
//     value     += velocity * dt                   // extrapolate
//     // optional per-DoF rate cap applied here
//
// α controls how hard each measurement pulls the value (responsiveness).
// β controls how aggressively velocity is corrected (anticipation). With
// α ≈ 0.5–0.6 / β ≈ 0.05 every DoF stays smooth on a 1 Hz GPS feed (zero
// steady-state lag at constant motion) while remaining instant under a
// 60 Hz gesture-driven worklet.
//
// `viewCorrection` (a unit quaternion) uses a single-coefficient SLERP toward
// the latest demand each frame — there is no useful velocity concept for it.
// `verticalFovDeg` is a 1-D scalar with α only (no velocity term).
inline constexpr double kAlphaPos    = 0.6;   inline constexpr double kBetaPos    = 0.05;
inline constexpr double kAlphaAlt    = 0.6;   inline constexpr double kBetaAlt    = 0.05;
inline constexpr double kAlphaHdg    = 0.55;  inline constexpr double kBetaHdg    = 0.05;
inline constexpr double kAlphaPitch  = 0.75;  inline constexpr double kBetaPitch  = 0.05;
inline constexpr double kAlphaRoll   = 0.75;  inline constexpr double kBetaRoll   = 0.05;
inline constexpr double kAlphaVfov     = 0.4;   // no velocity term
inline constexpr double kAlphaViewCorr = 0.5;   // SLERP toward target

// Convergence epsilons (used to decide when the integrator has settled and
// the render loop can drop back to idle).
inline constexpr double kEpsLatLon          = 1e-7;
inline constexpr double kEpsAltitudeMeters  = 0.1;
inline constexpr double kEpsAngleDeg        = 0.05;
inline constexpr double kEpsQuatDot         = 1e-8;

// Per-DoF rate caps (per second). 0 = uncapped. Hard ceilings; the integrator
// clamps the extrapolated velocity to these on every render tick. Useful to
// reject a single noisy IMU sample without making the camera spin or pitch up
// at hundreds of degrees per second. Default values are deliberately generous.
inline constexpr double kMaxYawRateDegSec   = 360.0;
inline constexpr double kMaxPitchRateDegSec = 360.0;
inline constexpr double kMaxRollRateDegSec  = 720.0;
inline constexpr double kMaxClimbRateMps    = 0.0;
inline constexpr double kMaxGroundSpeedMps  = 0.0;

// Render-step dt clamp (catch-up after a long pause / app foreground). Without
// this, the first render after a multi-second sleep would extrapolate a long
// way along the last known velocity vector and visibly jump.
inline constexpr double kMaxFrameDtSec = 0.1;

// Velocity bleed for bursty inputs. Without this, a gesture-driven feed (a
// worklet that emits at 60 Hz during the pan and stops emitting on release,
// or a joystick that pushes rates only while held) would leave the
// integrator extrapolating along the last learned velocity forever — there
// is no measurement-end signal in the per-DoF API.
//
// Each scalar DoF tracks an EWMA of inter-arrival times between its
// measurements. While measurements keep arriving on schedule, silence is
// small relative to the mean and velocity is extrapolated normally. Once
// silence exceeds the grace window the velocity bleeds exponentially with a
// time-constant proportional to the same mean interval — fast for gestures
// (mean ≈ 16 ms → τ ≈ 32 ms), slow for 1 Hz GPS (mean ≈ 1 s → τ ≈ 2 s, but
// silence rarely exceeds grace before the next fix arrives so the glide is
// untouched).
//
// silence > kVelocitySilenceGraceFactor * meanIntervalSec → bleed active.
// velocity *= exp(-frameDt / (kVelocityBleedTauFactor * meanIntervalSec))
inline constexpr double kVelocitySilenceGraceFactor = 1.5;
inline constexpr double kVelocityBleedTauFactor     = 2.0;
// EWMA blend coefficient for the inter-arrival mean. Higher = faster adapt
// to cadence changes (e.g. user switches from gesture to sparse GPS feed).
inline constexpr double kIntervalEwmaAlpha          = 0.3;
// Demand-pull time constant (seconds). When measurements have stopped
// arriving (silence > grace), the actual value is gently pulled toward
// the last demand at this rate. This guarantees convergence for one-shot
// API calls (e.g. setAltitude from a button) without affecting continuous
// sensor or gesture streams — those always deliver a new measurement before
// the grace window expires, so this term is never active. 0 = disabled.
inline constexpr double kDemandPullTauSec           = 0.8;

// Optional outlier rejection (off if 0). When > 0, a single measurement whose
// residual against the predicted state exceeds the threshold is dropped (and
// logged in debug builds). For deliberate jumps the consumer should call
// teleport() instead. Disabled by default — easy to enable per-deployment.
inline constexpr double kOutlierLatLonDeg   = 0.0;
inline constexpr double kOutlierAltMeters   = 0.0;
inline constexpr double kOutlierHeadingDeg  = 0.0;

// Metrics throttle: emit telemetry every N rendered frames.
inline constexpr int kMetricsEmitEveryFrames = 20;

// Minimum interval between onActualCamera callback emissions (seconds).
// 0.2 s = 5 Hz — fast enough for HUD alignment, light on the JS bridge.
inline constexpr double kActualCameraCallbackIntervalSec = 0.2;

// Change-detection thresholds for onActualCamera.
// A new snapshot is only dispatched to JS when at least one field has moved
// by more than the corresponding epsilon since the last emission.
inline constexpr double kActualCameraDeltaLatLonDeg = 1e-7;   // ~1 cm at equator
inline constexpr double kActualCameraDeltaAltMeters = 0.05;   // 5 cm
inline constexpr double kActualCameraDeltaAngleDeg  = 0.01;   // heading / pitch / roll
inline constexpr double kActualCameraDeltaFovDeg    = 0.01;   // vertical FoV

// Idle-probe interval (seconds): when the engine is idle, periodically tick
// to catch late tile completions without redundant full draws.
inline constexpr double kIdleProbeSeconds = 0.25;

} // namespace tunables
} // namespace reactnativecesium
