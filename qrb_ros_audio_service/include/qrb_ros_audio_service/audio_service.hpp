// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_ROS_AUDIO_SERVICE__AUDIO_SERVICE_HPP_
#define QRB_ROS_AUDIO_SERVICE__AUDIO_SERVICE_HPP_

// clang-format off
#include <rclcpp/rclcpp.hpp>
#include <rmw/types.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "builtin_interfaces/msg/time.hpp"
#include "std_msgs/msg/bool.hpp"
#include "qrb_audio_manager/audio_manager.hpp"
#include "qrb_ros_audio_service_msgs/msg/audio_data.hpp"
#include "qrb_ros_audio_service_msgs/srv/audio_request.hpp"
// clang-format on

using AudioService = qrb_ros_audio_service_msgs::srv::AudioRequest;
using AudioData = qrb_ros_audio_service_msgs::msg::AudioData;

namespace qrb_ros
{
namespace audio_service
{

struct LatencyStats
{
  uint64_t count = 0;
  uint64_t sum_usec = 0;
  uint64_t max_usec = 0;
  uint64_t min_usec = UINT64_MAX;
};

class AudioServer : public rclcpp::Node
{
public:
  explicit AudioServer(const rclcpp::NodeOptions & options);

private:
  rclcpp::CallbackGroup::SharedPtr callback_group_{nullptr};
  rclcpp::Service<AudioService>::SharedPtr server_{nullptr};

  // Read on the capture worker thread (on_stream_data), modified on service
  // threads (create/release of any stream), hence the mutex.
  std::unordered_map<uint32_t, rclcpp::Publisher<AudioData>::SharedPtr> capture_pubs_;
  // Stage-5 STOPGAP: per-stream VAD decisions on "<pcm_topic><vad_topic_suffix>"
  // as std_msgs/Bool, one message right after each PCM message of a stream
  // whose capture chain has "vad" (no message while the decision is unknown).
  // Bool carries no stream_handle/timestamp; pair it with the AudioData
  // message it follows. To be replaced by a proper msg in qrb_ros_interfaces
  // when stage 3 revises the interfaces.
  std::unordered_map<uint32_t, rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr> vad_pubs_;
  std::mutex capture_pubs_mutex_;  // guards capture_pubs_ and vad_pubs_
  std::unordered_map<uint32_t, rclcpp::Subscription<AudioData>::SharedPtr> playback_subs_;

  std::unordered_map<uint32_t, LatencyStats> capture_latency_stats_;
  std::unordered_map<uint32_t, LatencyStats> playback_latency_stats_;
  std::mutex latency_stats_mutex_;
  rclcpp::TimerBase::SharedPtr latency_log_timer_;
  std::atomic<bool> latency_log_enabled_{false};
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr param_callback_handle_;

  // Per capture stream: (monotonic ns, ROS clock ns) sampled back-to-back
  // when the stream delivered its first timestamped chunk. Written/read on
  // the capture worker thread (on_stream_data), erased on the service
  // thread (release), hence the mutex.
  struct ClockBase
  {
    int64_t mono_ns = 0;
    int64_t ros_ns = 0;
  };
  std::unordered_map<uint32_t, ClockBase> stamp_bases_;
  std::mutex stamp_bases_mutex_;

  void service_callback(const std::shared_ptr<rmw_request_id_t> request_header,
      const std::shared_ptr<AudioService::Request> request,
      std::shared_ptr<AudioService::Response> response);
  void shutdown_callback();
  static qrb::audio_common_lib::core::Result parse_processor_specs(
      const std::vector<std::string> & specs,
      std::vector<qrb::audio_common_lib::core::ProcessorSlot> & out);
  void on_stream_data(uint32_t am_handle,
      std::shared_ptr<std::vector<uint8_t>> data_buf,
      const qrb::audio_manager::StreamDataInfo & info);
  // CLOCK_MONOTONIC capture time -> this node's ROS clock. Zero stamp when
  // capture_time_mono_usec is 0 or the mapping is out of range.
  builtin_interfaces::msg::Time to_ros_stamp(uint32_t am_handle, uint64_t capture_time_mono_usec);
  void on_audio_data(uint32_t am_handle,
      const qrb_ros_audio_service_msgs::msg::AudioData::SharedPtr msg);
  void create_pcm_topic(uint32_t stream_handle, const std::string & topic_name, bool is_capture);
  void delete_pcm_topic(uint32_t stream_handle);
  void update_latency_stats(std::unordered_map<uint32_t, LatencyStats> & stats_map,
      uint32_t handle,
      uint64_t latency_usec);
  void on_latency_log_timer();
  void declare_backend_parameters();
  // If pending is non-null, values for names present in it take priority over
  // this->get_parameter() (which, when called from an on_set_parameters
  // callback, would still return the pre-change value - see call site).
  qrb::audio_common_lib::core::Result build_backend_defaults(
      const std::vector<rclcpp::Parameter> * pending,
      qrb::audio_manager::AudioBackendDefaults & out);
  void apply_backend_parameters(const std::vector<rclcpp::Parameter> * pending = nullptr);
};

}  // namespace audio_service
}  // namespace qrb_ros

#endif  // QRB_ROS_AUDIO_SERVICE__AUDIO_SERVICE_HPP_