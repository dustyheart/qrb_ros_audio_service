// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#define LOG_TAG "AudioManager"

// clang-format off
#include <dirent.h>
#include <sys/types.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

#include "qrb_audio_common_lib/audio_common_wrapper.hpp"
#include "qrb_audio_common_lib/core/config_validation.hpp"
#include "qrb_audio_common_lib/core/pipewire_pulse_backend.hpp"
#include "qrb_audio_common_lib/core/processing_capture_backend.hpp"
#include "qrb_audio_common_lib/core/processor_factory.hpp"
#include "qrb_audio_manager/audio_manager.hpp"
#include "qrb_audio_manager/stream.hpp"
// clang-format on

#define BUILDIN_SOUNDS_PATH "/opt/qcom/qirp-sdk/usr/share/qrb-audio-manager/sounds"

uint32_t am_log_lvl = (AM_ERROR | AM_INFO | AM_DEBUG);

namespace qrb
{
namespace audio_manager
{

// clang-format off
std::map<std::string, AudioManagerCommand> audio_manager_cmd_name {
  { "play", AudioManagerCommand::PLAY },
  { "create", AudioManagerCommand::CREATE },
  { "start", AudioManagerCommand::START },
  { "stop", AudioManagerCommand::STOP },
  { "release", AudioManagerCommand::RELEASE },
  { "mute", AudioManagerCommand::MUTE },
  { "get-buildin-sound", AudioManagerCommand::GETBUILDINSOUND }
};

std::map<std::string, AudioManagerPlayMode> audio_manager_play_mode {
  { "normal", AudioManagerPlayMode::NORMAL },
  { "one-touch", AudioManagerPlayMode::ONE_TOUCH }
};
// clang-format on

AudioManager * AudioManager::instance_ = nullptr;
stream_data_cb_t AudioManager::stream_data_cb_ = nullptr;
AudioBackendDefaults AudioManager::backend_defaults_{};
static std::mutex g_backend_defaults_mutex;

static stream_data_cb_t g_stream_data_cb = nullptr;

// Record/capture streams that stream PCM (pub_pcm / a topic subscriber, i.e.
// no destination file) are routed through core::IAudioCaptureBackend
// (PipeWirePulseBackend in production) instead of the legacy
// audio_stream_open/start/stop/close C API. Record-to-file (configs->source
// non-empty, using libsndfile inside CaptureStream) and all of playback keep
// using the legacy API unchanged - AudioStreamConfig/IAudioCaptureBackend
// has no concept of a destination file, and silently dropping that
// capability would be a real behavior regression (source is a real,
// documented AudioRequest field; see audio_service.cpp's "either source or
// topic_name" check), not just an unlikely corner case.
//
// START/STOP/CLOSE/MUTE dispatch on registry membership of ac_handle, not on
// configs->type, so a mixed-mode record stream (source set -> legacy path)
// is routed correctly even though configs->type is RECORD for it too; the
// two handle spaces are independently randomly generated (same scheme
// AudioManager::generate_key() already uses for am_handle), so collision risk
// is the same order as the rest of this codebase already accepts.
static std::mutex g_capture_backends_mutex;
// shared_ptr so START can keep the backend alive after dropping the registry
// lock (a concurrent CLOSE on another reentrant service thread may erase it).
static std::unordered_map<uint32_t,
    std::shared_ptr<qrb::audio_common_lib::core::IAudioCaptureBackend>>
    g_capture_backends;

// Why the last OPEN failed, for create_record_stream()'s exception text.
// Stream::open() -> domain callback only carries a bool back, and the
// callback runs synchronously on the calling (ROS service) thread, so a
// thread_local is enough to get the core::Result across without changing the
// Stream/domain-callback interface. Service callbacks run in a reentrant
// group, so this must not be a plain static.
static thread_local qrb::audio_common_lib::core::Result g_last_open_error;

static qrb::audio_common_lib::core::AudioStreamConfig make_capture_config(
    const StreamConfigs * configs)
{
  const auto & backend_defaults = AudioManager::get_backend_defaults();
  qrb::audio_common_lib::core::AudioStreamConfig config;
  config.device = backend_defaults.capture_device;
  config.sample_rate = configs->sample_rate;
  config.channels = configs->channels;
  config.sample_format = configs->sample_format;
  config.fragment_duration_ms = backend_defaults.capture_fragment_duration_ms;
  config.processors = backend_defaults.capture_processors;
  // With processors: 10 ms blocks, the only size WebRTC APM accepts.
  // Without: frame_duration_ms is only validated, never used for framing.
  config.frame_duration_ms = !config.processors.empty() ?
                                 10 :
                             backend_defaults.capture_fragment_duration_ms ?
                                 backend_defaults.capture_fragment_duration_ms :
                                 20;
  config.need_timestamp = configs->need_timestamp;
  return config;
}

static uint32_t register_capture_backend(
    std::shared_ptr<qrb::audio_common_lib::core::IAudioCaptureBackend> backend)
{
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);

  std::lock_guard<std::mutex> lock(g_capture_backends_mutex);
  uint32_t handle = dist(gen);
  while (handle == 0 || g_capture_backends.find(handle) != g_capture_backends.end()) {
    handle = dist(gen);
  }
  g_capture_backends[handle] = std::move(backend);
  return handle;
}

static bool audio_common_stream_cb(const void * const payload,
    StreamCommand cmd,
    uint32_t & ac_handle)
{
  using namespace qrb::audio_common_lib;
  const StreamConfigs * configs = static_cast<const StreamConfigs *>(payload);
  uint32_t am_handle = configs->stream_handle;

  bool use_new_capture_backend = configs->type == StreamType::RECORD && configs->source.empty();

  if (cmd == StreamCommand::OPEN)
    g_last_open_error = core::Result::Ok();

  if (cmd == StreamCommand::OPEN && use_new_capture_backend) {
    // Every streaming capture goes through ProcessingCaptureBackend, even
    // with no processors: it moves delivery (ROS publish) off the
    // PulseAudio mainloop thread onto its own worker.
    const core::AudioStreamConfig config = make_capture_config(configs);
    std::unique_ptr<core::ProcessingPipeline> pipeline;
    core::Result result = core::build_pipeline(config.processors, pipeline);
    if (!result.ok()) {
      AM_LOGE(LOG_TAG, "capture processing pipeline: " << result.to_log_string());
      g_last_open_error = result;
      return false;
    }
    // Queue ~2 s of audio (min 8 chunks, PipeWire delivers in bursts): long
    // enough to ride out a stall, short enough that a too-slow processor
    // starts dropping instead of building seconds of latency.
    const uint32_t chunk_ms = config.fragment_duration_ms ? config.fragment_duration_ms : 80;
    const size_t queue_chunks = std::max<size_t>(8, (2000 + chunk_ms - 1) / chunk_ms);
    auto backend = std::make_shared<core::ProcessingCaptureBackend>(
        std::make_unique<core::PipeWirePulseBackend>(), std::move(pipeline), queue_chunks);
    result = backend->configure(config);
    if (!result.ok()) {
      AM_LOGE(LOG_TAG, "capture backend configure failed: " << result.to_log_string());
      g_last_open_error = result;
      return false;
    }

    ac_handle = register_capture_backend(std::move(backend));
    return true;
  }

  if (cmd != StreamCommand::OPEN) {
    // Dispatch on registry membership, not configs->type: a record-to-file
    // stream (source non-empty) is StreamType::RECORD too but lives in the
    // legacy handle space, so it must fall through to the switch below.
    std::unique_lock<std::mutex> lock(g_capture_backends_mutex);
    auto it = g_capture_backends.find(ac_handle);
    if (it != g_capture_backends.end()) {
      switch (cmd) {
        case StreamCommand::START: {
          std::shared_ptr<core::IAudioCaptureBackend> backend = it->second;
          lock.unlock();  // a concurrent STOP/CLOSE must not wait for start()
          core::Result result = backend->start([am_handle](const core::AudioFrame & frame) {
            if (g_stream_data_cb) {
              StreamDataInfo info;
              info.read_duration_usec = frame.read_duration_usec;
              info.capture_time_mono_usec = frame.timestamp_usec;
              g_stream_data_cb(am_handle, frame.data, info);
            }
          });
          if (!result.ok())
            AM_LOGE(LOG_TAG, "capture backend start failed: " << result.message);
          return result.ok();
        }
        case StreamCommand::STOP:
          return it->second->stop().ok();
        case StreamCommand::CLOSE:
          it->second->stop();
          g_capture_backends.erase(it);
          return true;
        case StreamCommand::MUTE:
          // Capture never supported mute through the legacy API either
          // (audio_stream_mute() only acts on a PulseAudio sink-input, i.e.
          // playback); keep that behavior.
          return false;
        default:
          return false;
      }
    }
  }

  switch (cmd) {
    case StreamCommand::OPEN: {
      AudioStreamInfo info;
      info.rate = configs->sample_rate;
      info.channels = configs->channels;
      info.format = configs->sample_format;
      info.volume = configs->volume;
      info.repeat = configs->repeat;

      const auto & backend_defaults = AudioManager::get_backend_defaults();
      if (configs->type == StreamType::PLAYBACK) {
        info.type = qrb::audio_common_lib::StreamType::Playback;
        info.file_path = configs->source;
        info.device = backend_defaults.playback_device;
        info.buffer_duration_ms = backend_defaults.playback_buffer_duration_ms;
        if (!configs->subs_name.empty())
          info.pcm_mode = true;
      } else {
        // RECORD with a non-empty source (record-to-file): legacy path only,
        // use_new_capture_backend above is false whenever source is set.
        // Apply the same format/device validation as the streaming path:
        // without it this path had the same two silent failures (unknown
        // sample_format recorded as S16, unknown device recorded nothing).
        core::AudioStreamConfig check = make_capture_config(configs);
        core::Result result = core::validate_capture_config(check);
        if (result.ok())
          result = core::PipeWirePulseBackend::check_capture_device(check.device);
        if (!result.ok()) {
          AM_LOGE(LOG_TAG, "record-to-file config rejected: " << result.to_log_string());
          g_last_open_error = result;
          return false;
        }
        // The legacy path never produces capture timestamps (its StreamData
        // carries only the read duration). Accept the request so existing
        // callers keep working, but say so instead of silently publishing
        // zero stamps.
        if (configs->need_timestamp)
          AM_LOGI(LOG_TAG,
              "need_timestamp is not supported for record-to-file streams "
              "(source set); published AudioData.timestamp will be zero");
        info.type = qrb::audio_common_lib::StreamType::Capture;
        info.file_path = configs->source;
        info.device = backend_defaults.capture_device;
        info.fragment_duration_ms = backend_defaults.capture_fragment_duration_ms;
        if (configs->pub_pcm || !configs->pub_name.empty())
          info.pcm_mode = true;
      }

      auto event_cb = [am_handle](StreamEvent event, StreamEventData data, void * userdata) {
        uint32_t handle = static_cast<uint32_t>(reinterpret_cast<intptr_t>(userdata));
        std::string cmd_str;
        bool result = true;
        switch (event) {
          case StreamEvent::StreamStart:
            cmd_str = audio_stream_cmd_name[StreamCommand::START];
            break;
          case StreamEvent::StreamStoped:
            cmd_str = audio_stream_cmd_name[StreamCommand::STOP];
            break;
          case StreamEvent::StreamAbort:
            cmd_str = audio_stream_cmd_name[StreamCommand::STOP];
            result = false;
            break;
          case StreamEvent::StreamEos:
            cmd_str = audio_stream_cmd_name[StreamCommand::EOS];
            break;
          case StreamEvent::StreamData:
            if (g_stream_data_cb) {
              StreamDataInfo info;
              info.read_duration_usec = data.usec;  // legacy path: no capture timestamp
              g_stream_data_cb(am_handle, data.data_buf, info);
            }
            return;
          default:
            return;
        }
        AudioManager::on_task_completed(DOMAIN_ID_AUDIO_COMMON, am_handle, handle, cmd_str, result);
      };

      ac_handle = audio_stream_open(info, event_cb);
      return ac_handle != 0;
    }
    case StreamCommand::START:
      return audio_stream_start(ac_handle) == 0;
    case StreamCommand::STOP:
      return audio_stream_stop(ac_handle) == 0;
    case StreamCommand::CLOSE:
      return audio_stream_close(ac_handle) == 0;
    case StreamCommand::MUTE:
      return audio_stream_mute(ac_handle, configs->mute) == 0;
    default:
      return false;
  }
}

AudioManager * AudioManager::get_instance()
{
  if (instance_ == nullptr) {
    instance_ = new AudioManager();
    instance_->load_buildin_sounds(BUILDIN_SOUNDS_PATH);
    qrb::audio_common_lib::detect_audio_backend();
    Stream::register_callback(DOMAIN_ID_AUDIO_COMMON, audio_common_stream_cb, false);
  }
  return instance_;
}

void AudioManager::set_stream_data_callback(stream_data_cb_t cb)
{
  stream_data_cb_ = cb;
  g_stream_data_cb = cb;
}

void AudioManager::set_backend_defaults(const AudioBackendDefaults & defaults)
{
  {
    std::lock_guard<std::mutex> lock(g_backend_defaults_mutex);
    backend_defaults_ = defaults;
  }
  std::string processors;
  for (const auto & slot : defaults.capture_processors) {
    processors += (processors.empty() ? "" : ", ") + slot.name;
    for (const auto & kv : slot.config)
      processors += " " + kv.first + "=" + kv.second;
  }
  AM_LOGI(
      LOG_TAG, "backend defaults updated: capture_device='"
                   << defaults.capture_device << "', playback_device='" << defaults.playback_device
                   << "', capture_fragment_duration_ms=" << defaults.capture_fragment_duration_ms
                   << ", playback_buffer_duration_ms=" << defaults.playback_buffer_duration_ms
                   << ", capture_processors=[" << processors << "]");
}

AudioBackendDefaults AudioManager::get_backend_defaults()
{
  std::lock_guard<std::mutex> lock(g_backend_defaults_mutex);
  return backend_defaults_;
}

bool AudioManager::write_stream(uint32_t stream_handle, const void * data, size_t size)
{
  using namespace qrb::audio_common_lib;
  auto stream = find_stream(stream_handle);
  if (!stream)
    return false;

  uint32_t ac_handle = stream->get_domain_handle(DOMAIN_ID_AUDIO_COMMON);
  return audio_stream_write(ac_handle, data, size) >= 0;
}

uint32_t AudioManager::create_playback_stream(std::string source,
    uint32_t sample_rate,
    uint8_t channels,
    uint8_t sample_format,
    std::string coding_format,
    uint8_t volume,
    std::string play_mode,
    int8_t repeat,
    std::string subs_name)
{
  auto stream_handle = generate_key();
  while (streams_.find(stream_handle) != streams_.end()) {
    stream_handle = generate_key();
  }

  auto it = buildin_sounds_.find(source);
  if (it != buildin_sounds_.end())
    source = it->second;

  std::shared_ptr<Stream> stream_ptr = std::make_shared<PlaybackStream>(stream_handle, source,
      sample_rate, channels, sample_format, coding_format, volume, play_mode, repeat, subs_name);

  if (!stream_ptr->open()) {
    stream_ptr = nullptr;
    throw std::runtime_error("create playback stream failed");
  }

  streams_[stream_handle] = stream_ptr;

  AM_LOGI(LOG_TAG, "create playback stream, handle 0x"
                       << (std::ostringstream() << std::hex << stream_handle).str() << " success");

  try {
    if (!stream_ptr->get_domain_async_mode(DOMAIN_ID_AUDIO_COMMON)) {
      on_task_completed(DOMAIN_ID_AUDIO_COMMON, stream_handle,
          stream_ptr->get_domain_handle(DOMAIN_ID_AUDIO_COMMON),
          audio_stream_cmd_name[StreamCommand::OPEN], true);
    }
  } catch (const std::exception & e) {
    AM_LOGE(LOG_TAG, "caught exception: " << e.what());
  }

  return stream_handle;
}

uint32_t AudioManager::create_record_stream(uint32_t sample_rate,
    uint8_t channels,
    uint8_t sample_format,
    std::string coding_format,
    std::string source,
    bool pub_pcm,
    std::string pub_name,
    bool need_timestamp)
{
  auto stream_handle = generate_key();
  while (streams_.find(stream_handle) != streams_.end()) {
    stream_handle = generate_key();
  }

  std::shared_ptr<Stream> stream_ptr = std::make_shared<RecordStream>(stream_handle, sample_rate,
      channels, sample_format, coding_format, source, pub_pcm, pub_name, need_timestamp);

  if (!stream_ptr->open()) {
    stream_ptr = nullptr;
    using qrb::audio_common_lib::core::ErrorCode;
    using qrb::audio_common_lib::core::Result;
    Result reason = g_last_open_error.ok() ?
                        Result::Error(ErrorCode::kBackendInitFailed, "audio_stream_open failed") :
                        g_last_open_error;
    throw std::runtime_error("create record stream failed: " + reason.to_log_string());
  }

  streams_[stream_handle] = stream_ptr;

  AM_LOGI(LOG_TAG, "create record stream, handle 0x" +
                       (std::ostringstream() << std::hex << stream_handle).str() + " success");

  return stream_handle;
}

bool AudioManager::start_stream(uint32_t stream_handle)
{
  auto ret = false;

  auto stream = find_stream(stream_handle);
  if (stream) {
    AM_LOGD(LOG_TAG,
        "start stream handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
    ret = stream->start();
  }

  if (!ret) {
    streams_.erase(stream_handle);
    AM_LOGE(LOG_TAG,
        "erase stream, handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
  }

  return ret;
}

bool AudioManager::stop_stream(uint32_t stream_handle)
{
  auto ret = false;

  auto stream = find_stream(stream_handle);
  if (stream) {
    AM_LOGD(LOG_TAG,
        "stop stream handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
    ret = stream->stop();

    try {
      if (!stream->get_domain_async_mode(DOMAIN_ID_AUDIO_COMMON))
        on_task_completed(DOMAIN_ID_AUDIO_COMMON, stream_handle,
            stream->get_domain_handle(DOMAIN_ID_AUDIO_COMMON),
            audio_stream_cmd_name[StreamCommand::STOP], ret);
    } catch (const std::exception & e) {
      AM_LOGE(LOG_TAG, "caught exception: " << e.what());
    }

    if (!ret) {
      streams_.erase(stream_handle);
      AM_LOGE(LOG_TAG,
          "erase stream, handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
    }
  }

  return ret;
}

bool AudioManager::release_stream(uint32_t stream_handle)
{
  auto ret = false;

  auto stream = find_stream(stream_handle);
  if (stream) {
    AM_LOGD(LOG_TAG,
        "release stream handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
    ret = stream->close();
  }

  streams_.erase(stream_handle);

  return ret;
}

bool AudioManager::mute_stream(uint32_t stream_handle, bool mute)
{
  auto ret = false;

  auto stream = find_stream(stream_handle);
  if (stream && (stream->get_stream_configs().type == StreamType::PLAYBACK) &&
      (stream->get_stream_configs().mute) != mute) {
    AM_LOGD(LOG_TAG,
        "mute stream handle 0x" << (std::ostringstream() << std::hex << stream_handle).str());
    auto playback_stream = std::static_pointer_cast<PlaybackStream>(stream);
    ret = playback_stream->set_mute(mute);
  }

  return ret;
}

std::shared_ptr<Stream> AudioManager::find_stream(uint32_t stream_handle)
{
  auto stream = streams_.find(stream_handle);
  auto stream_handle_str = (std::ostringstream() << std::hex << stream_handle).str();

  if (stream != streams_.end()) {
    AM_LOGV(LOG_TAG, "find stream handle 0x" << stream_handle_str);
    return stream->second;
  }

  return nullptr;
}

uint32_t AudioManager::generate_key()
{
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);

  return dist(gen);
}

void AudioManager::worker_thread(MessageQueue & queue)
{
  struct as_message msg;

  while (queue.pop(msg)) {
    auto stream_handle = msg.stream_handle;
    auto stream_handle_str = (std::ostringstream() << std::hex << stream_handle).str();

    auto cmd = msg.command;
    auto cmd_str = audio_stream_cmd_name[static_cast<StreamCommand>(cmd)];

    AM_LOGI(LOG_TAG, "worker thread received message, stream handle 0x" << stream_handle_str
                                                                        << ", command " << cmd_str);

    auto stream = find_stream(stream_handle);
    if (!stream) {
      AM_LOGE(LOG_TAG, "invalid stream handle 0x" << stream_handle_str);
      continue;
    }

    switch (static_cast<StreamCommand>(cmd)) {
      case StreamCommand::START:
        start_stream(stream_handle);
        break;
      case StreamCommand::STOP:
        stop_stream(stream_handle);
        break;
      case StreamCommand::CLOSE:
        release_stream(stream_handle);
        break;
      default:
        AM_LOGE(LOG_TAG, "unsupport command " << cmd_str);
        break;
    }
  }
}

void AudioManager::on_task_completed(int domain,
    uint32_t stream_handle,
    uint32_t audio_domain_handle,
    std::string cmd,
    bool result)
{
  std::shared_ptr<Stream> stream = nullptr;

  auto stream_handle_str = (std::ostringstream() << std::hex << stream_handle).str();
  auto audio_domain_handle_str = (std::ostringstream() << std::hex << audio_domain_handle).str();

  AM_LOGI(LOG_TAG, "domain " << domain << ", command " << cmd << ", result " << result
                             << ", stream_handle 0x" << stream_handle_str
                             << ", audio_domain_handle 0x" << audio_domain_handle_str);

  switch (domain) {
    case DOMAIN_ID_AUDIO_COMMON:
      if (audio_stream_cmd_name[StreamCommand::OPEN] == cmd) {
        for (const auto & kv : instance_->streams_) {
          if (kv.first == stream_handle) {
            stream = kv.second;
            break;
          }
        }

        if (!stream) {
          AM_LOGE(LOG_TAG, "invalid stream handle 0x" << stream_handle_str);
          return;
        }
        if (!stream->get_domain_handle(DOMAIN_ID_AUDIO_COMMON)) {
          stream->set_domain_handle(DOMAIN_ID_AUDIO_COMMON, audio_domain_handle);
          AM_LOGI(LOG_TAG, "set audio common handle 0x" << audio_domain_handle_str
                                                        << " for stream handle 0x"
                                                        << stream_handle_str);
        }
      }

      if (!stream) {
        for (const auto & kv : instance_->streams_) {
          if (kv.second->get_domain_handle(DOMAIN_ID_AUDIO_COMMON) == audio_domain_handle) {
            stream_handle = kv.first;
            stream_handle_str = (std::ostringstream() << std::hex << stream_handle).str();
            stream = kv.second;
            AM_LOGV(LOG_TAG, "found stream, handle is 0x" << stream_handle_str);
            break;
          }
        }
      }

      if (!result) {
        instance_->streams_.erase(stream_handle);
        AM_LOGE(LOG_TAG, "erase stream, handle 0x" << stream_handle_str);
        return;
      }

      if (stream && (stream->get_stream_configs().play_mode == "one-touch")) {
        struct as_message msg;
        msg.stream_handle = stream_handle;

        if (audio_stream_cmd_name[StreamCommand::OPEN] == cmd)
          msg.command = StreamCommand::START;
        else if (audio_stream_cmd_name[StreamCommand::EOS] == cmd)
          msg.command = StreamCommand::STOP;
        else if (audio_stream_cmd_name[StreamCommand::STOP] == cmd)
          msg.command = StreamCommand::CLOSE;
        else
          break;

        AM_LOGI(LOG_TAG, "handle one touch playback for stream handle 0x"
                             << stream_handle_str << " next command is "
                             << audio_stream_cmd_name[msg.command]);

        instance_->queue_.push(msg);
      }
      break;
    default:
      AM_LOGE(LOG_TAG, "unsupport domain " << domain);
      break;
  }
}

const std::map<std::string, std::string> & AudioManager::get_buildin_sounds()
{
  return buildin_sounds_;
}

void AudioManager::load_buildin_sounds(const std::string & path)
{
  DIR * dirp = opendir(path.c_str());
  if (dirp == nullptr) {
    return;
  }

  struct dirent * dp;
  while ((dp = readdir(dirp)) != nullptr) {
    std::string filename(dp->d_name);

    if (filename == "." || filename == "..") {
      continue;
    }

    size_t last_dot = filename.find_last_of(".");
    if (last_dot != std::string::npos) {
      filename = filename.substr(0, last_dot);
    }

    std::string filepath = path + "/" + dp->d_name;
    buildin_sounds_[filename] = filepath;
  }

  closedir(dirp);
}

}  // namespace audio_manager
}  // namespace qrb
