#!/usr/bin/env bash
# Compile and run the host engine tests. GeoidConverterTest needs only EGM96.
# CameraIntegratorTest needs the iOS simulator-slice headers from the vendored
# Cesium Native XCFramework. Lightweight smoke tests — not part of the
# production build.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HEADERS="$ROOT/vendor/ios/CesiumNative.xcframework/ios-arm64-simulator/Headers"
OUT="$(mktemp -d)"
echo "Building into $OUT..."

xcrun -sdk macosx clang -std=c11 -O0 -g \
  -I"$ROOT/cpp" \
  -c "$ROOT/cpp/third_party/egm96/EGM96.c" \
  -o "$OUT/EGM96.o"

xcrun -sdk macosx clang++ -std=c++20 -O0 -g \
  -I"$ROOT/cpp" \
  -o "$OUT/geoid" \
  "$ROOT/cpp/engine/test/GeoidConverterTest.cpp" \
  "$ROOT/cpp/engine/GeoidConverter.cpp" \
  "$OUT/EGM96.o"

"$OUT/geoid"

if [[ ! -d "$HEADERS" ]]; then
  echo "Cesium Native headers not found at $HEADERS"
  echo "Run 'npm run update && CESIUM_BUILD_ONLY=ios npm run build' first."
  exit 1
fi

xcrun -sdk macosx clang++ -std=c++20 -O0 -g \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE=1 \
  -I"$ROOT/cpp" -I"$HEADERS" \
  -o "$OUT/cit" \
  "$ROOT/cpp/engine/CameraIntegrator.cpp" \
  "$ROOT/cpp/engine/test/CameraIntegratorTest.cpp"

"$OUT/cit"
