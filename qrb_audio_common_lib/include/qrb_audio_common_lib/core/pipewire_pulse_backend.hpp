// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_

#include <atomic>
#include <cstdint>

#include "qrb_audio_common_lib/core/audio_capture_backend.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Wraps the existing qrb::audio_common_lib C API (audio_stream_open/start/
// stop/close, the same functions AudioManager has always used and that the
// backend-parameterization phase already verified end-to-end on real
// hardware). Named per refactoring_plan.md section 3.3 ("保留当前 PulseAudio
// API 实现，并命名为 PipeWirePulseBackend"): the actual server on these
// boards is pipewire-pulse (PipeWire's Pulse-protocol compatibility layer),
// not a native PulseAudio daemon - see audio_requirements.md's investigation.
//
// This class is additive: AudioManager does not use it yet. It exists so the
// IAudioCaptureBackend abstraction can be verified against real hardware
// before AudioManager is switched over to it (kept as a separate change to
// avoid regressing the capture path qrb_ros_audio_service already depends
// on).
class PipeWirePulseBackend : public IAudioCaptureBackend
{
public:
  ~PipeWirePulseBackend() override;

  Result configure(const AudioStreamConfig & config) override;
  Result start(FrameCallback callback) override;
  Result stop() override;
  AudioBackendStatus status() const override { return status_; }

private:
  AudioStreamConfig config_;
  FrameCallback callback_;
  uint32_t stream_handle_ = 0;
  std::atomic<AudioBackendStatus> status_{ AudioBackendStatus::kUnconnected };
  uint64_t sequence_ = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_
