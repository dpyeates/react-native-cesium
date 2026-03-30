# Prepend this package's cpp/ to HEADER_SEARCH_PATHS for ReactNativeCesium so
# cpp/fmt/format.h shadows CesiumNative.xcframework's fmt before <fmt/format.h>.
def react_native_cesium_post_install(installer)
  cpp_root = File.expand_path("../cpp", __dir__)
  installer.pods_project.targets.each do |target|
    next unless target.name == "ReactNativeCesium"

    target.build_configurations.each do |config|
      existing = config.build_settings["HEADER_SEARCH_PATHS"] || "$(inherited)"
      config.build_settings["HEADER_SEARCH_PATHS"] = "\"#{cpp_root}\" #{existing}"
    end
  end
end
