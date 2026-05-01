import type {
  HybridView,
  HybridViewProps,
  HybridViewMethods,
} from 'react-native-nitro-modules'

/** Snapshot of the native camera for persistence / sync with React state. */
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
 * Unit quaternion (w + xi + yj + zk). Used for camera-space view correction.
 * Non-unit values are normalized on the native side.
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
  /** Construction-time seed camera. Use `setCamera()` for runtime updates. */
  initialCamera: CameraState
  pauseRendering: boolean
  maximumScreenSpaceError: number
  maximumSimultaneousTileLoads: number
  loadingDescendantLimit: number
  /** 1 = off; 2 or 4 = MSAA when supported. */
  msaaSampleCount: number
  ionImageryAssetId: number

  // ── Optional perf / quality knobs (added in 1.1) ─────────────────────
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

  onMetrics?: (metrics: CesiumMetrics) => void
}

export interface CesiumViewMethods extends HybridViewMethods {
  /** Returns the current native camera state. */
  getCameraState(): Promise<CameraState>
  /**
   * Runtime camera control (heading/pitch/roll + position + VFOV).
   * Does not change the view-correction quaternion; use `setCameraQuaternion` to set that.
   */
  setCamera(camera: CameraState): void
  /**
   * Same fields as `setCamera`, plus a camera-space rotation applied after HPR
   * (e.g. boresight / HUD alignment). See README.
   */
  setCameraQuaternion(camera: CameraState, viewCorrection: Quaternion): void
  /** Current view-correction quaternion (identity if never set via `setCameraQuaternion`). */
  getViewCorrection(): Promise<Quaternion>
}

export type CesiumView = HybridView<CesiumViewProps, CesiumViewMethods>
