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
  /** Plain-text credits summary (HTML stripped) for optional JS UI. */
  creditsPlainText: string
}

export interface CesiumViewProps extends HybridViewProps {
  ionAccessToken: string
  ionAssetId: number
  cameraLatitude: number
  cameraLongitude: number
  cameraAltitude: number
  cameraHeading: number
  cameraPitch: number
  cameraRoll: number
  /** Vertical field of view in degrees (clamped natively, typically 20–100). */
  cameraVerticalFovDeg: number
  debugOverlay: boolean
  /** When true, CADisplayLink does not run (saves battery when off-screen). */
  pauseRendering: boolean
  gesturePanEnabled: boolean
  gesturePinchZoomEnabled: boolean
  gesturePinchRotateEnabled: boolean
  /** Multiplier on pan pixel deltas (default 1). */
  gesturePanSensitivity: number
  /** Multiplier on pinch zoom ratio per frame (default 1). */
  gesturePinchSensitivity: number
  /** Cesium tileset `maximumScreenSpaceError` (higher = coarser / faster). */
  maximumScreenSpaceError: number
  maximumSimultaneousTileLoads: number
  loadingDescendantLimit: number
  /** 1 = off; 2 or 4 = MSAA when supported. */
  msaaSampleCount: number
  /** Show ion/imagery attribution in a native footer label. */
  showCreditsFooter: boolean
  /** Asset ID of the raster overlay. 1 = terrain-only (hypsometric). */
  ionImageryAssetId: number
  /** Optional; called ~2×/s with fps and tile stats. */
  onMetrics?: (metrics: CesiumMetrics) => void
}

export interface CesiumViewMethods extends HybridViewMethods {
  setJoystickRates(pitchRate: number, rollRate: number): void
  onTouchStart(pointerId: number, x: number, y: number): void
  onTouchChange(pointerId: number, x: number, y: number): void
  onTouchEnd(pointerId: number): void
  getCameraState(): Promise<CameraState>
  flyTo(
    latitude: number,
    longitude: number,
    altitude: number,
    heading: number,
    pitch: number,
    roll: number,
    durationSeconds: number
  ): void
  lookAt(
    targetLatitude: number,
    targetLongitude: number,
    targetAltitude: number,
    durationSeconds: number
  ): void
}

export type CesiumView = HybridView<CesiumViewProps, CesiumViewMethods>
