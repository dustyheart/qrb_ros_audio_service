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
  uint64_t timestamp_usec = 0;

  uint32_t flags = static_cast<uint32_t>(FrameFlag::kNone);

  size_t byte_size() const { return data ? data->size() : 0; }
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_FRAME_HPP_
