import { getHostComponent } from 'react-native-nitro-modules'
import type {
  CesiumViewProps,
  CesiumViewMethods,
} from './specs/cesium-view.nitro'
import CesiumViewConfig from '../nitrogen/generated/shared/json/CesiumViewConfig.json'

/** For `hybridRef`, use `callback(...)` from `react-native-nitro-modules` or a ref object —
 *  unwrapped functions are turned into booleans by RN and never reach native. */
export const CesiumView = getHostComponent<CesiumViewProps, CesiumViewMethods>(
  'CesiumView',
  () => CesiumViewConfig
)
