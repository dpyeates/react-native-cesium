# Overlay for microsoft/vcpkg triplets/community/arm64-ios.cmake — curl's CMake can set
# HAVE_PIPE2 even though iOS has no pipe2(); that fails with -Werror=implicit-function-declaration.
#
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY: FindThreads (pulled in via zstdConfig → ktx, etc.)
# fails on iOS when try_compile builds an executable; static libs avoid that toolchain quirk.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
# vcpkg scripts/toolchains/ios.cmake only sets a sysroot for simulator archs; without this,
# CMake defaults CMAKE_OSX_SYSROOT to the host MacOSX SDK and Clang builds with an invalid
# --target=…-macabi mix (breaks ktx/astc-encoder on newer Xcode).
set(VCPKG_OSX_SYSROOT iphoneos)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DHAVE_PIPE2=OFF;-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY")
