import type {
  HybridView,
  HybridViewProps,
  HybridViewMethods,
} from 'react-native-nitro-modules'

export interface CesiumViewProps extends HybridViewProps {
  ionAccessToken: string
  ionAssetId: number
  cameraLatitude: number
  cameraLongitude: number
  cameraAltitude: number
  cameraHeading: number
  cameraPitch: number
  cameraRoll: number
  debugOverlay: boolean
  /** Asset ID of the raster overlay. 1 = terrain-only (hypsometric). Any other
   *  valid Cesium ion asset ID adds that imagery drape on the terrain mesh. */
  ionImageryAssetId: number
}

export interface CesiumViewMethods extends HybridViewMethods {
  /** Set aircraft-style joystick rates (degrees/second).
   *  Called on each joystick move; native side applies rates each render frame.
   *  Pass (0, 0) on touch release to stop movement. */
  setJoystickRates(pitchRate: number, rollRate: number): void
  /** Forward raw touch events from the JS gesture overlay to the native
   *  GestureHandler so pan / pinch / rotate camera controls work. */
  onTouchStart(pointerId: number, x: number, y: number): void
  onTouchChange(pointerId: number, x: number, y: number): void
  onTouchEnd(pointerId: number): void
}

export type CesiumView = HybridView<CesiumViewProps, CesiumViewMethods>
