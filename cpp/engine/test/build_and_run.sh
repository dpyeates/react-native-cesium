#!/usr/bin/env bash
# Compile and run CameraIntegratorTest against the iOS simulator-slice headers
# from the vendored Cesium Native XCFramework. Lightweight smoke test —
# not part of the production build.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HEADERS="$ROOT/vendor/ios/CesiumNative.xcframework/ios-arm64-simulator/Headers"

if [[ ! -d "$HEADERS" ]]; then
  echo "Cesium Native headers not found at $HEADERS"
  echo "Run 'npm run update && CESIUM_BUILD_ONLY=ios npm run build' first."
  exit 1
fi

OUT="$(mktemp -d)"
echo "Building into $OUT..."

xcrun -sdk macosx clang++ -std=c++20 -O0 -g \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE=1 \
  -I"$ROOT/cpp" -I"$HEADERS" \
  -o "$OUT/cit" \
  "$ROOT/cpp/engine/CameraIntegrator.cpp" \
  "$ROOT/cpp/engine/test/CameraIntegratorTest.cpp"

"$OUT/cit"
