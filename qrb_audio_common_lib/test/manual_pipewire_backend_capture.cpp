// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Manual, hardware-dependent verification harness for PipeWirePulseBackend.
// NOT registered with CTest (needs a real capture device); run by hand:
//   ./manual_pipewire_backend_capture <device_name> <fragment_duration_ms>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "qrb_audio_common_lib/core/fake_capture_backend.hpp"
#include "qrb_audio_common_lib/core/passthrough_processor.hpp"
#include "qrb_audio_common_lib/core/pipewire_pulse_backend.hpp"
#include "qrb_audio_common_lib/core/processing_pipeline.hpp"

using namespace qrb::audio_common_lib::core;

int main(int argc, char ** argv)
{
  std::string device = argc > 1 ? argv[1] : "";
  uint32_t fragment_ms = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 20;

  AudioStreamConfig config;
  config.device = device;
  config.sample_rate = 16000;
  config.channels = 1;
  config.sample_format = 16;
  config.frame_duration_ms = fragment_ms;
  config.fragment_duration_ms = fragment_ms;

  PipeWirePulseBackend backend;
  Result configure_result = backend.configure(config);
  printf("[manual] configure() ok=%d msg=%s\n", configure_result.ok(), configure_result.message.c_str());
  if (!configure_result.ok())
    return 1;

  ProcessingPipeline pipeline;
  pipeline.add_processor(std::make_unique<PassthroughProcessor>(), {});
  ProcessingContext ctx{ config.sample_rate, config.channels, config.frame_duration_ms };

  std::atomic<int> frame_count{ 0 };
  std::atomic<size_t> last_bytes{ 0 };

  Result start_result = backend.start([&](const AudioFrame & const_frame) {
    AudioFrame frame = const_frame;
    pipeline.process(frame, ctx);
    frame_count++;
    last_bytes = frame.byte_size();
    if (frame_count <= 3) {
      printf("[manual] frame #%d bytes=%zu flags=0x%x\n", frame_count.load(), frame.byte_size(),
          frame.flags);
    }
  });
  printf("[manual] start() ok=%d msg=%s status=%d\n", start_result.ok(),
      start_result.message.c_str(), static_cast<int>(backend.status()));
  if (!start_result.ok())
    return 1;

  std::this_thread::sleep_for(std::chrono::seconds(2));

  backend.stop();
  printf("[manual] total frames=%d last_frame_bytes=%zu status_after_stop=%d\n", frame_count.load(),
      last_bytes.load(), static_cast<int>(backend.status()));
  printf("[manual] %s\n", frame_count.load() > 0 ? "PASS" : "FAIL");
  return frame_count.load() > 0 ? 0 : 1;
}
