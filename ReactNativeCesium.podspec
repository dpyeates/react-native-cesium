require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "ReactNativeCesium"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported, :visionos => 1.0 }
  s.source       = { :git => "https://github.com/sensorworks/react-native-cesium.git", :tag => "#{s.version}" }

  cesium_xcframework = File.join(__dir__, "vendor", "ios", "CesiumNative.xcframework")
  unless File.exist?(cesium_xcframework)
    raise "react-native-cesium: Missing Cesium Native iOS prebuilts at #{cesium_xcframework}. Maintainer: run `yarn run update` and `yarn run build` (see package README)."
  end
  s.vendored_frameworks = "vendor/ios/CesiumNative.xcframework"

  cpp_include = File.join(__dir__, "cpp")
  nitro_cpp = File.join(__dir__, "nitrogen", "generated", "shared", "c++")
  s.pod_target_xcconfig = {
    # cpp/ must precede XCFramework Headers for <fmt/format.h>; CocoaPods merges $(inherited)
    # first, so apps should call react_native_cesium_post_install from ios/react_native_cesium_post_install.rb.
    # nitrogen/shared/c++ holds HybridCesiumViewSpec.hpp includes (CesiumMetrics.hpp, CameraState.hpp).
    "HEADER_SEARCH_PATHS" => "$(inherited) \"#{cpp_include}\" \"#{nitro_cpp}\"",
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "GCC_PREPROCESSOR_DEFINITIONS" => "$(inherited) GLM_FORCE_DEPTH_ZERO_TO_ONE=1",
  }

  s.frameworks   = "Metal", "MetalKit", "QuartzCore"

  s.source_files = [
    "ios/**/*.{swift}",
    "ios/**/*.{h,m,mm}",
    "cpp/**/*.{hpp,cpp,h,mm}",
  ]

  s.public_header_files = [
    "ios/ReactNativeCesium-umbrella.h",
    "ios/CesiumBridge.h",
  ]

  s.resource_bundles = {
    "ReactNativeCesiumShaders" => ["cpp/metal/*.metal"],
  }

  load 'nitrogen/generated/ios/ReactNativeCesium+autolinking.rb'
  add_nitrogen_files(s)

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end
