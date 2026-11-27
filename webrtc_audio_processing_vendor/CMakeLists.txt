cmake_minimum_required(VERSION 3.8)
project(webrtc_audio_processing_vendor VERSION 1.3.0)

# Builds the freedesktop.org webrtc-audio-processing 1.x (AEC3, new NS, WebRTC
# VAD) for qrb_audio_common_lib's "aec"/"ns"/"vad" capture processors. Ubuntu
# 24.04 only packages 0.3.1 (no AEC3), and 0.3.1 and 1.x must never be linked
# into one process (they export ~400 identical symbols), so 1.x is vendored here
# under its own soname (libwebrtc-audio-processing-1.so.3) instead.
#
# The upstream build is meson + ninja. Output is staged with DESTDIR and then
# installed by CMake, so the installed pkg-config files carry the real install
# prefix both for colcon workspaces and for bloom/debian packaging.
#
# Offline builds: set WEBRTC_APM_URL to a local copy of the tarball (same hash).

find_package(ament_cmake REQUIRED)
include(ExternalProject)

set(WEBRTC_APM_VERSION "1.3")
set(WEBRTC_APM_URL
    "https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-${WEBRTC_APM_VERSION}.tar.xz"
    CACHE STRING "webrtc-audio-processing release tarball (URL or local path)")
set(WEBRTC_APM_SHA256 "2365e93e778d7b61b5d6e02d21c47d97222e9c7deff9e1d0838ad6ec2e86f1b9"
    CACHE STRING "SHA256 of the tarball")
option(WEBRTC_APM_BUILD "Build webrtc-audio-processing (OFF: install nothing; consumers fall back)" ON)

find_program(MESON_EXECUTABLE meson)
find_program(NINJA_EXECUTABLE NAMES ninja ninja-build)

if(NOT WEBRTC_APM_BUILD)
  message(STATUS "webrtc_audio_processing_vendor: WEBRTC_APM_BUILD=OFF, nothing is built")
elseif(NOT MESON_EXECUTABLE OR NOT NINJA_EXECUTABLE)
  # Not fatal: qrb_audio_common_lib then uses the system 0.3.1 (ns/vad only) or
  # builds without WebRTC processors.
  message(WARNING "webrtc_audio_processing_vendor: meson and/or ninja not found "
                  "(rosdep keys meson, ninja-build) - webrtc-audio-processing ${WEBRTC_APM_VERSION} "
                  "is NOT built; the 'aec' capture processor will be unavailable.")
else()
  set(_stage "${CMAKE_CURRENT_BINARY_DIR}/stage")
  ExternalProject_Add(webrtc_apm
    URL "${WEBRTC_APM_URL}"
    URL_HASH "SHA256=${WEBRTC_APM_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ${MESON_EXECUTABLE} setup <BINARY_DIR> <SOURCE_DIR>
      --prefix=${CMAKE_INSTALL_PREFIX} --libdir=lib --buildtype=release
      -Dcpp_args=-w
    BUILD_COMMAND ${NINJA_EXECUTABLE} -C <BINARY_DIR>
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env DESTDIR=${_stage} ${NINJA_EXECUTABLE} -C <BINARY_DIR> install
  )
  # ${_stage}${CMAKE_INSTALL_PREFIX} = what meson installed (lib, include, lib/pkgconfig).
  install(DIRECTORY "${_stage}${CMAKE_INSTALL_PREFIX}/" DESTINATION "." USE_SOURCE_PERMISSIONS)
  message(STATUS "webrtc_audio_processing_vendor: building webrtc-audio-processing ${WEBRTC_APM_VERSION}")
endif()

ament_package()
