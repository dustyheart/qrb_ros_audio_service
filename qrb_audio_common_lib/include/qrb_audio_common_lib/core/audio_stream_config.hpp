// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Which processors are attached to a stream, and in what order. Names are
// registry keys (see ProcessorRegistry, introduced alongside the first real
// processor), not yet backed by per-processor config here - that is added
// once a concrete processor (NS/VAD/AEC) defines what it needs.
struct ProcessorSlot
{
  std::string name;
  bool bypassed = false;
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

  uint32_t frame_duration_ms = 20;      // processing frame size, independent of backend fragment
  uint32_t fragment_duration_ms = 0;     // 0 = backend default (capture)
  uint32_t buffer_duration_ms = 0;       // 0 = backend default (playback)

  bool publish_pcm = false;
  bool need_timestamp = false;

  std::vector<ProcessorSlot> processors;  // empty = passthrough, no processing pipeline
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STREAM_CONFIG_HPP_
