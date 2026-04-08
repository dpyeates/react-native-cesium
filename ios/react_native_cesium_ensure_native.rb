# Call from your app Podfile inside `pre_install` (see package README). Runs
# scripts/cesium/ensure-native.mjs --ios so Cesium Native is cloned/built when
# vendor/ios/CesiumNative.xcframework is missing.
def react_native_cesium_ensure_native
  package_root = File.expand_path("..", __dir__)
  script = File.join(package_root, "scripts", "cesium", "ensure-native.mjs")
  unless File.exist?(script)
    raise "react-native-cesium: expected #{script}"
  end
  ok = system("node", script, "--ios")
  raise "react-native-cesium: ensure-native failed (see package README)" unless ok
end
