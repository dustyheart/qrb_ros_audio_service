// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qrb_audio_common_lib/core/processor_factory.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "qrb_audio_common_lib/core/passthrough_processor.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

namespace
{

// Deliberately violates IAudioProcessor's "must return promptly" rule: it
// exists to show that a slow processor only delays/drops processed output on
// the worker thread and never blocks the PulseAudio mainloop.
class TestDelayProcessor : public IAudioProcessor
{
public:
  std::string name() const override { return "test_delay"; }

  Result configure(const ProcessorConfig & config) override
  {
    auto it = config.find("delay_us");
    if (it == config.end())
      return Result::Ok();
    char * end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (it->second.empty() || *end != '\0' || v < 0 || v > 1000000) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "test_delay: delay_us must be an integer in [0, 1000000], got '" + it->second + "'");
    }
    delay_us_ = static_cast<uint32_t>(v);
    return Result::Ok();
  }

  ProcessResult process(AudioFrame &, const ProcessingContext &) override
  {
    std::this_thread::sleep_for(std::chrono::microseconds(delay_us_));
    return ProcessResult{};
  }

  void reset() override {}

private:
  uint32_t delay_us_ = 1000;
};

}  // namespace

std::vector<std::string> known_processor_names()
{
  return {"passthrough", "test_delay"};
}

std::unique_ptr<IAudioProcessor> create_processor(const std::string & name)
{
  if (name == "passthrough")
    return std::make_unique<PassthroughProcessor>();
  if (name == "test_delay")
    return std::make_unique<TestDelayProcessor>();
  return nullptr;
}

Result parse_processor_spec(const std::string & spec, ProcessorSlot & out)
{
  out = ProcessorSlot{};
  const auto colon = spec.find(':');
  out.name = spec.substr(0, colon);
  if (out.name.empty())
    return Result::Error(ErrorCode::kInvalidConfig, "processor spec '" + spec + "': empty name");
  if (!create_processor(out.name)) {
    std::string known;
    for (const auto & n : known_processor_names())
      known += (known.empty() ? "" : ", ") + n;
    return Result::Error(
        ErrorCode::kInvalidConfig, "unknown processor '" + out.name + "' (known: " + known + ")");
  }
  if (colon == std::string::npos)
    return Result::Ok();

  const std::string rest = spec.substr(colon + 1);
  size_t pos = 0;
  while (pos <= rest.size()) {
    size_t comma = rest.find(',', pos);
    if (comma == std::string::npos)
      comma = rest.size();
    const std::string kv = rest.substr(pos, comma - pos);
    const auto eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "processor spec '" + spec + "': expected key=value, got '" + kv + "'");
    }
    out.config[kv.substr(0, eq)] = kv.substr(eq + 1);
    pos = comma + 1;
  }
  return Result::Ok();
}

Result build_pipeline(const std::vector<ProcessorSlot> & slots,
    std::unique_ptr<ProcessingPipeline> & out)
{
  auto pipeline = std::make_unique<ProcessingPipeline>();
  for (const auto & slot : slots) {
    if (slot.bypassed)
      continue;
    auto processor = create_processor(slot.name);
    if (!processor)
      return Result::Error(ErrorCode::kInvalidConfig, "unknown processor '" + slot.name + "'");
    if (!pipeline->add_processor(std::move(processor), slot.config)) {
      return Result::Error(ErrorCode::kInvalidConfig, "duplicate processor '" + slot.name + "'");
    }
    if (pipeline->stats().back().health == ProcessorHealth::kFailed) {
      return Result::Error(ErrorCode::kProcessorInitFailed,
          "processor '" + slot.name +
              "' configure() failed: " + pipeline->stats().back().init_error);
    }
  }
  out = std::move(pipeline);
  return Result::Ok();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
