project(ReactNativeCesium)
cmake_minimum_required(VERSION 3.15.0)

set (PACKAGE_NAME ReactNativeCesium)
set (CMAKE_VERBOSE_MAKEFILE ON)
set (CMAKE_CXX_STANDARD 20)

# Enable Raw Props parsing in react-native (for Nitro Views)
add_compile_options(-DRN_SERIALIZABLE_STATE=1)

# Define C++ library and add all sources
add_library(${PACKAGE_NAME} SHARED
        src/main/cpp/cpp-adapter.cpp
        ../cpp/cesium-native-smoke.cpp
)

# Add Nitrogen specs :)
include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/ReactNativeCesium+autolinking.cmake)

# Set up local includes
include_directories(
        "src/main/cpp"
        "../cpp"
)

find_library(LOG_LIB log)
find_library(VULKAN_LIB vulkan)

# Cesium Native (prebuilt via `yarn run build` — see README)
set(CESIUM_PREFIX "${CMAKE_SOURCE_DIR}/../vendor/android")
if (NOT EXISTS "${CESIUM_PREFIX}/share/cesium-native/cmake/cesium-nativeConfig.cmake")
  message(FATAL_ERROR "Cesium Native Android prebuilts missing. Maintainer: run `yarn run update` and `yarn run build` (see README). Expected: ${CESIUM_PREFIX}/share/cesium-native/cmake/cesium-nativeConfig.cmake")
endif()
list(APPEND CMAKE_PREFIX_PATH "${CESIUM_PREFIX}")
# Android NDK toolchain uses CMAKE_FIND_ROOT_PATH for package lookup; without this,
# find_package ignores CMAKE_PREFIX_PATH and prebuilts are not found.
list(APPEND CMAKE_FIND_ROOT_PATH "${CESIUM_PREFIX}")
find_package(cesium-native CONFIG REQUIRED)

# Link all libraries together (Vulkan loader for GPU work on Android)
target_link_libraries(
        ${PACKAGE_NAME}
        ${LOG_LIB}
        ${VULKAN_LIB}
        android
        CesiumUtility
)
