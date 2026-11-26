// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Exercises the full create -> configure -> start -> per-frame processing ->
// stop -> destroy lifecycle using FakeCaptureBackend, with zero hardware or
// ROS dependency. This is refactoring_plan.md stage 1's literal exit
// condition: "使用 fake backend 能走通创建、启动、逐帧处理、停止和销毁流程."

#include <cassert>
#include <cstdio>
#include <memory>

#include "qrb_audio_common_lib/core/audio_capture_backend.hpp"
#include "qrb_audio_common_lib/core/fake_capture_backend.hpp"
#include "qrb_audio_common_lib/core/passthrough_processor.hpp"
#include "qrb_audio_common_lib/core/processing_pipeline.hpp"

using namespace qrb::audio_common_lib::core;

int main()
{
  AudioStreamConfig config;
  config.sample_rate = 16000;
  config.channels = 1;
  config.sample_format = 16;
  config.frame_duration_ms = 20;

  FakeCaptureBackend backend;
  assert(backend.status() == AudioBackendStatus::kUnconnected);

  Result configure_result = backend.configure(config);
  assert(configure_result.ok());
  assert(backend.status() == AudioBackendStatus::kStopped);

  ProcessingPipeline pipeline;
  assert(pipeline.add_processor(std::make_unique<PassthroughProcessor>(), {}));

  int frames_received = 0;
  ProcessingContext ctx{config.sample_rate, config.channels, config.frame_duration_ms};

  Result start_result = backend.start([&](const AudioFrame & const_frame) {
    AudioFrame frame = const_frame;  // backend callback hands a const ref; pipeline mutates
    pipeline.process(frame, ctx);
    frames_received++;
    assert(frame.sequence == static_cast<uint64_t>(frames_received - 1));
    assert(frame.byte_size() == config.sample_rate * config.frame_duration_ms / 1000 *
                                    config.channels * (config.sample_format / 8));
  });
  assert(start_result.ok());
  assert(backend.status() == AudioBackendStatus::kReady);

  for (int i = 0; i < 5; ++i) {
    backend.emit_frame();
  }
  assert(frames_received == 5);
  assert(pipeline.stats()[0].bypassed_count == 5);

  Result stop_result = backend.stop();
  assert(stop_result.ok());
  assert(backend.status() == AudioBackendStatus::kStopped);

  // Frames emitted after stop() must be silently dropped, not crash or queue up.
  backend.emit_frame();
  assert(frames_received == 5);

  std::printf("core_fake_backend_test: full lifecycle PASS\n");
  return 0;
}
