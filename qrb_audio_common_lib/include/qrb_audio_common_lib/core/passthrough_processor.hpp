// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PASSTHROUGH_PROCESSOR_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PASSTHROUGH_PROCESSOR_HPP_

#include "qrb_audio_common_lib/core/audio_processor.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Does nothing to the frame; exists for pipeline/unit tests and as a bypass
// stand-in before a real NS/VAD/AEC processor is registered in that slot.
class PassthroughProcessor : public IAudioProcessor
{
public:
  std::string name() const override { return "passthrough"; }
  Result configure(const ProcessorConfig &) override { return Result::Ok(); }
  ProcessResult process(AudioFrame &, const ProcessingContext &) override
  {
    return ProcessResult{ ProcessResultStatus::kBypassed, Result::Ok() };
  }
  void reset() override {}
};

// Always fails process() - used by unit tests to exercise the
// ProcessingPipeline degraded/bypass policy without needing a real flaky
// processor.
class AlwaysFailProcessor : public IAudioProcessor
{
public:
  std::string name() const override { return "always_fail"; }
  Result configure(const ProcessorConfig &) override { return Result::Ok(); }
  ProcessResult process(AudioFrame &, const ProcessingContext &) override
  {
    return ProcessResult{ ProcessResultStatus::kFailed,
      Result::Error(ErrorCode::kProcessorProcessFailed, "injected failure") };
  }
  void reset() override {}
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PASSTHROUGH_PROCESSOR_HPP_
