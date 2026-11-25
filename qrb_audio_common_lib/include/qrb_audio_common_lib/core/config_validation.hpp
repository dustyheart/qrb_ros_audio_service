// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__CONFIG_VALIDATION_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__CONFIG_VALIDATION_HPP_

#include <string>

#include "qrb_audio_common_lib/core/audio_stream_config.hpp"
#include "qrb_audio_common_lib/core/result.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Limits shared by every capture backend. Kept free of PulseAudio headers so
// this stays usable (and unit-testable) without any audio server.
constexpr uint32_t kMaxSampleRate = 384000;     // PA_RATE_MAX on older libpulse; conservative
constexpr uint8_t kMaxChannels = 32;            // PA_CHANNELS_MAX
constexpr uint32_t kMaxDurationMs = 2000;       // matches the ROS param integer_range

inline bool is_supported_sample_format(uint8_t bits)
{
  return bits == 8 || bits == 16 || bits == 24 || bits == 32;
}

// Pure, side-effect-free validation of the format/buffer part of a capture
// config. Device existence is a separate, backend-specific check (it needs a
// live audio server), see PipeWirePulseBackend::check_capture_device().
//
// Why this exists: before it, the legacy PulseAudio path silently mapped any
// sample_format it didn't recognise (e.g. 12) to S16 and reported success, and
// PipeWirePulseBackend then computed AudioFrame::frame_count with the
// requested (wrong) bytes-per-sample - verified on real hardware, see
// test_report.md section 5g.
inline Result validate_capture_config(const AudioStreamConfig & config)
{
  if (!is_supported_sample_format(config.sample_format)) {
    return Result::Error(ErrorCode::kUnsupportedFormat,
        "sample_format " + std::to_string(config.sample_format) +
            " not supported (expected 8, 16, 24 or 32)");
  }
  if (config.channels == 0 || config.channels > kMaxChannels) {
    return Result::Error(ErrorCode::kInvalidConfig,
        "channels " + std::to_string(config.channels) + " out of range [1, " +
            std::to_string(kMaxChannels) + "]");
  }
  if (config.sample_rate == 0 || config.sample_rate > kMaxSampleRate) {
    return Result::Error(ErrorCode::kInvalidConfig,
        "sample_rate " + std::to_string(config.sample_rate) + " out of range [1, " +
            std::to_string(kMaxSampleRate) + "]");
  }
  if (config.fragment_duration_ms > kMaxDurationMs) {
    return Result::Error(ErrorCode::kInvalidConfig,
        "fragment_duration_ms " + std::to_string(config.fragment_duration_ms) +
            " out of range [0, " + std::to_string(kMaxDurationMs) + "]");
  }
  if (config.frame_duration_ms == 0 || config.frame_duration_ms > kMaxDurationMs) {
    return Result::Error(ErrorCode::kInvalidConfig,
        "frame_duration_ms " + std::to_string(config.frame_duration_ms) + " out of range [1, " +
            std::to_string(kMaxDurationMs) + "]");
  }
  return Result::Ok();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__CONFIG_VALIDATION_HPP_
