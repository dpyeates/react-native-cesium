import type {
  HybridView,
  HybridViewProps,
  HybridViewMethods,
} from 'react-native-nitro-modules'

/**
 * Snapshot of the native camera for persistence / sync with React state.
 * Returned by `getActualCamera()` and `getDemandCamera()`.
 */
export interface CameraState {
  latitude: number
  longitude: number
  altitude: number
  heading: number
  pitch: number
  roll: number
  verticalFovDeg: number
}

/**
 * Unit quaternion (w + xi + yj + zk). Used for camera-space view correction
 * applied after HPR (boresight / HUD alignment). Non-unit values are
 * normalised on the native side.
 */
export interface Quaternion {
  w: number
  x: number
  y: number
  z: number
}

/** Throttled telemetry for dashboards (see `onMetrics` prop). */
export interface CesiumMetrics {
  fps: number
  tilesRendered: number
  tilesLoading: number
  tilesVisited: number
  ionTokenConfigured: boolean
  /** True when a CA bundle has been resolved for libcurl TLS. */
  tlsConfigured: boolean
  tilesetReady: boolean
  creditsPlainText: string
}

export interface CesiumViewProps extends HybridViewProps {
  ionAccessToken: string
  ionAssetId: number
  /**
   * Initial camera applied via `teleport(...)` once when the native bridge is
   * created. Subsequent prop writes are ignored — use the per-DoF setters or
   * `teleport(...)` for runtime updates.
   */
  initialCamera: CameraState
  pauseRendering: boolean
  maximumScreenSpaceError: number
  maximumSimultaneousTileLoads: number
  loadingDescendantLimit: number
  /** 1 = off; 2 or 4 = MSAA when supported. */
  msaaSampleCount: number
  ionImageryAssetId: number

  // ── Optional perf / quality knobs ────────────────────────────────────────
  /** RAM budget for live tileset, in mebibytes. Default 256. */
  maximumCachedMiB?: number
  /** Pre-load ancestor tiles around the visible set (smoother panning, more loads). Default true. */
  preloadAncestors?: boolean
  /** Pre-load sibling tiles around the visible set. Default true. */
  preloadSiblings?: boolean
  /** Refuse to render holes in the terrain. Default true. Relax for fast pans. */
  forbidHoles?: boolean
  /** Decode water-mask textures (coastline shading). Default true. */
  enableWaterMask?: boolean
  /** Cull tiles fully inside the fog volume. Default false. */
  enableFogCulling?: boolean
  /** When fog culling is on, override SSE for fogged tiles. Default true. */
  enforceCulledScreenSpaceError?: boolean
  /** SSE applied to fogged tiles when `enforceCulledScreenSpaceError` is true. Default 64. */
  culledScreenSpaceError?: number
  /** Cross-fade between LODs when refining (smoother pop-in). Default false. */
  enableLodTransitionPeriod?: boolean
  /** Length (s) of the LOD cross-fade. Default 1. */
  lodTransitionLength?: number
  /** Sqlite asset cache row budget. Default 4096. */
  sqliteCacheMaxRows?: number
  /** Worker thread count. 0 = auto (hardware_concurrency-1, clamped 2..8). */
  taskProcessorThreads?: number
  /**
   * Minimum altitude above the loaded terrain surface, in metres MSL.
   * Default 0 (no constraint). Set to e.g. 2 to prevent the camera from going underground.
   * The clamp is applied every frame in native, based on the altitude of the nearest
   * terrain mesh vertex to the camera nadir.
   */
  minAltitudeAboveTerrain?: number

  // ── Optional rate caps (flight-sim feel) ─────────────────────────────────
  // All defaults: 0 = uncapped. Override per-screen to enforce a maximum
  // angular / linear speed on the integrator's per-DoF velocity. Useful when
  // your sensor feed is noisy and you would rather drop the change than
  // visually whip the camera.
  /** Maximum yaw rate (degrees per second). 0 = uncapped. */
  maxYawRateDegSec?: number
  /** Maximum pitch rate (degrees per second). 0 = uncapped. */
  maxPitchRateDegSec?: number
  /** Maximum roll rate (degrees per second). 0 = uncapped. */
  maxRollRateDegSec?: number
  /** Maximum climb / descent rate (metres per second). 0 = uncapped. */
  maxClimbRateMps?: number
  /** Maximum horizontal ground speed (metres per second). 0 = uncapped. */
  maxGroundSpeedMps?: number

  onMetrics?: (metrics: CesiumMetrics) => void
}

/**
 * Per-DoF camera control. Every setter records the measurement with the
 * native steady_clock time of arrival and feeds it into an α-β tracker
 * (constant-velocity predictor) that owns both demand and actual state.
 *
 * All setters are synchronous and thread-safe. Calling them from a
 * Reanimated worklet via a Nitro `hybridRef` is the supported path for
 * high-frequency updates; the worklet thread takes the integrator's mutex
 * briefly and returns immediately.
 *
 * Getters return Promises and should be called from the JS thread (e.g. a
 * `useEffect` or a throttled HUD update), not from a worklet.
 */
export interface CesiumViewMethods extends HybridViewMethods {
  /**
   * New geographic position demand (latitude / longitude in degrees). Use
   * for GPS or scripted position updates. Latitude is clamped to ±90 by the
   * integrator; longitude is continuous (no wrap normalisation) so the
   * shortest-arc residual maths stays correct across the antimeridian.
   */
  setPosition(latitude: number, longitude: number): void

  /** New altitude demand (metres above mean sea level). */
  setAltitude(altitudeMeters: number): void

  /** New heading demand (degrees, 0 = north, increasing clockwise). */
  setHeading(headingDeg: number): void

  /**
   * New attitude demand (pitch and roll in degrees). Bundled because almost
   * every IMU emits them together — but you can pass the previous value for
   * either axis if you only have one of them.
   */
  setAttitude(pitchDeg: number, rollDeg: number): void

  /**
   * New camera-space rotation applied after HPR (unit quaternion). Use for
   * boresight calibration or screen-fixed HUD alignment. SLERPed toward the
   * latest demand each frame.
   */
  setViewCorrection(q: Quaternion): void

  /** New vertical field-of-view demand (degrees). Clamped to 20..100. */
  setVerticalFov(deg: number): void

  /**
   * Atomically reset every DoF to the given camera state (value, velocity
   * and demand all snap to the target). Bypasses the integrator entirely;
   * use for `Fly to coordinate` actions or initial seeding from saved
   * state.
   */
  teleport(camera: CameraState): void

  /**
   * Most recent camera that was rendered (post-integration). This is what
   * the user is currently looking at and is what a HUD overlay should
   * mirror. Differs from `getDemandCamera()` while a glide is in progress
   * or when the terrain-floor clamp has raised the altitude.
   */
  getActualCamera(): Promise<CameraState>

  /**
   * What the consumer last asked for (per-DoF demand). Useful for
   * diagnostics or for showing the requested camera alongside the rendered
   * one.
   */
  getDemandCamera(): Promise<CameraState>

  /** Current view-correction quaternion (smoothed toward the latest demand). */
  getViewCorrection(): Promise<Quaternion>
}

export type CesiumView = HybridView<CesiumViewProps, CesiumViewMethods>
