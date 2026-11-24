// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// clang-format off
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "qrb_ros_audio_service/audio_service.hpp"
#include "qrb_audio_manager/audio_manager.hpp"
// clang-format on

using namespace std::placeholders;

using qrb::audio_manager::audio_manager_cmd_name;
using qrb::audio_manager::AudioManager;
using qrb::audio_manager::AudioManagerCommand;

#define AUDIO_SERVER_NAME_NODE_NAME "audio_service"
#define AUDIO_SERVER_NAME "audio_server"

namespace qrb_ros
{
namespace audio_service
{

AudioServer::AudioServer(const rclcpp::NodeOptions & options)
  : Node(AUDIO_SERVER_NAME_NODE_NAME, options)
{
  callback_group_ = this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);

  server_ = this->create_service<AudioService>(AUDIO_SERVER_NAME,
      std::bind(&AudioServer::service_callback, this, _1, _2, _3), rmw_qos_profile_services_default,
      callback_group_);

  AudioManager::get_instance();

  AudioManager::set_stream_data_callback(std::bind(&AudioServer::on_stream_data, this, _1, _2, _3));

  latency_log_enabled_ = this->declare_parameter<bool>("enable_latency_log", false);

  declare_backend_parameters();
  apply_backend_parameters();

  param_callback_handle_ =
      this->add_on_set_parameters_callback([this](const std::vector<rclcpp::Parameter> & params) {
        rcl_interfaces::msg::SetParametersResult result;
        result.successful = true;
        bool backend_param_changed = false;
        for (const auto & param : params) {
          if (param.get_name() == "enable_latency_log") {
            latency_log_enabled_ = param.as_bool();
            RCLCPP_INFO(this->get_logger(), "enable_latency_log set to %s",
                latency_log_enabled_ ? "true" : "false");
          } else if (param.get_name() == "capture_device" || param.get_name() == "playback_device" ||
                     param.get_name() == "capture_fragment_duration_ms" ||
                     param.get_name() == "playback_buffer_duration_ms") {
            backend_param_changed = true;
          }
        }
        if (backend_param_changed) {
          // rclcpp invokes on_set_parameters callbacks BEFORE committing the new
          // values, so this->get_parameter() below would still return the OLD
          // value for whatever is in `params`. Pass the pending params through so
          // apply_backend_parameters() can prefer them over the (still-stale)
          // stored values. Only affects streams created after this point;
          // existing streams keep the config they were opened with.
          apply_backend_parameters(&params);
        }
        return result;
      });

  latency_log_timer_ = this->create_wall_timer(
      std::chrono::seconds(1), std::bind(&AudioServer::on_latency_log_timer, this));

  rclcpp::on_shutdown(std::bind(&AudioServer::shutdown_callback, this));
}

void AudioServer::on_stream_data(uint32_t am_handle,
    std::shared_ptr<std::vector<uint8_t>> data_buf,
    uint64_t usec)
{
  bool log_enabled = latency_log_enabled_.load();

  auto it = capture_pubs_.find(am_handle);
  if (it == capture_pubs_.end())
    return;

  std::chrono::steady_clock::time_point publish_start;
  if (log_enabled)
    publish_start = std::chrono::steady_clock::now();

  AudioData msg;
  msg.stream_handle = am_handle;
  if (data_buf)
    msg.data = std::move(*data_buf);
  it->second->publish(msg);

  if (log_enabled) {
    auto t2 = std::chrono::steady_clock::now();
    uint64_t publish_usec =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - publish_start).count();
    uint64_t latency_usec = usec + publish_usec;
    update_latency_stats(capture_latency_stats_, am_handle, latency_usec);
  }
}

void AudioServer::on_audio_data(uint32_t am_handle,
    const qrb_ros_audio_service_msgs::msg::AudioData::SharedPtr msg)
{
  if (msg->data.empty())
    return;

  bool log_enabled = latency_log_enabled_.load();
  std::chrono::steady_clock::time_point t3;
  if (log_enabled)
    t3 = std::chrono::steady_clock::now();

  AudioManager::get_instance()->write_stream(am_handle, msg->data.data(), msg->data.size());

  if (log_enabled) {
    auto t4 = std::chrono::steady_clock::now();
    uint64_t latency_usec = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count();
    update_latency_stats(playback_latency_stats_, am_handle, latency_usec);
  }
}

void AudioServer::update_latency_stats(std::unordered_map<uint32_t, LatencyStats> & stats_map,
    uint32_t handle,
    uint64_t latency_usec)
{
  std::lock_guard<std::mutex> lock(latency_stats_mutex_);
  auto & stats = stats_map[handle];
  stats.count++;
  stats.sum_usec += latency_usec;
  stats.max_usec = std::max(stats.max_usec, latency_usec);
  stats.min_usec = std::min(stats.min_usec, latency_usec);
}

void AudioServer::on_latency_log_timer()
{
  if (!latency_log_enabled_.load())
    return;

  std::lock_guard<std::mutex> lock(latency_stats_mutex_);

  for (auto & [handle, stats] : capture_latency_stats_) {
    if (stats.count == 0)
      continue;
    RCLCPP_INFO(this->get_logger(),
        "[latency][capture] handle=0x%x avg=%luus min=%luus max=%luus count=%lu", handle,
        stats.sum_usec / stats.count, stats.min_usec, stats.max_usec, stats.count);
  }
  capture_latency_stats_.clear();

  for (auto & [handle, stats] : playback_latency_stats_) {
    if (stats.count == 0)
      continue;
    RCLCPP_INFO(this->get_logger(),
        "[latency][playback] handle=0x%x avg=%luus min=%luus max=%luus count=%lu", handle,
        stats.sum_usec / stats.count, stats.min_usec, stats.max_usec, stats.count);
  }
  playback_latency_stats_.clear();
}

void AudioServer::create_pcm_topic(uint32_t stream_handle,
    const std::string & topic_name,
    bool is_capture)
{
  if (is_capture) {
    rclcpp::PublisherOptions pub_options;
    pub_options.use_intra_process_comm = rclcpp::IntraProcessSetting::Enable;
    capture_pubs_[stream_handle] = this->create_publisher<AudioData>(topic_name, 10, pub_options);
  } else {
    rclcpp::SubscriptionOptions sub_options;
    sub_options.use_intra_process_comm = rclcpp::IntraProcessSetting::Enable;
    playback_subs_[stream_handle] = this->create_subscription<AudioData>(
        topic_name, 10,
        [this, stream_handle](const qrb_ros_audio_service_msgs::msg::AudioData::SharedPtr msg) {
          on_audio_data(stream_handle, msg);
        },
        sub_options);
  }
}

void AudioServer::delete_pcm_topic(uint32_t stream_handle)
{
  capture_pubs_.erase(stream_handle);
  playback_subs_.erase(stream_handle);
}

void AudioServer::service_callback(const std::shared_ptr<rmw_request_id_t> request_header,
    const std::shared_ptr<AudioService::Request> request,
    std::shared_ptr<AudioService::Response> response)
{
  (void)request_header;

  RCLCPP_INFO(this->get_logger(), "received service call");

  auto ret = false;

  auto channels = request->audio_info.channels;
  auto sample_rate = request->audio_info.sample_rate;
  auto sample_format = request->audio_info.sample_format;
  auto coding_format = request->audio_info.coding_format;

  auto command = request->command;
  auto type = request->type;
  auto source = request->source;
  auto volume = request->volume;
  auto repeat = request->repeat;
  auto stream_handle_req = request->stream_handle;
  auto pub_pcm = request->pub_pcm;
  auto topic_name = request->topic_name;
  auto mute = request->mute;

  std::string play_mode = "normal";
  uint32_t stream_handle_by_create = 0;
  std::vector<std::string> buildin_sound_name = {};
  auto buildin_sound_count = 0;

  auto am = AudioManager::get_instance();

  switch (static_cast<int>(audio_manager_cmd_name[command])) {
    case static_cast<int>(AudioManagerCommand::PLAY):
      play_mode = "one-touch";
      type = "playback";
    case static_cast<int>(AudioManagerCommand::CREATE):
      if ((type == "playback") || (play_mode == "one-touch")) {
        RCLCPP_INFO(this->get_logger(),
            "source %s, coding_format %s, volume %d, play_mode %s, repeat %d topic_name %s",
            source.c_str(), coding_format.c_str(), volume, play_mode.c_str(), repeat,
            topic_name.c_str());
        if (!source.empty() && !topic_name.empty()) {
          RCLCPP_ERROR(this->get_logger(), "either source or topic_name can exist, but not both");
          break;
        }
        if (!source.empty() || !topic_name.empty()) {
          try {
            stream_handle_by_create = am->create_playback_stream(source, sample_rate, channels,
                sample_format, coding_format, volume, play_mode, repeat, topic_name);
            ret = true;
            if (!topic_name.empty())
              create_pcm_topic(stream_handle_by_create, topic_name, false);
          } catch (const std::exception & e) {
            RCLCPP_ERROR(this->get_logger(), "%s", e.what());
          }
        }
      } else if (type == "record") {
        RCLCPP_INFO(this->get_logger(),
            "source %s, channels %d, sample_rate %d, sample_format %d, "
            "coding_format %s, pub_pcm %s, topic_name %s",
            source.c_str(), channels, sample_rate, sample_format, coding_format.c_str(),
            (pub_pcm ? "true" : "false"), topic_name.c_str());
        if (!((source.empty() && !pub_pcm) || (0 == channels) || (0 == sample_rate) ||
                (0 == sample_format))) {
          try {
            stream_handle_by_create = am->create_record_stream(
                sample_rate, channels, sample_format, coding_format, source, pub_pcm, topic_name);
            ret = true;
            if (pub_pcm || !topic_name.empty())
              create_pcm_topic(stream_handle_by_create, topic_name, true);
          } catch (const std::exception & e) {
            RCLCPP_ERROR(this->get_logger(), "%s", e.what());
          }
        }
      }
      response->stream_handle = stream_handle_by_create;
      break;
    case static_cast<int>(AudioManagerCommand::START):
      ret = am->start_stream(stream_handle_req);
      break;
    case static_cast<int>(AudioManagerCommand::MUTE):
      ret = am->mute_stream(stream_handle_req, mute);
      break;
    case static_cast<int>(AudioManagerCommand::STOP):
      ret = am->stop_stream(stream_handle_req);
      break;
    case static_cast<int>(AudioManagerCommand::RELEASE):
      ret = am->release_stream(stream_handle_req);
      delete_pcm_topic(stream_handle_req);
      break;
    case static_cast<int>(AudioManagerCommand::GETBUILDINSOUND):
      for (const auto & pair : am->get_buildin_sounds())
        buildin_sound_name.push_back(pair.first);
      if ((buildin_sound_count = buildin_sound_name.size()) > 0) {
        response->buildin_sound_name.resize(buildin_sound_count);
        for (std::size_t i = 0; i < buildin_sound_name.size(); ++i)
          response->buildin_sound_name[i] = buildin_sound_name[i];
        ret = true;
      }
      break;
    default:
      RCLCPP_ERROR(this->get_logger(), "unsupported command: %s", command.c_str());
      break;
  }

  response->success = ret;
}

void AudioServer::declare_backend_parameters()
{
  rcl_interfaces::msg::ParameterDescriptor device_desc;
  device_desc.description =
      "PipeWire/PulseAudio source(capture)/sink(playback) name. Empty means use the "
      "backend's default device. See `pactl list sources/sinks short` (or the "
      "pipewire-pulse equivalent) for valid names.";

  // Upper bound is arbitrary but keeps a fat-fingered value (or a negative one,
  // see below) from turning into a multi-second buffer or, after the uint32_t
  // cast, wrapping into a huge fragsize/prebuf passed straight to PulseAudio.
  rcl_interfaces::msg::IntegerRange duration_range;
  duration_range.from_value = 0;
  duration_range.to_value = 2000;
  duration_range.step = 1;

  rcl_interfaces::msg::ParameterDescriptor capture_fragment_desc;
  capture_fragment_desc.description =
      "Capture fragment size requested from the backend, in milliseconds. Lower values "
      "reduce latency at the cost of more frequent wakeups. 0 means use the library default "
      "(80ms).";
  capture_fragment_desc.integer_range = { duration_range };

  rcl_interfaces::msg::ParameterDescriptor playback_buffer_desc;
  playback_buffer_desc.description =
      "Playback pre-buffer/minreq size requested from the backend, in milliseconds. 0 means "
      "use the library default (300ms).";
  playback_buffer_desc.integer_range = { duration_range };

  this->declare_parameter<std::string>("capture_device", "", device_desc);
  this->declare_parameter<std::string>("playback_device", "", device_desc);
  this->declare_parameter<int>("capture_fragment_duration_ms", 0, capture_fragment_desc);
  this->declare_parameter<int>("playback_buffer_duration_ms", 0, playback_buffer_desc);
}

namespace
{
// Prefers the value for `name` from `pending` (parameters an on_set_parameters
// callback is currently validating, not yet committed) and falls back to
// `node`'s already-committed value otherwise.
std::string pending_or_current_string(rclcpp::Node * node,
    const std::vector<rclcpp::Parameter> * pending,
    const std::string & name)
{
  if (pending) {
    for (const auto & param : *pending) {
      if (param.get_name() == name)
        return param.as_string();
    }
  }
  return node->get_parameter(name).as_string();
}

int64_t pending_or_current_int(rclcpp::Node * node,
    const std::vector<rclcpp::Parameter> * pending,
    const std::string & name)
{
  if (pending) {
    for (const auto & param : *pending) {
      if (param.get_name() == name)
        return param.as_int();
    }
  }
  return node->get_parameter(name).as_int();
}
}  // namespace

void AudioServer::apply_backend_parameters(const std::vector<rclcpp::Parameter> * pending)
{
  qrb::audio_manager::AudioBackendDefaults defaults;
  defaults.capture_device = pending_or_current_string(this, pending, "capture_device");
  defaults.playback_device = pending_or_current_string(this, pending, "playback_device");
  defaults.capture_fragment_duration_ms = static_cast<uint32_t>(
      pending_or_current_int(this, pending, "capture_fragment_duration_ms"));
  defaults.playback_buffer_duration_ms = static_cast<uint32_t>(
      pending_or_current_int(this, pending, "playback_buffer_duration_ms"));

  AudioManager::set_backend_defaults(defaults);
}

void AudioServer::shutdown_callback()
{
  AudioManager::get_instance()->clean();
}

}  // namespace audio_service
}  // namespace qrb_ros

#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(qrb_ros::audio_service::AudioServer)