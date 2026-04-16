# react-native-cesium

> [!WARNING]
> This library is under development and may not work correctly in all cases yet.
>

Experimental Cesium rendering for React Native using [Nitro Modules](https://github.com/mrousavy/nitro), with the native Metal/Vulkan renderer powered by [Cesium Native](https://github.com/CesiumGS/cesium-native).

![react-native-cesium example screenshot](./example/react-native-cesium-example.jpg)

## Status

- Platform maturity: **under development**
- Supported renderers: Metal (iOS) and Vulkan (Android)
- Native engine: **Cesium Native**
- React Native integration: **Nitro host component + imperative hybrid ref API**

## Requirements

- React Native **new architecture**
- React Native Nitro Modules is a dependency
- Follow the native build instructions below: additional setup is required by you.
- A valid Cesium Ion access token and asset IDs for your content (get these from https://ion.cesium.com)
- **Disk and time:** building Cesium Native pulls **vcpkg** dependencies, can use **several GB** of disk, and often takes a long time on first build.

## Installation

First install the Nitro Modules runtime dependency:

```bash
yarn add react-native-nitro-modules
```

or:

```bash
npm install react-native-nitro-modules
```

Then add `react-native-cesium`:

```bash
yarn add react-native-cesium
```

or:

```bash
npm install react-native-cesium
```


## Building Cesium Native locally

The package **does not include** the required iOS or Android Cesium Native libraries. Generate them locally by compiling **Cesium Native** with the required toolchains.

### System dependencies

These are **not** installed by `yarn add` / `pod install`. Install them on your local machine first.

#### Shared (both iOS and Android)

| Tool | Typical install (macOS) | Why |
| --- | --- | --- |
| **CMake** | `brew install cmake` | Configures and drives the native build |
| **Ninja** | `brew install ninja` | Required generator on macOS for the bundled build script |
| **Git** | Xcode includes `/usr/bin/git` | Clones Cesium Native and vcpkg |
| **Python 3** | Usually present on macOS; `brew install python@3` if needed | Some vcpkg/port steps expect a working `python3` |

Optional but recommended for faster or more reliable builds (see [Cesium Native developer setup](https://github.com/CesiumGS/cesium-native/blob/main/doc/topics/developer-setup.md)):

- **`nasm`** — `brew install nasm` (speeds some JPEG-related builds in dependency trees)

Ensure Homebrew’s binary directory is on your **`PATH`** when you run the build (typically `/opt/homebrew/bin` on Apple Silicon or `/usr/local/bin` on Intel Macs). The build script also tries to prepend common Homebrew paths for nested CMake/vcpkg processes.

<details>
<summary><strong>Additional iOS specific requirements</strong></summary>

- **Xcode** or Command Line Tools: `xcode-select --install` (or install Xcode from the App Store)
- Needed for Apple `clang`, SDKs, and `xcodebuild` (XCFramework assembly)

</details>

<details>
<summary><strong>Additional Android specific requirements</strong></summary>

- Install **Android Studio**
- In SDK Manager, install:
  - **Android SDK**
  - **NDK** (the project currently uses `27.1.12297006`)
- Ensure **Java 17 (JDK)** is available (Android Studio bundled JDK is fine)

For Android builds, make sure one of these is set so the NDK can be discovered:

- `ANDROID_NDK_HOME` (explicit NDK path), or
- `ANDROID_SDK_ROOT` / `ANDROID_HOME` with an installed NDK under `ndk/`

If `ANDROID_NDK_HOME` is unset, the build script will try to auto-detect the latest installed NDK from your SDK directory.

</details>

### Required Podfile hooks (iOS)

Your app **`ios/Podfile`** must **`require`** two helpers from this package and call them in **`pre_install`** and **`post_install`**.

**Top of the Podfile** (paths assume the default `node_modules` layout):

```ruby
require_relative "../node_modules/react-native-cesium/ios/react_native_cesium_ensure_native"
require_relative "../node_modules/react-native-cesium/ios/react_native_cesium_post_install"
```

**`pre_install`** (runs before pods resolve; triggers automatic Cesium Native clone/build when the XCFramework is missing):

```ruby
pre_install do |installer|
  react_native_cesium_ensure_native
end
```

**`post_install`** (header search paths and simulator arch exclusions):

```ruby
post_install do |installer|
  react_native_post_install(
    installer,
    config[:reactNativePath],
    :mac_catalyst_enabled => false
  )
  react_native_cesium_post_install(installer)
end
```

The **post-install** helper is currently needed to:

- prepend this package's `cpp/` headers before the locally built XCFramework headers
- exclude `x86_64` iOS simulator builds, because the simulator slice is Apple Silicon `arm64` only

### Android (Gradle automatic ensure-native)

Unlike iOS, you **do not** need to copy anything into your **application’s** `android/build.gradle` (or `settings.gradle`) so Cesium Native can be cloned and built automatically. React Native already includes this package as an Android library dependency; this repo’s **`android/build.gradle`** registers **`ensureCesiumNativeAndroid`** and makes **`preBuild`** depend on it.

What happens on **`./gradlew`** / Android Studio build:

1. **`preBuild`** runs **`ensureCesiumNativeAndroid`** before CMake compiles the JNI/native code.
2. That task checks for a marker file under the installed package:  
   `vendor/android/share/cesium-native/cmake/cesium-nativeConfig.cmake`.
3. If the marker **exists**, the task does nothing.
4. If the marker is **missing**, it runs (from the package root, e.g. `node_modules/react-native-cesium`):  
   `node scripts/cesium/ensure-native.mjs --android`  
   which may run **`update`** then a **`CESIUM_BUILD_ONLY=android`** **`build`**, same as a manual run (can take a long time).
5. If **`REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1`** is set and artifacts are still missing, the build **fails fast** with an error instead of running the script.

You still need a discoverable **Android NDK** (expand **Additional Android specific requirements** under [System dependencies](#system-dependencies) above) so that build can succeed.

### Building

**Automatic (default):** assuming you have done the above and have all the required dependencies in place, when the native output is missing, the package tries to build it for you:

- **iOS:** add **`pre_install`** in your app **`Podfile`** (one-time; see [Required Podfile hooks](#required-podfile-hooks-ios)) so **`pod install`** runs **`scripts/cesium/ensure-native.mjs --ios`**, which runs **`npm run update`** / **`yarn run update`** (if needed) and then **`CESIUM_BUILD_ONLY=ios`** build. The first run can take **a long time** and needs the same **system dependencies** as a manual build (CMake, Ninja, Xcode, disk space, etc.).
- **Android:** no extra hooks in your app; see [Android (Gradle automatic ensure-native)](#android-gradle-automatic-ensure-native). Needs a discoverable **NDK**.

**Manual (optional):** run the **`update`** and **`build`** scripts defined in **`react-native-cesium`’s `package.json`**. They are **not** available as top-level commands in your app; you must run them **in the context of the installed package**.

From your **application project root** (where your app’s `package.json` lives), after `yarn add` / `npm install`:

**Yarn:**

```bash
yarn --cwd node_modules/react-native-cesium run update
yarn --cwd node_modules/react-native-cesium run build
```

**npm:**

```bash
npm run update --prefix node_modules/react-native-cesium
npm run build --prefix node_modules/react-native-cesium
```

**Alternative:** `cd node_modules/react-native-cesium` and run `yarn run update` / `yarn run build` (or the equivalent `npm run …`).

**Yarn/npm workspaces:** if `react-native-cesium` is a workspace package in your monorepo, use your tool’s workspace runner, e.g. `yarn workspace react-native-cesium run update` (exact syntax depends on your workspace layout).

- `update` checks out **Cesium Native `v0.59.0`** into `vendor/cesium-native` under the package (next to its other files; typically ignored in app repos).
- `build` runs `scripts/cesium/build.mjs` (**CMake**, **vcpkg** under `vendor/vcpkg` unless **`VCPKG_ROOT`** is set, **Ninja** on macOS) and writes **`vendor/ios/CesiumNative.xcframework`** and/or **`vendor/android`** depending on **`CESIUM_BUILD_ONLY`**.

Then install pods (iOS):

```bash
cd ios && pod install && cd ..
```

## Usage

The public JS surface is exported from `react-native-cesium`:

- `CesiumView`
- `CameraState`
- `Quaternion`
- `CesiumMetrics`
- `CesiumViewProps`
- `CesiumViewMethods`

Minimal usage:

```tsx
import React, { useRef } from 'react'
import { StyleSheet, View } from 'react-native'
import { callback } from 'react-native-nitro-modules'
import {
  CesiumView,
  type CameraState,
  type CesiumMetrics,
  type CesiumViewMethods,
} from 'react-native-cesium'

const initialCamera: CameraState = {
  latitude: 46.02,
  longitude: 7.6,
  altitude: 5800,
  heading: 220,
  pitch: -20,
  roll: 0,
  verticalFovDeg: 60,
}

export function GlobeScreen() {
  const cesiumRef = useRef<CesiumViewMethods | null>(null)

  return (
    <View style={styles.container}>
      <CesiumView
        hybridRef={callback((ref: CesiumViewMethods | null) => {
          cesiumRef.current = ref
        })}
        style={styles.fill}
        ionAccessToken="YOUR_CESIUM_ION_ACCESS_TOKEN"
        ionAssetId={1}
        initialCamera={initialCamera}
        pauseRendering={false}
        maximumScreenSpaceError={16}
        maximumSimultaneousTileLoads={8}
        loadingDescendantLimit={20}
        msaaSampleCount={1}
        ionImageryAssetId={2}
        onMetrics={callback((metrics: CesiumMetrics) => {
          console.log('Cesium FPS:', metrics.fps)
        })}
      />
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  fill: { flex: 1 },
})
```

### Nitro `hybridRef` requirement

When you pass a callback ref to `CesiumView`, wrap it with `callback(...)` from `react-native-nitro-modules`. Plain callback functions do not make it across the React Native bridge correctly for this component.

## API

### `CesiumViewProps`

`CesiumView` is a Nitro host component. In addition to standard view props like `style`, it currently expects these Cesium-specific props:

**Consumer-required props (TypeScript)**

| Prop | Type | Default | Description                                                                                                                                                                                                  |
| --- | --- | --- |--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ionAccessToken` | `string` | _none_ | Cesium Ion token for authenticated asset/imagery requests. An invalid token usually leaves the globe empty or partially loaded due to 401/403 responses.                                                     |
| `ionAssetId` | `number` | _none_ | Main Ion asset to render (tileset/terrain).                                                                                                                                                                  |
| `initialCamera` | `CameraState` | _none_ | Initial camera used when the view is created. This is only used for initial camera; use `setCamera()` for runtime moves.                                                                                     |
| `pauseRendering` | `boolean` | `false` | Pauses the native render loop. Set `true` to stop rendering and reduce GPU/CPU usage.                                                                                                                        |
| `maximumScreenSpaceError` | `number` | `32` | Quality/performance trade-off for tile refinement. Lower values are sharper (more work); higher values are faster/blurrier.                                                                                  |
| `maximumSimultaneousTileLoads` | `number` | `12` | Max concurrent tile fetch/decode operations. Raising `8 -> 16` can improve fast camera moves on good networks but may increase memory/bandwidth spikes.                                                      |
| `loadingDescendantLimit` | `number` | `20` | Caps descendant tile fan-out during traversal. Lower values like `10` smooth bursts on low-end devices; higher values like `40` can fill detail faster.                                                      |
| `msaaSampleCount` | `number` | `1` | Anti-aliasing sample count. iOS uses `1`, `2`, or `4` (`>=4 -> 4`, `>=2 -> 2`, otherwise `1`); Android currently renders at `1` (MSAA setting is currently ignored in Vulkan backend). |
| `ionImageryAssetId` | `number` | `1` | Imagery layer to drape over terrain/tiles. Use a satellite imagery asset for a photoreal look, or switch to a streets/map layer for legibility. See your Cesium Ion Asset IDs. |

**Consumer-optional props (TypeScript)**

| Prop | Type | Default | Description |
| --- | --- | --- | --- |
| `onMetrics` | `(metrics: CesiumMetrics) => void` | `undefined` | Receives throttled runtime stats and credits text. |

### `CameraState`

```ts
type CameraState = {
  latitude: number
  longitude: number
  altitude: number
  heading: number
  pitch: number
  roll: number
  verticalFovDeg: number
}
```

| Field | Type | Valid range / format | Description                                                                                                                     |
| --- | --- | --- |---------------------------------------------------------------------------------------------------------------------------------|
| `latitude` | `number` | `-90..90` | Camera latitude in degrees.                                                                                                     |
| `longitude` | `number` | `-180..180` | Camera longitude in degrees.                                                                                                    |
| `altitude` | `number` | Meters above ellipsoid | Camera height in meters.                                                                                                        |
| `heading` | `number` | Degrees | Compass direction the camera faces. Example: `0` faces north, `90` faces east.                                                  |
| `pitch` | `number` | Degrees | Tilt angle. Negative values look downward toward terrain; positive values tilt up toward horizon/sky.                           |
| `roll` | `number` | Degrees | Bank/rotation around forward axis. Positive right bank, negative left bank.                                                     |
| `verticalFovDeg` | `number` | `20..100` (clamped) | Vertical field of view. Example: `30` zooms in/narrows view; `90` gives wider peripheral view with more perspective distortion. |

### `Quaternion`

Used for **camera-space view correction** with `setCameraQuaternion` (see below). Components are `w, x, y, z` (Hamilton convention). Non-unit values are normalized on the native side.

```ts
type Quaternion = {
  w: number
  x: number
  y: number
  z: number
}
```

### `CesiumViewMethods`

| Method | Signature | Description                                                                                                                                   |
| --- | --- |-----------------------------------------------------------------------------------------------------------------------------------------------|
| `setCamera` | `(camera: CameraState) => void` | Applies a new camera state at runtime (position, heading, pitch, roll, vertical FOV). Does **not** change the stored view-correction quaternion; use `setCameraQuaternion` when you need to update that. |
| `setCameraQuaternion` | `(camera: CameraState, viewCorrection: Quaternion) => void` | Same camera fields as `setCamera`, plus a rotation applied **in camera space after** heading/pitch/roll. Use this for screen-fixed adjustments (e.g. boresight calibration, aligning a synthetic horizon overlay) without expressing the offset as extra Euler angles. |
| `getCameraState` | `() => Promise<CameraState>` | Returns the current native camera snapshot (lat/lon/alt, HPR, VFOV). This is the underlying globe attitude; it does **not** encode the view correction into HPR. |
| `getViewCorrection` | `() => Promise<Quaternion>` | Returns the **smoothed** view-correction quaternion currently applied (identity `w=1,x=y=z=0` if you have never called `setCameraQuaternion`). |

#### `setCamera` vs `setCameraQuaternion`

- Use **`setCamera`** alone when you only need the classic globe camera.
- Use **`setCameraQuaternion`** when you need both the usual `CameraState` **and** a small rotation relative to the uncorrected camera (HUD alignment, horizon line offset in screen space, lens/display calibration, etc.).
- You may **mix** the two: calling `setCamera` updates position and HPR but leaves the last view-correction target unchanged, so a calibration quaternion set earlier continues to apply until you change it with `setCameraQuaternion`.
- Prefer **`setCameraQuaternion`** on frames where you need to drive both the globe camera and the correction together so the native target stays consistent.

### `CesiumMetrics`

| Field | Type | Description                                                                                                                                 |
| --- | --- |---------------------------------------------------------------------------------------------------------------------------------------------|
| `fps` | `number` | Smoothed frames-per-second estimate from the native render loop.                                                                            |
| `tilesRendered` | `number` | Number of tiles currently rendered in the frame.                                                                                            |
| `tilesLoading` | `number` | Number of tiles still loading. If this stays high for long periods, reduce load pressure (`maximumSimultaneousTileLoads`) or check network. |
| `tilesVisited` | `number` | Number of tiles visited during traversal/culling.                                                                                           |
| `ionTokenConfigured` | `boolean` | Whether a non-empty Ion token is configured natively. `false` is a quick signal to check `ionAccessToken`.                                  |
| `tilesetReady` | `boolean` | Whether the primary tileset is initialized and ready.                                                                                       |
| `creditsPlainText` | `string` | Plain-text attribution/credits from Cesium data sources. Display this in your app footer to satisfy attribution requirements.               |

## Example App

The example app in [`example/App.tsx`](./example/App.tsx) is the best current integration reference. It shows:

- creating and storing a Nitro `hybridRef`
- driving camera updates with `setCamera(...)`
- listening to `onMetrics`
- switching imagery layers
- presenting Cesium attribution from `creditsPlainText`

Before running the example, copy `example/.env_example` to `example/.env` and set `CESIUM_ION_ACCESS_TOKEN` to your own token. Restart Metro after editing `.env` so the env transform is reapplied.

The example project currently links this library locally via `link:..`.

## Credits

- Native rendering is built on [Cesium Native](https://github.com/CesiumGS/cesium-native)
- Cesium Native is licensed under Apache 2.0; see [`NOTICE`](./NOTICE)
- React Native integration is built with [Nitro Modules](https://github.com/mrousavy/nitro)


