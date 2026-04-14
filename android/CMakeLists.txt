project(ReactNativeCesium)
cmake_minimum_required(VERSION 3.15.0)

set (PACKAGE_NAME ReactNativeCesium)
set (CMAKE_VERBOSE_MAKEFILE ON)
set (CMAKE_CXX_STANDARD 20)

# Enable Raw Props parsing in react-native (for Nitro Views)
add_compile_options(-DRN_SERIALIZABLE_STATE=1)

# ── SPIR-V shader compilation ──────────────────────────────────────────────────
# NDK ships glslc in shader-tools; find it automatically.
find_program(GLSLC glslc
  HINTS "${ANDROID_NDK}/shader-tools/${CMAKE_HOST_SYSTEM_PROCESSOR}-${CMAKE_HOST_SYSTEM_NAME}"
        "${ANDROID_NDK}/shader-tools/darwin-x86_64"
        "${ANDROID_NDK}/shader-tools/linux-x86_64"
        "${ANDROID_NDK}/shader-tools/windows-x86_64"
)
if (NOT GLSLC)
  message(FATAL_ERROR "glslc not found in NDK shader-tools. Ensure ANDROID_NDK points to a valid NDK (>=r21).")
endif()

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/src/main/cpp/vulkan/shaders")
set(SPIRV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SPIRV_OUTPUT_DIR}")

set(SHADER_SOURCES
  "${SHADER_DIR}/terrain.vert"
  "${SHADER_DIR}/terrain.frag"
  "${SHADER_DIR}/sky.vert"
  "${SHADER_DIR}/sky.frag"
)

foreach(SHADER ${SHADER_SOURCES})
  get_filename_component(SHADER_NAME ${SHADER} NAME)
  set(SPV_FILE "${SPIRV_OUTPUT_DIR}/${SHADER_NAME}.spv")
  set(SPV_HEADER "${SPIRV_OUTPUT_DIR}/${SHADER_NAME}.spv.h")
  string(REGEX REPLACE "[^a-zA-Z0-9]" "_" ARRAY_NAME "spv_${SHADER_NAME}")

  add_custom_command(
    OUTPUT "${SPV_FILE}"
    COMMAND "${GLSLC}" -O -o "${SPV_FILE}" "${SHADER}"
    DEPENDS "${SHADER}"
    COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
  )

  # Generate a C header embedding the SPIR-V as a uint32_t array
  add_custom_command(
    OUTPUT "${SPV_HEADER}"
    COMMAND ${CMAKE_COMMAND}
      -DSPV_FILE=${SPV_FILE}
      -DSPV_HEADER=${SPV_HEADER}
      -DARRAY_NAME=${ARRAY_NAME}
      -P "${CMAKE_SOURCE_DIR}/cmake/spv_to_header.cmake"
    DEPENDS "${SPV_FILE}"
    COMMENT "Embedding ${SHADER_NAME}.spv -> header"
  )
  list(APPEND SPIRV_HEADERS "${SPV_HEADER}")
endforeach()

add_custom_target(compile_shaders DEPENDS ${SPIRV_HEADERS})

# Define C++ library and add all sources
add_library(${PACKAGE_NAME} SHARED
        src/main/cpp/cpp-adapter.cpp
        # Engine (shared with iOS)
        ../cpp/engine/CesiumEngine.cpp
        ../cpp/engine/TaskProcessor.cpp
        ../cpp/engine/ResourcePreparer.cpp
        ../cpp/engine/GltfToMesh.cpp
        ../cpp/engine/GlobeCamera.cpp
        ../cpp/engine/DeletionQueue.cpp
        ../cpp/engine/TileLifecycleManager.cpp
        # Vulkan backend
        src/main/cpp/vulkan/VulkanBackend.cpp
        # JNI bridge
        src/main/cpp/CesiumBridgeAndroid.cpp
)

add_dependencies(${PACKAGE_NAME} compile_shaders)

# Add Nitrogen specs :)
include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/ReactNativeCesium+autolinking.cmake)

# Set up local includes
include_directories(
        BEFORE
        "../cpp"
        "src/main/cpp"
)

# Generated SPIR-V headers
target_include_directories(${PACKAGE_NAME} PRIVATE "${SPIRV_OUTPUT_DIR}")

find_library(LOG_LIB log)
find_library(VULKAN_LIB vulkan)

# Cesium Native (vendor/android from `yarn run build` or Gradle ensure-native — see README)
set(CESIUM_PREFIX "${CMAKE_SOURCE_DIR}/../vendor/android")
if (NOT EXISTS "${CESIUM_PREFIX}/share/cesium-native/cmake/cesium-nativeConfig.cmake")
  message(FATAL_ERROR "Cesium Native Android artifacts missing under vendor/android. Gradle should run ensure-native.mjs before CMake; or run `npm run update` and `CESIUM_BUILD_ONLY=android npm run build` from the package root. Expected: ${CESIUM_PREFIX}/share/cesium-native/cmake/cesium-nativeConfig.cmake")
endif()
list(APPEND CMAKE_PREFIX_PATH "${CESIUM_PREFIX}")
list(APPEND CMAKE_FIND_ROOT_PATH "${CESIUM_PREFIX}")

# cesium-native ships Find modules and header-only shim targets that aren't
# always resolved by the default CMake search paths.
list(APPEND CMAKE_MODULE_PATH "${CESIUM_PREFIX}/share/cesium-native/cmake")

# modp_b64 has a static lib + Find module
find_package(modp_b64 REQUIRED)

# Header-only shim targets referenced by cesium-native cmake configs but not
# shipped with their own Config.cmake in the vcpkg install.
if (NOT TARGET picosha2::picosha2)
  add_library(picosha2::picosha2 INTERFACE IMPORTED)
  set_target_properties(picosha2::picosha2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CESIUM_PREFIX}/include")
endif()
if (NOT TARGET nonstd::expected-lite)
  add_library(nonstd::expected-lite INTERFACE IMPORTED)
  set_target_properties(nonstd::expected-lite PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CESIUM_PREFIX}/include")
endif()

find_package(cesium-native CONFIG REQUIRED)

# Link all libraries together
target_link_libraries(
        ${PACKAGE_NAME}
        ${LOG_LIB}
        ${VULKAN_LIB}
        android
        # Cesium Native libraries
        Cesium3DTilesSelection
        Cesium3DTilesContent
        CesiumGltf
        CesiumGltfReader
        CesiumGltfContent
        CesiumGeospatial
        CesiumGeometry
        CesiumAsync
        CesiumIonClient
        CesiumRasterOverlays
        CesiumCurl
        CesiumJsonReader
        CesiumUtility
)
