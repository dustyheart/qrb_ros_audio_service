cmake_minimum_required(VERSION 3.8)
project(qrb_audio_common_lib VERSION 2.0.3)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -std=c++17)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Optional: WebRTC NS/VAD processors ("ns", "vad"). Without the library
# everything else builds and works; asking for ns/vad then fails with a
# message naming the missing package.
option(QRB_AUDIO_ENABLE_WEBRTC "Build WebRTC NS/VAD processors if webrtc-audio-processing is found" ON)
if(QRB_AUDIO_ENABLE_WEBRTC)
  pkg_check_modules(WEBRTC_APM QUIET webrtc-audio-processing)
endif()
if(WEBRTC_APM_FOUND)
  message(STATUS "qrb_audio_common_lib: WebRTC NS/VAD enabled (webrtc-audio-processing ${WEBRTC_APM_VERSION})")
elseif(QRB_AUDIO_ENABLE_WEBRTC)
  message(WARNING "qrb_audio_common_lib: webrtc-audio-processing not found - 'ns'/'vad' capture "
                  "processors are disabled. Install libwebrtc-audio-processing-dev to enable them.")
else()
  message(STATUS "qrb_audio_common_lib: WebRTC NS/VAD disabled (QRB_AUDIO_ENABLE_WEBRTC=OFF)")
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

if(WEBRTC_APM_FOUND)
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

  if(WEBRTC_APM_FOUND)
    add_executable(core_webrtc_processors_test test/core_webrtc_processors_test.cpp)
    target_include_directories(core_webrtc_processors_test PRIVATE include)
    target_link_libraries(core_webrtc_processors_test ${PROJECT_NAME})
    add_test(NAME core_webrtc_processors_test COMMAND core_webrtc_processors_test)
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
