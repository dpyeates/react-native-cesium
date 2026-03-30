# Prepend this package's cpp/ to HEADER_SEARCH_PATHS for ReactNativeCesium so
# cpp/fmt/format.h shadows CesiumNative.xcframework's fmt before <fmt/format.h>.
#
# CesiumNative.xcframework simulator slice is arm64-only; exclude x86_64 so
# generic "iOS Simulator" builds link (Apple Silicon). Intel Mac simulators
# need an xcframework that includes x86_64.
def react_native_cesium_post_install(installer)
  cpp_root = File.expand_path("../cpp", __dir__)
  sim_arch_key = "EXCLUDED_ARCHS[sdk=iphonesimulator*]"

  installer.pods_project.targets.each do |target|
    next unless target.name == "ReactNativeCesium"

    target.build_configurations.each do |config|
      existing = config.build_settings["HEADER_SEARCH_PATHS"] || "$(inherited)"
      config.build_settings["HEADER_SEARCH_PATHS"] = "\"#{cpp_root}\" #{existing}"
      config.build_settings[sim_arch_key] = "x86_64"
    end
  end

  installer.aggregate_targets.each do |aggregate_target|
    aggregate_target.user_project.native_targets.each do |native_target|
      native_target.build_configurations.each do |config|
        cur = config.build_settings[sim_arch_key].to_s.strip
        config.build_settings[sim_arch_key] =
          cur.empty? ? "x86_64" : "#{cur} x86_64".split.uniq.join(" ")
      end
    end
  end
end
