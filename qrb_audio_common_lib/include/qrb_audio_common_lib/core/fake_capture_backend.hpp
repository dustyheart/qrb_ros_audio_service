// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__FAKE_CAPTURE_BACKEND_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__FAKE_CAPTURE_BACKEND_HPP_

#include <cstdint>

#include "qrb_audio_common_lib/core/audio_capture_backend.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Synthesizes silent frames on demand (via emit_frame(), called by a test, not
// a real timer/thread) instead of talking to any real audio backend. Exists
// so pipeline/integration tests can exercise the full
// configure->start->frame->stop lifecycle without hardware or PulseAudio,
// per refactoring_plan.md stage 1 exit condition.
class FakeCaptureBackend : public IAudioCaptureBackend
{
public:
  Result configure(const AudioStreamConfig & config) override
  {
    if (config.sample_rate == 0 || config.channels == 0) {
      return Result::Error(ErrorCode::kInvalidConfig, "sample_rate/channels must be non-zero");
    }
    config_ = config;
    status_ = AudioBackendStatus::kStopped;
    return Result::Ok();
  }

  Result start(FrameCallback callback) override
  {
    if (status_ == AudioBackendStatus::kUnconnected) {
      return Result::Error(ErrorCode::kStreamStateConflict, "configure() was not called");
    }
    callback_ = std::move(callback);
    sequence_ = 0;
    status_ = AudioBackendStatus::kReady;
    return Result::Ok();
  }

  Result stop() override
  {
    status_ = AudioBackendStatus::kStopped;
    callback_ = nullptr;
    return Result::Ok();
  }

  AudioBackendStatus status() const override { return status_; }

  // Test-only driver: synthesizes one frame of `frame_duration_ms` worth of
  // silence and feeds it to whatever callback start() was given. No-op if
  // not currently kReady, mirroring how a real backend would drop/ignore
  // data-ready events after stop().
  void emit_frame()
  {
    if (status_ != AudioBackendStatus::kReady || !callback_)
      return;

    uint32_t samples = config_.sample_rate * config_.frame_duration_ms / 1000;
    uint32_t bytes_per_sample = config_.sample_format / 8;

    AudioFrame frame;
    frame.data =
        std::make_shared<std::vector<uint8_t>>(samples * config_.channels * bytes_per_sample, 0);
    frame.sample_rate = config_.sample_rate;
    frame.channels = config_.channels;
    frame.sample_format = config_.sample_format;
    frame.frame_count = samples;
    frame.sequence = sequence_++;

    callback_(frame);
  }

private:
  AudioStreamConfig config_;
  AudioBackendStatus status_ = AudioBackendStatus::kUnconnected;
  FrameCallback callback_;
  uint64_t sequence_ = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__FAKE_CAPTURE_BACKEND_HPP_
