// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "webrtc_processors.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/interface/module_common_types.h"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

namespace
{

bool parse_choice(const ProcessorConfig & config,
    const std::string & key,
    const char * const * names,
    int count,
    int & out,
    Result & err,
    const std::string & who)
{
  auto it = config.find(key);
  if (it == config.end())
    return true;
  for (int i = 0; i < count; ++i) {
    if (it->second == names[i]) {
      out = i;
      return true;
    }
  }
  std::string allowed;
  for (int i = 0; i < count; ++i)
    allowed += std::string(i ? ", " : "") + names[i];
  err = Result::Error(ErrorCode::kInvalidConfig,
      who + ": " + key + " must be one of " + allowed + ", got '" + it->second + "'");
  return false;
}

const char * const kNsLevels[] = {"low", "moderate", "high", "very_high"};
const char * const kVadLikelihoods[] = {"very_low", "low", "moderate", "high"};

}  // namespace

Result check_webrtc_format(const ProcessingContext & c)
{
  if (c.sample_format != 16) {
    return Result::Error(ErrorCode::kUnsupportedFormat,
        "WebRTC APM needs 16-bit samples, stream is " + std::to_string(c.sample_format) + "-bit");
  }
  if (c.sample_rate != 8000 && c.sample_rate != 16000 && c.sample_rate != 32000 &&
      c.sample_rate != 48000) {
    return Result::Error(
        ErrorCode::kUnsupportedFormat, "WebRTC APM needs 8000/16000/32000/48000 Hz, stream is " +
                                           std::to_string(c.sample_rate) + " Hz");
  }
  if (c.channels < 1 || c.channels > 2) {
    return Result::Error(ErrorCode::kUnsupportedFormat,
        "WebRTC processors support 1 or 2 channels, stream has " + std::to_string(c.channels));
  }
  if (c.frame_duration_ms != 10) {
    return Result::Error(
        ErrorCode::kUnsupportedFormat, "WebRTC APM needs 10 ms blocks, pipeline block is " +
                                           std::to_string(c.frame_duration_ms) + " ms");
  }
  return Result::Ok();
}

WebRtcApmStage::WebRtcApmStage() = default;
WebRtcApmStage::~WebRtcApmStage() = default;

Result WebRtcApmStage::prepare(const ProcessingContext & context)
{
  Result r = check_webrtc_format(context);
  if (!r.ok())
    return r;

  apm_.reset(webrtc::AudioProcessing::Create());
  if (!apm_)
    return Result::Error(ErrorCode::kProcessorInitFailed, "AudioProcessing::Create() failed");
  // Everything is off by default in 0.3.1 except what we enable; set the
  // high-pass filter explicitly (it alters the audio APM hands back).
  if (apm_->high_pass_filter()->Enable(highpass_) != webrtc::AudioProcessing::kNoError) {
    apm_.reset();
    return Result::Error(
        ErrorCode::kProcessorInitFailed, name() + ": high-pass filter setup failed");
  }
  r = enable_component(*apm_);
  if (!r.ok()) {
    apm_.reset();
    return r;
  }
  const int sr = static_cast<int>(context.sample_rate);
  const int ch = static_cast<int>(context.channels);
  int err = apm_->Initialize(sr, sr, sr,
      ch == 1 ? webrtc::AudioProcessing::kMono : webrtc::AudioProcessing::kStereo,
      ch == 1 ? webrtc::AudioProcessing::kMono : webrtc::AudioProcessing::kStereo,
      ch == 1 ? webrtc::AudioProcessing::kMono : webrtc::AudioProcessing::kStereo);
  if (err != webrtc::AudioProcessing::kNoError) {
    apm_.reset();
    return Result::Error(ErrorCode::kProcessorInitFailed,
        "AudioProcessing::Initialize() error " + std::to_string(err));
  }
  apm_frame_ = std::make_unique<webrtc::AudioFrame>();
  apm_frame_->sample_rate_hz_ = sr;
  apm_frame_->num_channels_ = ch;
  apm_frame_->samples_per_channel_ = context.sample_rate / 100;
  prepared_ = context;
  return Result::Ok();
}

Result WebRtcApmStage::parse_highpass(const ProcessorConfig & config, bool default_value)
{
  highpass_ = default_value;
  auto it = config.find("highpass");
  if (it == config.end())
    return Result::Ok();
  if (it->second == "true" || it->second == "1") {
    highpass_ = true;
  } else if (it->second == "false" || it->second == "0") {
    highpass_ = false;
  } else {
    return Result::Error(ErrorCode::kInvalidConfig,
        name() + ": highpass must be true or false, got '" + it->second + "'");
  }
  return Result::Ok();
}

void WebRtcApmStage::reset()
{
  if (apm_)
    apm_->Initialize();
}

ProcessResult WebRtcApmStage::run(AudioFrame & frame, const ProcessingContext &, bool write_back)
{
  const size_t samples = static_cast<size_t>(prepared_.sample_rate / 100) * prepared_.channels;
  if (!apm_ || !frame.data || frame.frame_count != prepared_.sample_rate / 100 ||
      frame.channels != prepared_.channels || frame.data->size() != samples * 2) {
    return ProcessResult{ProcessResultStatus::kFailed,
        Result::Error(ErrorCode::kProcessorProcessFailed, name() + ": block does not match the "
                                                                   "prepared format")};
  }
  std::memcpy(apm_frame_->data_, frame.data->data(), samples * 2);
  apm_frame_->samples_per_channel_ = prepared_.sample_rate / 100;
  apm_frame_->num_channels_ = prepared_.channels;
  apm_frame_->sample_rate_hz_ = static_cast<int>(prepared_.sample_rate);
  const int err = apm_->ProcessStream(apm_frame_.get());
  if (err != webrtc::AudioProcessing::kNoError) {
    return ProcessResult{
        ProcessResultStatus::kFailed, Result::Error(ErrorCode::kProcessorProcessFailed,
                                          name() + ": ProcessStream error " + std::to_string(err))};
  }
  if (write_back)
    std::memcpy(frame.data->data(), apm_frame_->data_, samples * 2);
  return ProcessResult{};
}

Result WebRtcNsProcessor::configure(const ProcessorConfig & config)
{
  Result err = Result::Ok();
  if (!parse_choice(config, "level", kNsLevels, 4, level_, err, "ns"))
    return err;
  return parse_highpass(config, false);
}

Result WebRtcNsProcessor::enable_component(webrtc::AudioProcessing & apm)
{
  if (apm.noise_suppression()->set_level(static_cast<webrtc::NoiseSuppression::Level>(level_)) !=
          webrtc::AudioProcessing::kNoError ||
      apm.noise_suppression()->Enable(true) != webrtc::AudioProcessing::kNoError) {
    return Result::Error(ErrorCode::kProcessorInitFailed, "ns: enabling NoiseSuppression failed");
  }
  return Result::Ok();
}

ProcessResult WebRtcNsProcessor::process(AudioFrame & frame, const ProcessingContext & context)
{
  return run(frame, context, true);
}

Result WebRtcVadProcessor::configure(const ProcessorConfig & config)
{
  Result err = Result::Ok();
  if (!parse_choice(config, "likelihood", kVadLikelihoods, 4, likelihood_, err, "vad"))
    return err;
  auto it = config.find("hangover_ms");
  if (it != config.end()) {
    char * end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (it->second.empty() || *end != '\0' || v < 0 || v > 2000) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "vad: hangover_ms must be an integer in [0, 2000], got '" + it->second + "'");
    }
    hangover_.set_hangover_ms(static_cast<uint32_t>(v));
  }
  return parse_highpass(config, false);
}

Result WebRtcVadProcessor::enable_component(webrtc::AudioProcessing & apm)
{
  auto * vd = apm.voice_detection();
  if (vd->set_likelihood(static_cast<webrtc::VoiceDetection::Likelihood>(likelihood_)) !=
          webrtc::AudioProcessing::kNoError ||
      vd->set_frame_size_ms(10) != webrtc::AudioProcessing::kNoError ||
      vd->Enable(true) != webrtc::AudioProcessing::kNoError) {
    return Result::Error(ErrorCode::kProcessorInitFailed, "vad: enabling VoiceDetection failed");
  }
  return Result::Ok();
}

ProcessResult WebRtcVadProcessor::process(AudioFrame & frame, const ProcessingContext & context)
{
  // Analysis only: the APM copy is processed, `frame` audio is never written.
  ProcessResult r = run(frame, context, false);
  if (r.status != ProcessResultStatus::kProcessed)
    return r;  // frame.voice stays kUnknown; PCM is unaffected
  const bool raw = apm_->voice_detection()->stream_has_voice();
  frame.voice = hangover_.update(raw, 10) ? VoiceActivity::kActive : VoiceActivity::kInactive;
  return r;
}

void WebRtcVadProcessor::reset()
{
  WebRtcApmStage::reset();
  hangover_.reset();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
