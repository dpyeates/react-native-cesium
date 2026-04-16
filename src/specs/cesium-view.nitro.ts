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
