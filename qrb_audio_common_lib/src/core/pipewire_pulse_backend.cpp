// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qrb_audio_common_lib/core/pipewire_pulse_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/audio_common_wrapper.hpp"
#include "qrb_audio_common_lib/audio_log.hpp"
#include "qrb_audio_common_lib/core/config_validation.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

namespace
{
// AudioStreamInfo::volume must be in (STREAM_VOL_MIN, STREAM_VOL_MAX], see
// PulseCommonStream::audio_stream_open - a value of 0 makes stream creation
// fail outright. AudioStreamConfig has no volume concept (capture doesn't
// need one), so always pass a safe non-zero default here.
constexpr uint8_t kCaptureVolume = 50;
}  // namespace

Result PipeWirePulseBackend::check_capture_device(const std::string & device)
{
  if (device.empty())
    return Result::Ok();

  std::vector<std::string> sources;
  int ret = audio_list_capture_devices(sources);
  if (ret == -ENOTSUP) {
    // Active backend can't enumerate (ALSA); it validates the device name
    // itself at open time, so there is nothing to pre-check here.
    return Result::Ok();
  }
  if (ret != 0) {
    return Result::Error(ErrorCode::kBackendInitFailed,
        "cannot enumerate capture devices (error " + std::to_string(ret) + ")");
  }
  if (std::find(sources.begin(), sources.end(), device) != sources.end())
    return Result::Ok();

  std::string available;
  for (const auto & name : sources) {
    if (!available.empty())
      available += ", ";
    available += name;
  }
  return Result::Error(ErrorCode::kDeviceUnavailable,
      "capture device '" + device + "' not found; available: " +
          (available.empty() ? "<none>" : available));
}

PipeWirePulseBackend::~PipeWirePulseBackend()
{
  stop();
}

Result PipeWirePulseBackend::configure(const AudioStreamConfig & config)
{
  if (status_ != AudioBackendStatus::kUnconnected && status_ != AudioBackendStatus::kStopped) {
    return Result::Error(
        ErrorCode::kStreamStateConflict, "configure() called while backend is active");
  }
  Result valid = validate_capture_config(config);
  if (!valid.ok())
    return valid;

  // audio_stream_open() dispatches on a process-wide static (g_backend) that
  // defaults to ALSA until something calls detect_audio_backend(). This class
  // promises to be the PipeWire-Pulse backend specifically, so it cannot rely
  // on some other caller (today, AudioManager::get_instance()) having already
  // run that detection - do it here, unconditionally, and fail loudly if the
  // detected backend isn't PulseAudio rather than silently falling through to
  // whatever audio_stream_open() picks.
  if (detect_audio_backend() != AudioBackend::PULSEAUDIO) {
    status_ = AudioBackendStatus::kFailed;
    return Result::Error(
        ErrorCode::kBackendInitFailed, "PulseAudio/pipewire-pulse backend not available");
  }

  Result device_ok = check_capture_device(config.device);
  if (!device_ok.ok()) {
    status_ = AudioBackendStatus::kFailed;
    return device_ok;
  }

  AudioStreamInfo info;
  info.type = StreamType::Capture;
  info.rate = config.sample_rate;
  info.channels = config.channels;
  info.format = config.sample_format;
  info.device = config.device;
  info.fragment_duration_ms = config.fragment_duration_ms;
  info.need_timestamp = config.need_timestamp;
  LOGI("configure: device='%s' rate=%u channels=%u format=%u fragment_ms=%u need_timestamp=%d",
      config.device.c_str(), config.sample_rate, config.channels, config.sample_format,
      config.fragment_duration_ms, config.need_timestamp ? 1 : 0);
  info.pcm_mode = true;
  info.volume = kCaptureVolume;

  config_ = config;

  auto event_cb = [this](StreamEvent event, StreamEventData data, void *) {
    switch (event) {
      case StreamEvent::StreamData: {
        if (!callback_ || status_ != AudioBackendStatus::kReady)
          return;
        AudioFrame frame;
        frame.data = data.data_buf;
        frame.sample_rate = config_.sample_rate;
        frame.channels = config_.channels;
        frame.sample_format = config_.sample_format;
        uint32_t bytes_per_sample = static_cast<uint32_t>(config_.sample_format / 8);
        uint32_t frame_bytes = config_.channels * bytes_per_sample;
        frame.frame_count = frame_bytes ? static_cast<uint32_t>(frame.byte_size() / frame_bytes) : 0;
        frame.sequence = sequence_++;
        frame.read_duration_usec = data.usec;  // a duration; see AudioFrame
        if (timestamper_) {
          auto ts = timestamper_->on_chunk(frame.frame_count, data.arrival_mono_usec, data.latency_usec);
          frame.timestamp_usec = ts.timestamp_usec;
          if (ts_debug_) {
            LOGI("tsdbg seq=%llu frames=%u arrival=%llu latency=%lld ts=%llu resync=%d",
                static_cast<unsigned long long>(frame.sequence), frame.frame_count,
                static_cast<unsigned long long>(data.arrival_mono_usec),
                static_cast<long long>(data.latency_usec),
                static_cast<unsigned long long>(ts.timestamp_usec), ts.resynced ? 1 : 0);
          }
          if (ts.gap_detected)
            frame.flags |= static_cast<uint32_t>(FrameFlag::kDropped);
        }
        callback_(frame);
        return;
      }
      case StreamEvent::StreamAbort:
        status_ = AudioBackendStatus::kFailed;
        return;
      case StreamEvent::StreamStoped:
        if (status_ != AudioBackendStatus::kFailed)
          status_ = AudioBackendStatus::kStopped;
        return;
      default:
        return;
    }
  };

  stream_handle_ = audio_stream_open(info, event_cb);
  if (stream_handle_ == 0) {
    status_ = AudioBackendStatus::kFailed;
    return Result::Error(ErrorCode::kBackendInitFailed, "audio_stream_open failed");
  }

  status_ = AudioBackendStatus::kStopped;
  return Result::Ok();
}

Result PipeWirePulseBackend::start(FrameCallback callback)
{
  if (stream_handle_ == 0) {
    return Result::Error(ErrorCode::kStreamStateConflict, "configure() was not called");
  }
  if (status_ == AudioBackendStatus::kReady) {
    return Result::Error(ErrorCode::kStreamStateConflict, "already started");
  }

  callback_ = std::move(callback);
  sequence_ = 0;
  // Set before audio_stream_start(): the data callback may fire as soon as
  // the stream connects.
  ts_debug_ = getenv("QRB_AUDIO_TS_DEBUG") != nullptr;  // per-chunk diagnostics, off by default
  timestamper_ = config_.need_timestamp
                     ? std::make_unique<CaptureTimestamper>(config_.sample_rate)
                     : nullptr;

  int ret = audio_stream_start(stream_handle_);
  if (ret != 0) {
    status_ = AudioBackendStatus::kFailed;
    return Result::Error(ErrorCode::kBackendInitFailed, "audio_stream_start failed");
  }

  status_ = AudioBackendStatus::kReady;
  return Result::Ok();
}

Result PipeWirePulseBackend::stop()
{
  if (stream_handle_ == 0) {
    return Result::Ok();  // nothing to stop, matches destructor calling stop() unconditionally
  }

  if (status_ == AudioBackendStatus::kReady) {
    audio_stream_stop(stream_handle_);
  }
  if (timestamper_) {
    LOGI("capture timestamps: %llu discontinuity resync(s); measured audio clock / "
         "CLOCK_MONOTONIC ratio %.5f",
        static_cast<unsigned long long>(timestamper_->resync_count()),
        timestamper_->clock_ratio() > 0 ? 1.0 / timestamper_->clock_ratio() : 0.0);
  }
  audio_stream_close(stream_handle_);
  stream_handle_ = 0;
  callback_ = nullptr;
  status_ = AudioBackendStatus::kStopped;
  return Result::Ok();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
