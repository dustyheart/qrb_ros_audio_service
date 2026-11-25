// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__AUDIO_COMMON_WRAPPER_HPP_
#define QRB_AUDIO_COMMON_LIB__AUDIO_COMMON_WRAPPER_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qrb
{
namespace audio_common_lib
{

enum class StreamType
{
  Invalid,
  Playback,
  Capture,
};

enum class StreamEvent
{
  StreamStart,
  StreamStoped,
  StreamAbort,
  StreamTimestamp,
  StreamData,
  StreamEos,
};

struct StreamEventData
{
  std::shared_ptr<std::vector<uint8_t>> data_buf;
  uint64_t usec = 0;
};

struct AudioStreamInfo
{
  uint8_t format = 16;
  uint32_t rate = 16000;
  uint8_t channels = 2;
  StreamType type = StreamType::Invalid;
  std::string file_path = "";
  uint8_t volume = 0;
  std::string device = "";
  bool need_timestamp = false;
  bool pcm_mode = false;
  int32_t repeat = 0;
  // Capture fragment size, in ms. 0 means "use backend default".
  uint32_t fragment_duration_ms = 0;
  // Playback pre-buffer size, in ms. 0 means "use backend default".
  uint32_t buffer_duration_ms = 0;
};

using stream_event_callback_func = std::function<void(StreamEvent, StreamEventData, void *)>;

enum class AudioBackend
{
  INVALID = -1,
  ALSA = 0,
  PULSEAUDIO,
};

AudioBackend detect_audio_backend();

uint32_t audio_stream_open(const AudioStreamInfo & stream_info,
    stream_event_callback_func event_callback);
int audio_stream_start(uint32_t stream_handle);
int audio_stream_mute(uint32_t stream_handle, bool mute);
int audio_stream_stop(uint32_t stream_handle);
int audio_stream_close(uint32_t stream_handle);
int audio_stream_write(uint32_t stream_handle, const void * buf, size_t length);

// Enumerate capture sources (PulseAudio/pipewire-pulse source names, including
// ".monitor" sources) as accepted by AudioStreamInfo::device. Returns 0 on
// success, -ENOTSUP when the active backend can't enumerate (ALSA), -EIO when
// the audio server can't be queried.
//
// Needed because pipewire-pulse does NOT reject an unknown source name in
// pa_stream_connect_record(): the stream still reaches PA_STREAM_READY, it is
// simply never linked to a device, so capture "succeeds" and delivers no data
// (verified on real hardware). Existence has to be checked up front.
int audio_list_capture_devices(std::vector<std::string> & names);

}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__AUDIO_COMMON_WRAPPER_HPP_
