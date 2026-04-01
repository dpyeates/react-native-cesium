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
  cameraLatitude: number
  cameraLongitude: number
  cameraAltitude: number
  cameraHeading: number
  cameraPitch: number
  cameraRoll: number
  cameraVerticalFovDeg: number
  debugOverlay: boolean
  pauseRendering: boolean
  gesturePanEnabled: boolean
  gesturePinchZoomEnabled: boolean
  gesturePinchRotateEnabled: boolean
  gesturePanSensitivity: number
  gesturePinchSensitivity: number
  maximumScreenSpaceError: number
  maximumSimultaneousTileLoads: number
  loadingDescendantLimit: number
  /** 1 = off; 2 or 4 = MSAA when supported. */
  msaaSampleCount: number
  showCredits: boolean
  ionImageryAssetId: number
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
