cmake_minimum_required(VERSION 3.8)
project(qrb_audio_common_lib VERSION 2.0.3)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -std=c++17)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(SNDFILE REQUIRED sndfile)

add_library(${PROJECT_NAME} SHARED
  src/audio_stream.cpp
  src/alsa_stream.cpp
  src/pulse_stream.cpp
  src/audio_common_wrapper.cpp
  src/core/processing_pipeline.cpp
  src/core/pipewire_pulse_backend.cpp
)

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
