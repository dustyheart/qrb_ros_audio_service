// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_MANAGER__AUDIO_MANAGER_HPP_
#define QRB_AUDIO_MANAGER__AUDIO_MANAGER_HPP_

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "qrb_audio_common_lib/core/audio_stream_config.hpp"
#include "qrb_audio_manager/message_queue.hpp"
#include "qrb_audio_manager/stream.hpp"

namespace qrb
{
namespace audio_manager
{

// The interface provided by Audio Manager
class IAudioManager
{
public:
  virtual uint32_t create_playback_stream(std::string source,
      uint32_t sample_rate,
      uint8_t channels,
      uint8_t sample_format,
      std::string coding_format,
      uint8_t volume,
      std::string play_mode,
      int8_t repeat,
      std::string subs_name) = 0;
  virtual uint32_t create_record_stream(uint32_t sample_rate,
      uint8_t channels,
      uint8_t sample_format,
      std::string coding_format,
      std::string source,
      bool pub_pcm,
      std::string pub_name,
      bool need_timestamp = false) = 0;
  virtual bool start_stream(uint32_t stream_handle) = 0;
  virtual bool stop_stream(uint32_t stream_handle) = 0;
  virtual bool release_stream(uint32_t stream_handle) = 0;
  virtual bool mute_stream(uint32_t stream_handle, bool mute) = 0;
  virtual bool write_stream(uint32_t stream_handle, const void * data, size_t size) = 0;
  virtual const std::map<std::string, std::string> & get_buildin_sounds() = 0;
  virtual void clean() = 0;
};

enum class AudioManagerCommand
{
  PLAY = 0,
  CREATE,
  START,
  STOP,
  MUTE,
  RELEASE,
  GETBUILDINSOUND
};

enum class AudioManagerPlayMode
{
  NORMAL = 1,
  ONE_TOUCH
};

extern std::map<std::string, AudioManagerCommand> audio_manager_cmd_name;

extern std::map<std::string, AudioManagerPlayMode> audio_manager_play_mode;

// Per-chunk metadata handed to the stream data callback alongside the PCM.
struct StreamDataInfo
{
  // How long the backend took to read this chunk (us). A duration used only
  // by AudioServer's enable_latency_log; this is what the old uint64_t
  // callback argument carried.
  uint64_t read_duration_usec = 0;
  // CLOCK_MONOTONIC capture time of the chunk's first sample (us), 0 if not
  // available (need_timestamp=false, or the record-to-file legacy path which
  // does not support timestamps).
  uint64_t capture_time_mono_usec = 0;
  // VAD decision for this chunk when the capture chain has a "vad" stage:
  // -1 unknown (no VAD, or VAD bypassed/failed for it), 0 no voice, 1 voice.
  int8_t voice_activity = -1;
};

using stream_data_cb_t =
    std::function<void(uint32_t, std::shared_ptr<std::vector<uint8_t>>, const StreamDataInfo &)>;

// Backend (PipeWire/PulseAudio) level defaults applied to every stream created
// through this process. These are node-level ROS parameters rather than
// per-request fields, since AudioRequest does not carry a device/buffer config.
struct AudioBackendDefaults
{
  std::string capture_device;
  std::string playback_device;
  uint32_t capture_fragment_duration_ms = 0;
  uint32_t playback_buffer_duration_ms = 0;
  // Processing chain for streaming capture (pub_pcm) streams, in order.
  // Empty = no processing. Record-to-file and playback ignore it.
  std::vector<qrb::audio_common_lib::core::ProcessorSlot> capture_processors;
};

class AudioManager : public IAudioManager
{
public:
  static AudioManager * get_instance();
  uint32_t create_playback_stream(std::string source,
      uint32_t sample_rate,
      uint8_t channels,
      uint8_t sample_format,
      std::string coding_format,
      uint8_t volume,
      std::string play_mode,
      int8_t repeat,
      std::string subs_name) override;
  uint32_t create_record_stream(uint32_t sample_rate,
      uint8_t channels,
      uint8_t sample_format,
      std::string coding_format,
      std::string source,
      bool pub_pcm,
      std::string pub_name,
      bool need_timestamp = false) override;
  bool start_stream(uint32_t stream_handle) override;
  bool stop_stream(uint32_t stream_handle) override;
  bool release_stream(uint32_t stream_handle) override;
  bool mute_stream(uint32_t stream_handle, bool mute) override;
  bool write_stream(uint32_t stream_handle, const void * data, size_t size) override;
  const std::map<std::string, std::string> & get_buildin_sounds() override;
  static void set_stream_data_callback(stream_data_cb_t cb);
  static void set_backend_defaults(const AudioBackendDefaults & defaults);
  // By value: set_backend_defaults() runs on the parameter callback thread
  // while streams are being created on (reentrant) service threads.
  static AudioBackendDefaults get_backend_defaults();
  static void on_task_completed(int domain,
      uint32_t stream_handle,
      uint32_t audio_domain_handle,
      std::string cmd,
      bool result);
  void clean() { streams_.clear(); };

private:
  AudioManager() : worker_([this] { this->worker_thread(std::ref(queue_)); }) {}
  ~AudioManager()
  {
    queue_.stop();
    if (worker_.joinable())
      worker_.join();
  }
  static AudioManager * instance_;
  static stream_data_cb_t stream_data_cb_;
  static AudioBackendDefaults backend_defaults_;
  std::unordered_map<uint32_t, std::shared_ptr<Stream>> streams_{};
  std::map<std::string, std::string> buildin_sounds_{};
  MessageQueue queue_;
  std::thread worker_;
  std::shared_ptr<Stream> find_stream(uint32_t stream_handle);
  uint32_t generate_key();
  void worker_thread(MessageQueue & queue);
  void load_buildin_sounds(const std::string & path);
};

}  // namespace audio_manager
}  // namespace qrb

#endif  // QRB_AUDIO_MANAGER__AUDIO_MANAGER_HPP_
