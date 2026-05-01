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

// Camera smoothing constants — shared between platform bridges so jank /
// motion feel is identical on iOS and Android.
inline constexpr double kSmoothAltitude     = 25.0;
inline constexpr double kSmoothHeading      = 30.0;
inline constexpr double kSmoothPitch        = 50.0;
inline constexpr double kSmoothRoll         = 50.0;
inline constexpr double kSmoothViewCorr     = 50.0;
inline constexpr double kEpsLatLon          = 1e-7;
inline constexpr double kEpsAltitudeMeters  = 0.1;
inline constexpr double kEpsAngleDeg        = 0.05;
inline constexpr double kEpsQuatDot         = 1e-8;

// Metrics throttle: emit telemetry every N rendered frames.
inline constexpr int kMetricsEmitEveryFrames = 20;

// Idle-probe interval (seconds): when the engine is idle, periodically tick
// to catch late tile completions without redundant full draws.
inline constexpr double kIdleProbeSeconds = 0.25;

} // namespace tunables
} // namespace reactnativecesium
