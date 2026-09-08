# CorsixTH-iOS @build 2026-09-06 iOS device triplet pinning IPHONEOS_DEPLOYMENT_TARGET=16.0
#
# vcpkg's built-in community triplet `arm64-ios` leaves the deployment target unset, so every
# port inherits the SDK default and the resulting static libraries carry
# `LC_BUILD_VERSION minos <SDK version>` (26.5 with Xcode 26.6). Linking those into an app whose
# minimum is iOS 16.0 silently raises the real minimum OS, so pin it here instead.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)

set(CORSIXTH_IOS_DEPLOYMENT_TARGET 16.0)
list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CORSIXTH_IOS_DEPLOYMENT_TARGET}"
  # CMake defaults CMAKE_MACOSX_BUNDLE to ON for CMAKE_SYSTEM_NAME=iOS, which makes every
  # helper executable a .app and breaks ports that install() them with only a RUNTIME
  # DESTINATION (fluidsynth's CLI, for one). We never ship a dependency's executable.
  "-DCMAKE_MACOSX_BUNDLE=OFF"
)
