# react-native-cesium

Nitro module for React Native with **Metal** on iOS and **Vulkan** on Android. Built on [Nitro Modules](https://github.com/mrousavy/nitro).

## Installation (consumer apps)

Add the package; **`react-native-nitro-modules`** is installed automatically as a dependency (you do not need to list it separately unless you want to pin a version explicitly).

**Yarn**

```bash
yarn add react-native-cesium
```

**npm**

```bash
npm install react-native-cesium
```

**iOS — run CocoaPods** (pulls in this pod, Nitro’s pods, and Metal-related frameworks; no manual Podfile edits required for normal setups):

```bash
cd ios && pod install && cd ..
```

**Android — no extra Gradle steps** for linking: sync or rebuild the app. The AAR links Vulkan via CMake as part of the library.

Then import and use the JS API (see your spec exports).

### Requirements

- React Native **0.76+**
- Node **18+**
- A compatible **`react-native-nitro-modules`** (pulled in with this package; see `dependencies` in `package.json` if you need to align versions)
- **Android:** `arm64-v8a` (Cesium Native prebuilts are built for that ABI only).
- **npm tarball:** includes `vendor/android` and `vendor/ios` prebuilts so apps do not compile Cesium Native at install time.

## Maintainers: Cesium Native prebuilts

The published package ships **pre-built** Cesium Native binaries under `vendor/android` and `vendor/ios` (see `NOTICE` for licensing). To refresh them from upstream:

1. Install prerequisites from the [Cesium Native developer setup](https://github.com/CesiumGS/cesium-native/blob/main/doc/topics/developer-setup.md): CMake 3.15+, **vcpkg** (clone, bootstrap, e.g. `git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"` and `$HOME/vcpkg/bootstrap-vcpkg.sh`), optional **nasm** for faster JPEG. **Ninja** is recommended (`brew install ninja`); if it is not on `PATH`, `yarn run build` falls back to Unix Makefiles on macOS/Linux.
2. **vcpkg:** If none of `VCPKG_ROOT`, **`.vcpkg-root`**, **`$HOME/vcpkg`**, or a valid **`vendor/vcpkg`** exists, the build **clones and bootstraps vcpkg into `vendor/vcpkg`** (gitignored). Requires **git** on `PATH`. Set **`CESIUM_SKIP_AUTO_VCPKG=1`** to disable that and fail with manual-install instructions instead.
3. **Android NDK:** If **`ANDROID_NDK_HOME`** is unset, the build picks the newest NDK under **`$ANDROID_HOME/ndk`**, **`$ANDROID_SDK_ROOT/ndk`**, or **`~/Library/Android/sdk/ndk`** (typical Android Studio layout). Set **`ANDROID_NDK_HOME`** explicitly to pin a version. For iOS-only on macOS: **`CESIUM_BUILD_ONLY=ios yarn build`** skips Android.
4. From the package root:

```bash
yarn run update    # clones/updates vendor/cesium-native (gitignored source)
yarn run build     # long-running: Release build + stage vendor/android and vendor/ios
```

`yarn run build` runs `scripts/cesium/build.mjs`. Set **`CESIUM_BUILD_ONLY=android`** or **`CESIUM_BUILD_ONLY=ios`** to build one platform. iOS requires macOS with Xcode.

The JavaScript library build (TypeScript + Bob) is **`yarn run build:js`** and is what runs on **`prepublishOnly`**.

**Git vs npm:** `vendor/android` and `vendor/ios` are **gitignored** (thousands of headers/libs; a full Android tree can be ~1GB+ including merged vcpkg **Release** and **debug** copies). They are still part of the **published npm package** when you run `yarn build` before `npm publish` (`package.json` `files` includes those paths). A fresh **git clone** does not contain prebuilts—run **`yarn update`** and **`yarn build`** in the repo root before native builds or publishing a release that ships binaries.

### Troubleshooting prebuilts

- **`vendor/android` must be ELF (Android), not Mach-O (Apple).** If Gradle fails with `archive member ... is neither ET_REL nor LLVM bitcode`, the static libs were built with the **host** compiler instead of the **Android NDK**. The build script sets **`VCPKG_CHAINLOAD_TOOLCHAIN_FILE`** to the NDK’s `build/cmake/android.toolchain.cmake` so vcpkg cross-compiles correctly. **Delete stale outputs and rebuild:** `rm -rf vendor/android vendor/cesium-native/build-android-arm64` then `CESIUM_BUILD_ONLY=android yarn run build`. The script also checks that `libCesiumUtility.a` does not contain Mach-O after staging.
- **`find_package(cesium-native)`** in Gradle: the library’s `android/CMakeLists.txt` appends `vendor/android` to **`CMAKE_FIND_ROOT_PATH`** so Android cross-compilation can see the config files (not only `CMAKE_PREFIX_PATH`).

## Native stacks (handled by the package)

- **iOS**: `Metal`, `MetalKit`, and `QuartzCore` are declared in the podspec; Swift imports Metal / MetalKit.
- **Android**: Native code links `libvulkan` and uses NDK Vulkan headers. Optional Vulkan `uses-feature` entries are in the library manifest for store metadata.

## Development (this repo)

```bash
npm install
npm run codegen
```

Edit `src/specs/*.nitro.ts`, then run `npm run codegen` again.

Publishing runs `prepublishOnly` (`npm run build:js`) to build `lib/` for npm consumers.

**Native development:** linking requires Cesium Native prebuilts under `vendor/android` and `vendor/ios/CesiumNative.xcframework`. Run `yarn run update` and `yarn run build` once (see [Maintainers: Cesium Native prebuilts](#maintainers-cesium-native-prebuilts)) before building the example app or publishing a release that includes native binaries.

### Example app (`example/`)

The `example/` project links this library locally (`"react-native-cesium": "file:.."`) and includes **`react-native-nitro-modules`** so CocoaPods can resolve the `NitroModules` pod (when using `file:` links, list Nitro explicitly like the example does). From the repo root:

```bash
cd example && npm install && cd ios && pod install && cd ..
```

Ensure **`vendor/ios/CesiumNative.xcframework`** and **`vendor/android`** exist (run `yarn run update` and `yarn run build` in the library root) before `pod install` or Gradle native builds.

- **Android:** `cd example/android && ./gradlew assembleDebug`
- **iOS:** `cd example/ios && xcodebuild -workspace CesiumExample.xcworkspace -scheme CesiumExample -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`

## Credits

Scaffolded with [create-nitro-module](https://github.com/patrickkabwe/create-nitro-module), aligned with [mrousavy/nitro](https://github.com/mrousavy/nitro).
