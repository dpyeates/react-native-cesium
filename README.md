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

## Native stacks (handled by the package)

- **iOS**: `Metal`, `MetalKit`, and `QuartzCore` are declared in the podspec; Swift imports Metal / MetalKit.
- **Android**: Native code links `libvulkan` and uses NDK Vulkan headers. Optional Vulkan `uses-feature` entries are in the library manifest for store metadata.

## Development (this repo)

```bash
npm install
npm run codegen
```

Edit `src/specs/*.nitro.ts`, then run `npm run codegen` again.

Publishing runs `prepublishOnly` to build `lib/` for npm consumers.

### Example app (`example/`)

The `example/` project links this library locally (`"react-native-cesium": "file:.."`) and includes **`react-native-nitro-modules`** so CocoaPods can resolve the `NitroModules` pod (when using `file:` links, list Nitro explicitly like the example does). From the repo root:

```bash
cd example && npm install && cd ios && pod install && cd ..
```

- **Android:** `cd example/android && ./gradlew assembleDebug`
- **iOS:** `cd example/ios && xcodebuild -workspace CesiumExample.xcworkspace -scheme CesiumExample -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`

## Credits

Scaffolded with [create-nitro-module](https://github.com/patrickkabwe/create-nitro-module), aligned with [mrousavy/nitro](https://github.com/mrousavy/nitro).
