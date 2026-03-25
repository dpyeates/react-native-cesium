import { NitroModules } from 'react-native-nitro-modules'
import type { ReactNativeCesium as ReactNativeCesiumSpec } from './specs/react-native-cesium.nitro'

export const ReactNativeCesium =
  NitroModules.createHybridObject<ReactNativeCesiumSpec>('ReactNativeCesium')