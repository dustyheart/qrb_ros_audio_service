// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_FRAME_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_FRAME_HPP_

#include <cstdint>
#include <memory>
#include <vector>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

enum class FrameFlag : uint32_t
{
  kNone = 0,
  kDropped = 1u << 0,   // gap detected before this frame (sequence not contiguous)
  kSilence = 1u << 1,   // known silence, e.g. during a muted/bypassed window
  kBypassed = 1u << 2,  // passed through a processor stage without being touched
};

// Voice activity decision attached to a frame by a VAD processor. Ordered so
// that the "most active" of several values is simply the max.
enum class VoiceActivity : int8_t
{
  kUnknown = -1,  // no VAD in the chain, VAD bypassed/failed, or not yet decided
  kInactive = 0,
  kActive = 1,
};

inline FrameFlag operator|(FrameFlag a, FrameFlag b)
{
  return static_cast<FrameFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_flag(uint32_t flags, FrameFlag f)
{
  return (flags & static_cast<uint32_t>(f)) != 0;
}

// The unit of data that flows through a ProcessingPipeline. `data` is a
// shared_ptr so a frame can be hand off to multiple processors/subscribers
// without copying the PCM payload; a processor that needs to mutate in place
// must copy-on-write (see IAudioProcessor::process doc).
struct AudioFrame
{
  std::shared_ptr<std::vector<uint8_t>> data;

  uint32_t sample_rate = 0;
  uint8_t channels = 0;
  uint8_t sample_format = 0;  // bit depth, matches AudioStreamInfo::format (8/16/24/32)

  uint32_t frame_count = 0;  // samples per channel in this frame
  uint64_t sequence = 0;     // monotonically increasing per stream, starting at 0
  // Capture time of the FIRST sample frame in `data`, CLOCK_MONOTONIC
  // microseconds (see core/capture_timestamp.hpp). 0 = no valid timestamp:
  // AudioStreamConfig::need_timestamp was false, or the backend could not
  // produce one. Before 2026-11-26 PipeWirePulseBackend stored the chunk READ
  // DURATION here (a few us), which was never a timestamp; that value now
  // lives in read_duration_usec.
  uint64_t timestamp_usec = 0;

  // How long the backend took to read/copy this chunk, microseconds. Only a
  // diagnostic (feeds AudioServer's enable_latency_log), not a time point.
  uint64_t read_duration_usec = 0;

  uint32_t flags = static_cast<uint32_t>(FrameFlag::kNone);

  // Set by a VAD processor on each processed block; ChunkReframer gives a
  // re-assembled chunk the max over the blocks it overlaps (active if any
  // part of it is active).
  VoiceActivity voice = VoiceActivity::kUnknown;

  size_t byte_size() const { return data ? data->size() : 0; }
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_FRAME_HPP_
