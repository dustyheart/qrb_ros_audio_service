// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Plain-assert unit test for the core data model / ProcessingPipeline.
// Deliberately has zero dependency on ROS, PulseAudio, or real hardware -
// this is the "fake backend, no hardware" test refactoring_plan.md section 7
// (stage 1 exit condition) asks for. Registered as a CTest case, not gtest,
// to avoid adding a new test framework dependency for four checks.

#include <cassert>
#include <cstdio>
#include <memory>

#include "qrb_audio_common_lib/core/audio_frame.hpp"
#include "qrb_audio_common_lib/core/audio_processor.hpp"
#include "qrb_audio_common_lib/core/passthrough_processor.hpp"
#include "qrb_audio_common_lib/core/processing_pipeline.hpp"

using namespace qrb::audio_common_lib::core;

namespace
{

AudioFrame make_frame(uint64_t seq)
{
  AudioFrame frame;
  frame.data = std::make_shared<std::vector<uint8_t>>(640, 0);
  frame.sample_rate = 16000;
  frame.channels = 1;
  frame.sample_format = 16;
  frame.frame_count = 320;
  frame.sequence = seq;
  return frame;
}

void test_passthrough_marks_bypassed()
{
  ProcessingPipeline pipeline;
  bool added = pipeline.add_processor(std::make_unique<PassthroughProcessor>(), {});
  assert(added);
  assert(pipeline.processor_count() == 1);

  AudioFrame frame = make_frame(0);
  ProcessingContext ctx{16000, 1, 20};
  pipeline.process(frame, ctx);

  assert(has_flag(frame.flags, FrameFlag::kBypassed));
  assert(pipeline.stats()[0].bypassed_count == 1);
  assert(pipeline.stats()[0].health == ProcessorHealth::kHealthy);
  std::printf("test_passthrough_marks_bypassed: PASS\n");
}

void test_duplicate_name_rejected()
{
  ProcessingPipeline pipeline;
  assert(pipeline.add_processor(std::make_unique<PassthroughProcessor>(), {}));
  assert(!pipeline.add_processor(std::make_unique<PassthroughProcessor>(), {}));
  assert(pipeline.processor_count() == 1);
  std::printf("test_duplicate_name_rejected: PASS\n");
}

void test_degrades_after_consecutive_failures()
{
  ProcessingPipeline pipeline;
  assert(pipeline.add_processor(std::make_unique<AlwaysFailProcessor>(), {}));

  ProcessingContext ctx{16000, 1, 20};
  for (uint64_t i = 0; i < ProcessingPipeline::kMaxConsecutiveFailures - 1; ++i) {
    AudioFrame frame = make_frame(i);
    pipeline.process(frame, ctx);
    assert(pipeline.stats()[0].health == ProcessorHealth::kHealthy);
  }

  AudioFrame tipping_frame = make_frame(999);
  pipeline.process(tipping_frame, ctx);
  assert(pipeline.stats()[0].health == ProcessorHealth::kDegraded);
  assert(pipeline.stats()[0].failed_count == ProcessingPipeline::kMaxConsecutiveFailures);

  // Once degraded, frames must keep flowing through untouched (bypassed),
  // not get stuck/dropped - "VAD 失败不得阻断 PCM 输出" generalizes to all stages.
  AudioFrame after_degraded = make_frame(1000);
  pipeline.process(after_degraded, ctx);
  assert(has_flag(after_degraded.flags, FrameFlag::kBypassed));
  assert(pipeline.stats()[0].bypassed_count == 1);

  std::printf("test_degrades_after_consecutive_failures: PASS\n");
}

void test_failed_configure_skips_processor_forever()
{
  class FailConfigureProcessor : public IAudioProcessor
  {
  public:
    std::string name() const override { return "fail_configure"; }
    Result configure(const ProcessorConfig &) override
    {
      return Result::Error(ErrorCode::kProcessorInitFailed, "nope");
    }
    ProcessResult process(AudioFrame &, const ProcessingContext &) override
    {
      called_ = true;
      return ProcessResult{ProcessResultStatus::kProcessed, Result::Ok()};
    }
    void reset() override {}
    bool called_ = false;
  };

  ProcessingPipeline pipeline;
  auto owned = std::make_unique<FailConfigureProcessor>();
  FailConfigureProcessor * raw = owned.get();
  assert(pipeline.add_processor(std::move(owned), {}));
  assert(pipeline.stats()[0].health == ProcessorHealth::kFailed);

  ProcessingContext ctx{16000, 1, 20};
  AudioFrame frame = make_frame(0);
  pipeline.process(frame, ctx);
  assert(!raw->called_);  // process() must never be called on a failed-init processor
  std::printf("test_failed_configure_skips_processor_forever: PASS\n");
}

}  // namespace

int main()
{
  test_passthrough_marks_bypassed();
  test_duplicate_name_rejected();
  test_degrades_after_consecutive_failures();
  test_failed_configure_skips_processor_forever();
  std::printf("all core pipeline tests PASS\n");
  return 0;
}
