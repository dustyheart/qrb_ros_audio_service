cmake_minimum_required(VERSION 3.8)
project(qrb_audio_common_lib VERSION 2.0.3)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -std=c++17)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Optional: WebRTC capture processors. Without a library everything else
# builds and works; asking for a missing processor fails with a message
# naming the package.
#   v1     - webrtc-audio-processing 1.x (pkg-config webrtc-audio-processing-1):
#            "aec" (AEC3), "ns", "vad"; aec + ns share one APM.
#   legacy - system webrtc-audio-processing 0.3.1 (Ubuntu 24.04
#            libwebrtc-audio-processing-dev): "ns", "vad" only, no AEC.
#   auto   - v1 if found, else legacy if found, else none.
# The two libraries export ~400 identical symbols, so exactly one is ever
# linked (stage 6, aec3_ab_test.md). 1.x is not packaged for Ubuntu 24.04; it
# comes from the webrtc_audio_processing_vendor package in this repository
# (found automatically), or from any install prefix given in
# QRB_AUDIO_WEBRTC1_PREFIX / PKG_CONFIG_PATH. The library is found at run time
# through an RPATH.
option(QRB_AUDIO_ENABLE_WEBRTC "Build WebRTC capture processors if a webrtc-audio-processing is found" ON)
set(QRB_AUDIO_WEBRTC_IMPL "auto" CACHE STRING "WebRTC implementation: auto, v1 or legacy")
set_property(CACHE QRB_AUDIO_WEBRTC_IMPL PROPERTY STRINGS auto v1 legacy)
set(QRB_AUDIO_WEBRTC1_PREFIX "$ENV{QRB_AUDIO_WEBRTC1_PREFIX}" CACHE PATH
    "Install prefix of webrtc-audio-processing 1.x (optional, adds it to the pkg-config path)")
set(QRB_AUDIO_WEBRTC "none")
if(QRB_AUDIO_ENABLE_WEBRTC)
  if(NOT QRB_AUDIO_WEBRTC_IMPL MATCHES "^(auto|v1|legacy)$")
    message(FATAL_ERROR "QRB_AUDIO_WEBRTC_IMPL must be auto, v1 or legacy, got '${QRB_AUDIO_WEBRTC_IMPL}'")
  endif()
  if(QRB_AUDIO_WEBRTC_IMPL MATCHES "^(auto|v1)$")
    set(_qrb_webrtc1_prefix "${QRB_AUDIO_WEBRTC1_PREFIX}")
    if(NOT _qrb_webrtc1_prefix)
      find_package(webrtc_audio_processing_vendor QUIET)
      if(webrtc_audio_processing_vendor_FOUND)
        # <prefix>/share/webrtc_audio_processing_vendor/cmake -> <prefix>
        get_filename_component(_qrb_webrtc1_prefix
          "${webrtc_audio_processing_vendor_DIR}/../../.." ABSOLUTE)
      endif()
    endif()
    if(_qrb_webrtc1_prefix)
      file(GLOB _qrb_webrtc1_pc_dirs "${_qrb_webrtc1_prefix}/lib/pkgconfig"
           "${_qrb_webrtc1_prefix}/lib/*/pkgconfig")
      foreach(_d ${_qrb_webrtc1_pc_dirs})
        set(ENV{PKG_CONFIG_PATH} "${_d}:$ENV{PKG_CONFIG_PATH}")
      endforeach()
    endif()
    pkg_check_modules(WEBRTC_APM1 QUIET webrtc-audio-processing-1)
    if(WEBRTC_APM1_FOUND)
      set(QRB_AUDIO_WEBRTC "v1")
    elseif(QRB_AUDIO_WEBRTC_IMPL STREQUAL "v1")
      message(WARNING "qrb_audio_common_lib: QRB_AUDIO_WEBRTC_IMPL=v1 but webrtc-audio-processing-1 "
                      "was not found (build webrtc_audio_processing_vendor or set "
                      "QRB_AUDIO_WEBRTC1_PREFIX); building without WebRTC processors.")
    endif()
  endif()
  if(QRB_AUDIO_WEBRTC STREQUAL "none" AND QRB_AUDIO_WEBRTC_IMPL MATCHES "^(auto|legacy)$")
    pkg_check_modules(WEBRTC_APM QUIET webrtc-audio-processing)
    if(WEBRTC_APM_FOUND)
      set(QRB_AUDIO_WEBRTC "legacy")
    endif()
  endif()
endif()
if(QRB_AUDIO_WEBRTC STREQUAL "v1")
  message(STATUS "qrb_audio_common_lib: WebRTC AEC/NS/VAD enabled (webrtc-audio-processing ${WEBRTC_APM1_VERSION}, ${WEBRTC_APM1_LIBDIR})")
elseif(QRB_AUDIO_WEBRTC STREQUAL "legacy")
  message(STATUS "qrb_audio_common_lib: WebRTC NS/VAD enabled, no AEC (webrtc-audio-processing ${WEBRTC_APM_VERSION})")
elseif(QRB_AUDIO_ENABLE_WEBRTC)
  message(WARNING "qrb_audio_common_lib: no webrtc-audio-processing found - 'aec'/'ns'/'vad' capture "
                  "processors are disabled. Build webrtc_audio_processing_vendor (needs meson, "
                  "ninja) or install libwebrtc-audio-processing-dev (ns/vad only).")
else()
  message(STATUS "qrb_audio_common_lib: WebRTC processors disabled (QRB_AUDIO_ENABLE_WEBRTC=OFF)")
endif()

add_library(${PROJECT_NAME} SHARED
  src/audio_stream.cpp
  src/alsa_stream.cpp
  src/pulse_stream.cpp
  src/audio_common_wrapper.cpp
  src/core/processing_pipeline.cpp
  src/core/pipewire_pulse_backend.cpp
  src/core/processing_capture_backend.cpp
  src/core/processor_factory.cpp
)

if(QRB_AUDIO_WEBRTC STREQUAL "v1")
  target_sources(${PROJECT_NAME} PRIVATE src/core/webrtc1_processors.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE QRB_AUDIO_HAVE_WEBRTC_APM1)
  target_compile_options(${PROJECT_NAME} PRIVATE ${WEBRTC_APM1_CFLAGS_OTHER})
  # SYSTEM: keep warnings from the library's (and abseil's) own headers out.
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${WEBRTC_APM1_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${WEBRTC_APM1_LINK_LIBRARIES})
  # 1.x usually lives in a private prefix: let the installed library find it.
  set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "${WEBRTC_APM1_LIBRARY_DIRS}")
elseif(QRB_AUDIO_WEBRTC STREQUAL "legacy")
  target_sources(${PROJECT_NAME} PRIVATE src/core/webrtc_processors.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE QRB_AUDIO_HAVE_WEBRTC_APM)
  target_compile_options(${PROJECT_NAME} PRIVATE ${WEBRTC_APM_CFLAGS_OTHER})
  # SYSTEM: the 0.3.1 headers trigger -Wunused-parameter on their own code.
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${WEBRTC_APM_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${WEBRTC_APM_LIBRARIES})
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    ${ALSA_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    pulse
    Threads::Threads
)

set_target_properties(${PROJECT_NAME}
  PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

install(FILES
  include/${PROJECT_NAME}/audio_common_wrapper.hpp
  DESTINATION include/${PROJECT_NAME}
)

install(DIRECTORY include/${PROJECT_NAME}/core
  DESTINATION include/${PROJECT_NAME}
)

if(BUILD_TESTING)
  enable_testing()
  add_executable(core_pipeline_test test/core_pipeline_test.cpp)
  target_include_directories(core_pipeline_test PRIVATE include)
  target_link_libraries(core_pipeline_test ${PROJECT_NAME})
  add_test(NAME core_pipeline_test COMMAND core_pipeline_test)

  add_executable(core_fake_backend_test test/core_fake_backend_test.cpp)
  target_include_directories(core_fake_backend_test PRIVATE include)
  target_link_libraries(core_fake_backend_test ${PROJECT_NAME})
  add_test(NAME core_fake_backend_test COMMAND core_fake_backend_test)

  add_executable(core_config_validation_test test/core_config_validation_test.cpp)
  target_include_directories(core_config_validation_test PRIVATE include)
  add_test(NAME core_config_validation_test COMMAND core_config_validation_test)

  add_executable(core_capture_timestamp_test test/core_capture_timestamp_test.cpp)
  target_include_directories(core_capture_timestamp_test PRIVATE include)
  add_test(NAME core_capture_timestamp_test COMMAND core_capture_timestamp_test)

  add_executable(core_chunk_reframer_test test/core_chunk_reframer_test.cpp)
  target_include_directories(core_chunk_reframer_test PRIVATE include)
  add_test(NAME core_chunk_reframer_test COMMAND core_chunk_reframer_test)

  add_executable(core_processing_backend_test test/core_processing_backend_test.cpp)
  target_include_directories(core_processing_backend_test PRIVATE include)
  target_link_libraries(core_processing_backend_test ${PROJECT_NAME})
  add_test(NAME core_processing_backend_test COMMAND core_processing_backend_test)

  add_executable(core_vad_hangover_test test/core_vad_hangover_test.cpp)
  target_include_directories(core_vad_hangover_test PRIVATE include)
  add_test(NAME core_vad_hangover_test COMMAND core_vad_hangover_test)

  add_executable(core_reference_buffer_test test/core_reference_buffer_test.cpp)
  target_include_directories(core_reference_buffer_test PRIVATE include)
  target_link_libraries(core_reference_buffer_test Threads::Threads)
  add_test(NAME core_reference_buffer_test COMMAND core_reference_buffer_test)

  if(NOT QRB_AUDIO_WEBRTC STREQUAL "none")
    add_executable(core_webrtc_processors_test test/core_webrtc_processors_test.cpp)
    target_include_directories(core_webrtc_processors_test PRIVATE include)
    target_link_libraries(core_webrtc_processors_test ${PROJECT_NAME})
    add_test(NAME core_webrtc_processors_test COMMAND core_webrtc_processors_test)
  endif()
  if(QRB_AUDIO_WEBRTC STREQUAL "v1")
    add_executable(core_aec_test test/core_aec_test.cpp)
    target_include_directories(core_aec_test PRIVATE include)
    target_link_libraries(core_aec_test ${PROJECT_NAME} Threads::Threads)
    add_test(NAME core_aec_test COMMAND core_aec_test)
  endif()

  # Hardware-dependent; not registered with CTest, built for manual use only.
  add_executable(manual_pipewire_backend_capture test/manual_pipewire_backend_capture.cpp)
  target_include_directories(manual_pipewire_backend_capture PRIVATE include)
  target_link_libraries(manual_pipewire_backend_capture ${PROJECT_NAME})
endif()

install(TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}Config
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(EXPORT ${PROJECT_NAME}Config
  DESTINATION share/${PROJECT_NAME}/cmake
)
