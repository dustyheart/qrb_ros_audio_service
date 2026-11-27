// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qrb_audio_common_lib/core/processor_factory.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "qrb_audio_common_lib/core/passthrough_processor.hpp"

#if defined(QRB_AUDIO_HAVE_WEBRTC_APM1)
#include "webrtc1_processors.hpp"
#elif defined(QRB_AUDIO_HAVE_WEBRTC_APM)
#include "webrtc_processors.hpp"
#endif

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

bool webrtc_processors_available()
{
#if defined(QRB_AUDIO_HAVE_WEBRTC_APM) || defined(QRB_AUDIO_HAVE_WEBRTC_APM1)
  return true;
#else
  return false;
#endif
}

bool webrtc_aec_available()
{
#ifdef QRB_AUDIO_HAVE_WEBRTC_APM1
  return true;
#else
  return false;
#endif
}

std::string webrtc_implementation()
{
#if defined(QRB_AUDIO_HAVE_WEBRTC_APM1)
  return "webrtc-audio-processing 1.x";
#elif defined(QRB_AUDIO_HAVE_WEBRTC_APM)
  return "webrtc-audio-processing 0.3.1";
#else
  return "none";
#endif
}

std::vector<std::string> known_processor_names()
{
  std::vector<std::string> names{"passthrough", "test_delay"};
  if (webrtc_aec_available())
    names.push_back("aec");
  if (webrtc_processors_available()) {
    names.push_back("ns");
    names.push_back("vad");
  }
  return names;
}

std::unique_ptr<IAudioProcessor> create_processor(const std::string & name)
{
  if (name == "passthrough")
    return std::make_unique<PassthroughProcessor>();
  if (name == "test_delay")
    return std::make_unique<TestDelayProcessor>();
#if defined(QRB_AUDIO_HAVE_WEBRTC_APM1)
  if (name == "aec")
    return std::make_unique<WebRtc1ApmProcessor>(true, false);
  if (name == "ns")
    return std::make_unique<WebRtc1ApmProcessor>(false, true);
  if (name == "vad")
    return std::make_unique<WebRtc1VadProcessor>();
#elif defined(QRB_AUDIO_HAVE_WEBRTC_APM)
  if (name == "ns")
    return std::make_unique<WebRtcNsProcessor>();
  if (name == "vad")
    return std::make_unique<WebRtcVadProcessor>();
#endif
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
    if ((out.name == "ns" || out.name == "vad") && !webrtc_processors_available()) {
      return Result::Error(ErrorCode::kInvalidConfig, "processor '" + out.name +
                                                          "' is not available: "
                                                          "qrb_audio_common_lib was built without "
                                                          "webrtc-audio-processing (install "
                                                          "webrtc-audio-processing 1.x, or "
                                                          "libwebrtc-audio-processing-dev, and "
                                                          "rebuild)");
    }
    if (out.name == "aec" && !webrtc_aec_available()) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "processor 'aec' is not available: it needs webrtc-audio-processing 1.x (AEC3); "
          "qrb_audio_common_lib was built with " +
              webrtc_implementation());
    }
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
  std::vector<const ProcessorSlot *> active;
  for (const auto & slot : slots) {
    if (!slot.bypassed)
      active.push_back(&slot);
  }
  // Echo must be removed before anything that analyses or reshapes the
  // signal: "aec" has to precede "ns" and "vad".
  bool seen_ns_or_vad = false;
  for (const auto * slot : active) {
    if (slot->name == "ns" || slot->name == "vad") {
      seen_ns_or_vad = true;
    } else if (slot->name == "aec" && seen_ns_or_vad) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "processor 'aec' must come before 'ns' and 'vad' in the chain (order: aec, ns, vad)");
    }
  }

  auto pipeline = std::make_unique<ProcessingPipeline>();
  for (size_t i = 0; i < active.size(); ++i) {
    const ProcessorSlot & slot = *active[i];
    std::unique_ptr<IAudioProcessor> processor;
    ProcessorConfig config = slot.config;
    std::string label = "'" + slot.name + "'";
#ifdef QRB_AUDIO_HAVE_WEBRTC_APM1
    // "aec" directly followed by "ns": one APM runs both (AEC3 then NS), so
    // the NS sees the echo-cancelled signal and the block is converted once.
    if (slot.name == "aec" && i + 1 < active.size() && active[i + 1]->name == "ns") {
      for (const auto & kv : active[i + 1]->config) {
        if (kv.first == "highpass") {
          // AEC3 enforces the high-pass filter anyway; keep the most
          // permissive value so validation still sees a bad one.
          if (!config.count("highpass") || (kv.second != "false" && kv.second != "0"))
            config["highpass"] = kv.second;
        } else if (config.count(kv.first) && config[kv.first] != kv.second) {
          return Result::Error(ErrorCode::kInvalidConfig,
              "processors 'aec' and 'ns' share one APM but set '" + kv.first + "' differently");
        } else {
          config[kv.first] = kv.second;
        }
      }
      processor = std::make_unique<WebRtc1ApmProcessor>(true, true);
      label = "'aec'+'ns'";
      ++i;
    }
#endif
    if (!processor)
      processor = create_processor(slot.name);
    if (!processor)
      return Result::Error(ErrorCode::kInvalidConfig, "unknown processor '" + slot.name + "'");
    if (!pipeline->add_processor(std::move(processor), config))
      return Result::Error(ErrorCode::kInvalidConfig, "duplicate processor " + label);
    if (pipeline->stats().back().health == ProcessorHealth::kFailed) {
      return Result::Error(ErrorCode::kProcessorInitFailed,
          "processor " + label + " configure() failed: " + pipeline->stats().back().init_error);
    }
  }
  out = std::move(pipeline);
  return Result::Ok();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
