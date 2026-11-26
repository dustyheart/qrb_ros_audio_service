// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "qrb_audio_common_lib/core/audio_capture_backend.hpp"
#include "qrb_audio_common_lib/core/capture_timestamp.hpp"

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
// AudioManager routes its streaming-capture path (record without a
// destination file) through this class since 7896f31.
//
// Timestamps (config.need_timestamp): AudioFrame::timestamp_usec is the
// CLOCK_MONOTONIC capture time of the frame's first sample, produced by
// CaptureTimestamper from the chunk arrival time and the server-reported
// record latency. 0 when need_timestamp is false.
class PipeWirePulseBackend : public IAudioCaptureBackend
{
public:
  ~PipeWirePulseBackend() override;

  Result configure(const AudioStreamConfig & config) override;
  Result start(FrameCallback callback) override;
  Result stop() override;
  AudioBackendStatus status() const override { return status_; }

  // kOk if `device` is empty (= server default) or names an existing capture
  // source; kDeviceUnavailable (message lists the available sources) if not;
  // kBackendInitFailed if the server couldn't be queried. kOk without
  // checking if the process-wide backend is ALSA (it can't enumerate, and
  // rejects bad names itself at open time) - lets AudioManager's legacy
  // record-to-file path share this check regardless of backend.
  static Result check_capture_device(const std::string & device);

private:
  AudioStreamConfig config_;
  FrameCallback callback_;
  uint32_t stream_handle_ = 0;
  std::atomic<AudioBackendStatus> status_{AudioBackendStatus::kUnconnected};
  uint64_t sequence_ = 0;
  // Only touched on the PulseAudio mainloop thread (data callback) after
  // start(); recreated by start() so each run gets a fresh timeline.
  std::unique_ptr<CaptureTimestamper> timestamper_;
  bool ts_debug_ = false;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PIPEWIRE_PULSE_BACKEND_HPP_
