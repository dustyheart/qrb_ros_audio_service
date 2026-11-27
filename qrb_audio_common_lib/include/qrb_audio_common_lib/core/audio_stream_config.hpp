// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/core/audio_processor.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Which processors are attached to a stream, and in what order. `name` is a
// key understood by core::create_processor() (core/processor_factory.hpp);
// `config` is passed verbatim to that processor's configure().
struct ProcessorSlot
{
  std::string name;
  bool bypassed = false;
  ProcessorConfig config;
};

// Unified capture/playback configuration, replacing the ad hoc combination of
// AudioStreamInfo fields + macros that qrb_audio_common_lib used before. This
// is the config type AudioManager and (later) the ROS layer should build and
// pass down; AudioStreamInfo remains the PulseAudio-backend-facing type for
// now and is constructed from this where the two overlap.
struct AudioStreamConfig
{
  std::string device;  // capture source / playback sink name, empty = backend default

  uint32_t sample_rate = 16000;
  uint8_t channels = 1;
  uint8_t sample_format = 16;  // bit depth

  // Processing block size, independent of the backend fragment. Only used
  // when `processors` is non-empty: capture chunks are re-framed into blocks
  // of this length for the processors (WebRTC APM needs exactly 10 ms), then
  // re-assembled into the original chunk sizes before delivery.
  uint32_t frame_duration_ms = 20;
  uint32_t fragment_duration_ms = 0;  // 0 = backend default (capture)
  uint32_t buffer_duration_ms = 0;    // 0 = backend default (playback)

  bool publish_pcm = false;
  bool need_timestamp = false;

  std::vector<ProcessorSlot> processors;  // empty = passthrough, no processing pipeline

  // Echo reference for an "aec" stage (ignored without one): the capture
  // source carrying what the speaker plays, normally a sink monitor.
  // "@DEFAULT_MONITOR@" = monitor of the default sink. Captured mono at
  // sample_rate by ProcessingCaptureBackend's reference backend.
  std::string reference_device = "@DEFAULT_MONITOR@";
  // Reference/mic pairing bias towards newer reference (ReferenceBuffer).
  uint32_t reference_lead_ms = 5;
  // Re-place the reference by timestamps when sample counting has drifted
  // further than this; 0 = never (ReferenceBuffer::Options).
  uint32_t reference_resync_ms = 40;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_
