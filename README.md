# react-native-cesium

> [!WARNING]
> This library is under development and may not work correctly in all cases yet.
>
> Current consumer support is **iOS only**. Android support is planned soon, but should be treated as in progress rather than production-ready today.

Experimental Cesium rendering for React Native using [Nitro Modules](https://github.com/mrousavy/nitro), with the native renderer powered by [Cesium Native](https://github.com/CesiumGS/cesium-native) and Metal on iOS.

![react-native-cesium example screenshot](./example/react-native-cesium-example.jpg)

## Status

- Consumer platform support today: **iOS**
- Android status: **planned / under development**
- Native engine: **Cesium Native**
- React Native integration: **Nitro host component + imperative hybrid ref API**

## Requirements

- React Native **new architecture**
- Node **18+**
- Ensure you follow the instructions below as there are dependcies and actions on your part
- A valid Cesium Ion access token and asset IDs for your content (get these from https://ion.cesium.com)
- **Disk and time:** building Cesium Native pulls **vcpkg** dependencies and can use **several GB** of disk; first iOS build often takes **a long time**

## Installation

Add the package to your app:

```bash
yarn add react-native-cesium
```

or:

```bash
npm install react-native-cesium
```

`react-native-nitro-modules` is installed automatically as a dependency of this package. You only need to add it yourself if you want to pin a specific compatible version.

### Building Cesium Native on your machine (not shipped in npm)

The npm package **does not ship** the required iOS or Android Cesium Native libraries. 
They are produced by compiling **Cesium Native** on your local machine with the right toolchains.

**Automatic (default):** when native output is missing, the package tries to build it for you:

- **iOS:** add **`pre_install`** in your app **`Podfile`** (one-time; see [Required Podfile hooks](#required-podfile-hooks-ios)) so **`pod install`** runs **`scripts/cesium/ensure-native.mjs --ios`**, which runs **`npm run update`** / **`yarn run update`** (if needed) and then **`CESIUM_BUILD_ONLY=ios`** build. The first run can take **a long time** and needs the same **system dependencies** as a manual build (CMake, Ninja, Xcode, disk space, etc.).
- **Android:** the library’s Gradle **`preBuild`** runs **`ensure-native.mjs --android`** if **`vendor/android`** is incomplete, which runs **`CESIUM_BUILD_ONLY=android`** build. Requires **ANDROID_NDK_HOME** (or an NDK the build script can find).

**CI / headless machines:** set **`REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1`** so missing artifacts fail fast with a clear error instead of starting a multi-hour compile. Build the **`vendor/`** trees on a Mac (iOS) or a machine with the NDK (Android), commit them if your workflow allows, or cache them in CI.

**Manual (optional):** from the package directory (e.g. `node_modules/react-native-cesium`):

```bash
yarn run update # this fecthes Cesium Native source to your local machine from Github
yarn run build # this actually does the build for both iOS and Android and can take a long time!
```

- `npm run update` / `yarn run update` checks out **Cesium Native `v0.59.0`** into `vendor/cesium-native` (created next to the package files; typically gitignored in app repos).
- `npm run build` / `yarn run build` runs `scripts/cesium/build.mjs` (**CMake**, **vcpkg** under `vendor/vcpkg` unless **`VCPKG_ROOT`** is set, **Ninja** on macOS) and writes **`vendor/ios/CesiumNative.xcframework`** and/or **`vendor/android`** depending on **`CESIUM_BUILD_ONLY`**.

You can also run **`npm run ensure-native`** / **`yarn ensure-native`** with **`--ios`**, **`--android`**, or both flags to trigger the same checks outside CocoaPods/Gradle.

Then install pods (iOS):

```bash
cd ios && pod install && cd ..
```

### System dependencies (Homebrew and Xcode)

These are **not** installed by `yarn add` / `pod install`. You need them on the **host** so `yarn run build` can succeed:

| Tool | Typical install (macOS) | Why |
| --- | --- | --- |
| **Xcode** or CLT | `xcode-select --install` or install Xcode from the App Store | Apple **clang**, **SDKs**, **xcodebuild** (XCFramework step) |
| **CMake** | `brew install cmake` | Configures and drives the native build |
| **Ninja** | `brew install ninja` | Required generator on macOS for the bundled build script |
| **Git** | Xcode includes `/usr/bin/git` | Clones Cesium Native and vcpkg |
| **Python 3** | Usually present on macOS; `brew install python@3` if needed | Some vcpkg/port steps expect a working `python3` |

Optional but recommended for faster or more reliable builds (see [Cesium Native developer setup](https://github.com/CesiumGS/cesium-native/blob/main/doc/topics/developer-setup.md)):

- **`nasm`** — `brew install nasm` (speeds some JPEG-related builds in dependency trees)

Ensure Homebrew’s binary directory is on your **`PATH`** when you run the build (Apple Silicon: `/opt/homebrew/bin`; Intel: `/usr/local/bin`). The build script tries to prepend those paths for nested CMake/vcpkg.

The Android output under **`vendor/android`** is built with the **Android NDK**; set **`ANDROID_NDK_HOME`** or install the NDK via Android Studio so the script can find it when you run a full `yarn run build` without `CESIUM_BUILD_ONLY=ios`.

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

## Usage

The public JS surface is exported from `react-native-cesium`:

- `CesiumView`
- `CameraState`
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

| Prop | Type | Description |
| --- | --- | --- |
| `ionAccessToken` | `string` | Cesium Ion access token used to authenticate requests. |
| `ionAssetId` | `number` | Cesium Ion asset ID for the main 3D tileset or terrain source. |
| `initialCamera` | `CameraState` | Construction-time camera seed. Use `setCamera()` for runtime updates. |
| `pauseRendering` | `boolean` | Temporarily pauses native rendering work. |
| `maximumScreenSpaceError` | `number` | Tileset quality / refinement tuning. |
| `maximumSimultaneousTileLoads` | `number` | Limits in-flight tile loading. |
| `loadingDescendantLimit` | `number` | Limits descendant loading fan-out. |
| `msaaSampleCount` | `number` | Anti-aliasing sample count. `1` disables MSAA. |
| `ionImageryAssetId` | `number` | Optional imagery layer asset ID to drape over the scene. |
| `onMetrics` | `(metrics: CesiumMetrics) => void` | Receives throttled runtime metrics and plain-text credits. |

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

### `CesiumViewMethods`

Use the imperative ref API exposed through `hybridRef`:

- `setCamera(camera: CameraState): void`
- `getCameraState(): Promise<CameraState>`

### `CesiumMetrics`

`onMetrics` currently reports:

- `fps`
- `tilesRendered`
- `tilesLoading`
- `tilesVisited`
- `ionTokenConfigured`
- `tilesetReady`
- `creditsPlainText`

## Example App

The example app in [`example/App.tsx`](./example/App.tsx) is the best current integration reference. It shows:

- creating and storing a Nitro `hybridRef`
- driving camera updates with `setCamera(...)`
- listening to `onMetrics`
- switching imagery layers
- presenting Cesium attribution from `creditsPlainText`

Before running the example, copy `example/.env_example` to `example/.env` and set `CESIUM_ION_ACCESS_TOKEN` to your own token. Restart Metro after editing `.env` so the env transform is reapplied.

The example project currently links this library locally via `link:..`.

## Library development (this repository)

If you are hacking on **react-native-cesium** itself (not only consuming it from an app), use the same native pipeline from the **repo root**: install devDependencies, then:

```bash
yarn run update
yarn run build
```

`yarn run update` checks out **Cesium Native `v0.59.0`** (pinned for stability). `yarn run build` runs `scripts/cesium/build.mjs` and writes **`vendor/ios`** and **`vendor/android`** next to the package. `yarn run build:js` builds the published JS (`lib/`) and runs on `prepublishOnly`.

Details and troubleshooting match **Cesium Native on your machine** and **System dependencies** above. In a git checkout, `vendor/` trees are gitignored; consumers and contributors alike generate them with `update` + `build` (or `npm run ensure-native`).

### Troubleshooting `yarn build` (iOS / vcpkg)

**EZVCPKG / openssl / `fatal error: 'stdlib.h' file not found'` (or `assert.h`, `sys/types.h`)**

That log line usually means **Cesium Native was configured with embedded EZVCPKG** (`cmake/ezvcpkg`, builds under `~/.ezvcpkg`) instead of this package’s flow. The host **openssl** build for **`arm64-osx`** must see the **macOS SDK**; otherwise clang has no system headers.

- **Use this library’s build:** from `node_modules/react-native-cesium`, run **`yarn run update`** then **`yarn run build`**. `scripts/cesium/build.mjs` passes **`-DCESIUM_USE_EZVCPKG=OFF`** and uses **`vendor/vcpkg`** with the same environment fixes as below—**do not** run raw CMake inside `vendor/cesium-native` unless you know how to mirror those flags.
- **If you configure Cesium Native yourself:** export the SDK before CMake:

  ```bash
  export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  ```

  The build script sets **`SDKROOT`** on macOS when it is unset. Ensure **Xcode** or **Command Line Tools** are installed (`xcode-select --install`).

If CMake reports **`CMAKE_MAKE_PROGRAM` is not set** (for **Ninja**), or **`CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` not set** after vcpkg fails, nested CMake often cannot see Homebrew or Xcode tools (common with a minimal `PATH` from **GUI / Yarn**). The build script **prepends** `/opt/homebrew/bin` and `/usr/local/bin` on macOS, resolves **`ninja` to an absolute path** in `CMAKE_MAKE_PROGRAM`, and passes **`-DCMAKE_C_COMPILER` / `-DCMAKE_CXX_COMPILER`** from `xcrun`. If it still fails, open a normal terminal, run `which ninja` and `xcrun --find clang`, then run `yarn build` again.

If the iOS simulator step fails while building **curl** with an error like `call to undeclared function 'pipe2'` in `socketpair.c`, that comes from vcpkg’s libcurl configure enabling `HAVE_PIPE2` even though iOS does not implement `pipe2()`. The build script prepends **`scripts/cesium/vcpkg-triplets`** to **`VCPKG_OVERLAY_TRIPLETS`** so **`arm64-ios`** / **`arm64-ios-simulator`** triplets set **`VCPKG_CMAKE_CONFIGURE_OPTIONS`** (including **`-DHAVE_PIPE2=OFF`**). If you still see a cached failure, delete the partial build and retry:

```bash
rm -rf vendor/cesium-native/build-ios-simulator vendor/vcpkg/buildtrees/curl
CESIUM_BUILD_ONLY=ios yarn build
```

If **ktx** (or another port) fails CMake configure with **`Could NOT find Threads`** / **`CMAKE_HAVE_LIBC_PTHREAD` failed**, upstream **zstd** is built with **multithreading**, so **`zstdConfig.cmake` calls `find_dependency(Threads)`**, which **`FindThreads`** often cannot satisfy on **iOS**. This package adds a **vcpkg overlay** for **`zstd`** (`scripts/cesium/vcpkg-ports/zstd`) that sets **`ZSTD_MULTITHREAD_SUPPORT=0`** when **`VCPKG_TARGET_IS_IOS`**, so the exported CMake config no longer pulls **Threads** (slightly slower compression on device; acceptable for linking). The build script also sets **`-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`** on iOS triplets as a secondary mitigation.

After pulling updates, **clear cached zstd/ktx** and the iOS Cesium build dirs so vcpkg rebuilds **zstd** with the overlay:

```bash
rm -rf vendor/cesium-native/build-ios-device vendor/cesium-native/build-ios-simulator \
  vendor/vcpkg/buildtrees/zstd vendor/vcpkg/buildtrees/ktx
CESIUM_BUILD_ONLY=ios yarn build
```

If **vcpkg** still restores a cached **zstd** binary and skips the overlay, run once with **`VCPKG_BINARY_SOURCES=clear`** (or your cache’s equivalent) so **zstd** rebuilds from the overlay.

If **ktx** fails the compile step with **`invalid version number in '--target=…-macabi'`** (or **`MacOSX`**.sdk / **macabi** in the compiler line), the **arm64-ios** triplet must set **`CMAKE_OSX_SYSROOT` to the iOS SDK**. Upstream vcpkg’s iOS toolchain does not set a sysroot for **device** **arm64** (only for simulator), so CMake can wrongly use the **host macOS SDK**. The overlay triplet **`scripts/cesium/vcpkg-triplets/arm64-ios.cmake`** sets **`VCPKG_OSX_SYSROOT` to `iphoneos`**. Clear **ktx** and the iOS build dirs, then rebuild:

```bash
rm -rf vendor/cesium-native/build-ios-device vendor/vcpkg/buildtrees/ktx
CESIUM_BUILD_ONLY=ios yarn build
```

**Android / `Could NOT find modp_b64`**

Cesium Native sets `PACKAGE_BUILD_DIR` from **`VCPKG_INSTALLED_DIR`**. That must be **`<build>/vcpkg_installed`** (manifest mode), not **`vendor/vcpkg/installed`**. A stale **CMakeCache** can leave the wrong path so **`find_package(modp_b64)`** runs before headers/libs are visible.

The build script passes **`-DVCPKG_INSTALLED_DIR=…/vcpkg_installed`** and clears Android build dirs when the cached value mismatches. If you still hit this, remove the Android build tree and rebuild:

```bash
rm -rf vendor/cesium-native/build-android-arm64 vendor/android
CESIUM_BUILD_ONLY=android yarn build
```

If install fails with **`/usr/local/include`** (or **`file INSTALL cannot make directory "/usr/local/include": File exists`**), the Android build tree was configured with CMake’s default **`CMAKE_INSTALL_PREFIX=/usr/local`**. Cesium Native bakes that path into install rules; **`cmake --install --prefix …` does not fix it.** The build script now drops **`build-android-arm64`** when the cached prefix does not match **`vendor/android`**. If needed, remove it manually and rebuild:

```bash
rm -rf vendor/cesium-native/build-android-arm64
CESIUM_BUILD_ONLY=android yarn build
```

## Credits

- Native rendering is built on [Cesium Native](https://github.com/CesiumGS/cesium-native)
- Cesium Native is licensed under Apache 2.0; see [`NOTICE`](./NOTICE)
- React Native integration is built with [Nitro Modules](https://github.com/mrousavy/nitro)
