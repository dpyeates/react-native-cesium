# Overlay for microsoft/vcpkg triplets/community/arm64-ios-simulator.cmake — curl's CMake can set
# HAVE_PIPE2 even though iOS has no pipe2(); that fails with -Werror=implicit-function-declaration.
#
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY: FindThreads / pthread checks (e.g. via zstd → ktx).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DHAVE_PIPE2=OFF;-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY")
