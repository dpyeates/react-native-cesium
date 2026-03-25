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
  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => "$(inherited) \"#{cpp_include}\"",
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
  }

  # GPU: Metal (iOS). Link frameworks used by Metal-based rendering.
  s.frameworks   = "Metal", "MetalKit", "QuartzCore"

  s.source_files = [
    # Implementation (Swift)
    "ios/**/*.{swift}",
    # Autolinking/Registration (Objective-C++)
    "ios/**/*.{m,mm}",
    # Implementation (C++ objects)
    "cpp/**/*.{hpp,cpp}",
  ]

  load 'nitrogen/generated/ios/ReactNativeCesium+autolinking.rb'
  add_nitrogen_files(s)

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end
