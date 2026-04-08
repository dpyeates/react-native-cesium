# Example App

This app is the reference integration for `react-native-cesium`. For the current public API, installation notes, and platform support status, start with the root [`README.md`](../README.md).

## What it demonstrates

- rendering `CesiumView` inside a React Native screen
- wiring a Nitro `hybridRef`
- driving the camera with `setCamera(...)`
- receiving runtime telemetry through `onMetrics`
- showing Cesium attribution text from `creditsPlainText`

## Before you run it

1. The example **`ios/Podfile`** includes **`react_native_cesium_ensure_native`** in **`pre_install`**, so the first **`pod install`** can clone and build Cesium Native automatically (long first run; see root README). You can still run **`yarn update`** and **`CESIUM_BUILD_ONLY=ios yarn build`** from the repo root first if you prefer. After changing `.env`, restart Metro so the Babel env transform re-reads the file.

2. Copy `example/.env_example` to `example/.env` and set `CESIUM_ION_ACCESS_TOKEN` to your own Cesium Ion access token.

```bash
cp .env_example .env
```

3. Install example dependencies and CocoaPods:

```bash
cd example
yarn install
cd ios && pod install && cd ..
```

## Run the example

```bash
yarn start
```

In a second terminal:

```bash
yarn ios
```

The example links the library locally using `link:..`, so changes in the package are reflected directly while developing.
