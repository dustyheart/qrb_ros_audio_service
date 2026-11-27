// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Internal header (not installed): format guard and config parsing shared by
// both WebRTC processor implementations (webrtc_processors.cpp for the
// system webrtc-audio-processing 0.3.1, webrtc1_processors.cpp for 1.x).
// Exactly one of the two is compiled into the library.

#ifndef QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_COMMON_HPP_
#define QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_COMMON_HPP_

#include <cstdlib>
#include <string>

#include "qrb_audio_common_lib/core/audio_processor.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{
namespace webrtc_common
{

// int16 interleaved, 8/16/32/48 kHz, exactly 10 ms per call, 1 or 2 channels
// (what these wrappers support and were tested with).
inline Result check_format(const ProcessingContext & c)
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

// `key` absent: `out` untouched, true. Present: must be one of names[0..count).
inline bool parse_choice(const ProcessorConfig & config,
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

inline bool parse_bool(const ProcessorConfig & config,
    const std::string & key,
    bool & out,
    Result & err,
    const std::string & who)
{
  auto it = config.find(key);
  if (it == config.end())
    return true;
  if (it->second == "true" || it->second == "1") {
    out = true;
  } else if (it->second == "false" || it->second == "0") {
    out = false;
  } else {
    err = Result::Error(ErrorCode::kInvalidConfig,
        who + ": " + key + " must be true or false, got '" + it->second + "'");
    return false;
  }
  return true;
}

inline bool parse_uint(const ProcessorConfig & config,
    const std::string & key,
    long lo,
    long hi,
    uint32_t & out,
    Result & err,
    const std::string & who)
{
  auto it = config.find(key);
  if (it == config.end())
    return true;
  char * end = nullptr;
  long v = std::strtol(it->second.c_str(), &end, 10);
  if (it->second.empty() || *end != '\0' || v < lo || v > hi) {
    err = Result::Error(ErrorCode::kInvalidConfig,
        who + ": " + key + " must be an integer in [" + std::to_string(lo) + ", " +
            std::to_string(hi) + "], got '" + it->second + "'");
    return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Names shared by both implementations (index = webrtc enum value / mode).
constexpr const char * kNsLevels[] = {"low", "moderate", "high", "very_high"};
// 0.3.1 VoiceDetection::Likelihood order; WebRTC VAD mode = 3 - index
// (0.3.1 voice_detection_impl.cc: kVeryLow -> 3 ... kHigh -> 0).
constexpr const char * kVadLikelihoods[] = {"very_low", "low", "moderate", "high"};

}  // namespace webrtc_common
}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_COMMON_HPP_
