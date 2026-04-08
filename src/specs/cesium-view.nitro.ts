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
  /** Runtime camera control path after the view has been created. */
  setCamera(camera: CameraState): void
}

export type CesiumView = HybridView<CesiumViewProps, CesiumViewMethods>
