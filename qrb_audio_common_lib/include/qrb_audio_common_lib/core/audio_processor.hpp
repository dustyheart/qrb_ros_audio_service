// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_PROCESSOR_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_PROCESSOR_HPP_

#include <cstdint>
#include <map>
#include <string>

#include "qrb_audio_common_lib/core/audio_frame.hpp"
#include "qrb_audio_common_lib/core/result.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Opaque per-processor settings. Concrete processors (NS/VAD/AEC) define
// their own typed config struct; this string map is only what crosses the
// ProcessingPipeline boundary before a processor parses it into its own type,
// mirroring how AudioStreamConfig::ProcessorSlot stays generic. Keeps the
// pipeline from depending on every processor's config type.
using ProcessorConfig = std::map<std::string, std::string>;

enum class ProcessResultStatus
{
  kProcessed,   // frame was modified in place
  kBypassed,    // processor chose not to touch this frame (disabled/degraded)
  kFailed,      // processing failed; frame is unchanged, caller decides bypass/stop
};

struct ProcessResult
{
  ProcessResultStatus status = ProcessResultStatus::kProcessed;
  Result result = Result::Ok();  // set when status == kFailed
};

// Context passed alongside a frame so a processor can see stream-level facts
// (e.g. an AEC needs frame_duration_ms and sample_rate to size its internal
// buffers) without taking a dependency on AudioStreamConfig itself.
struct ProcessingContext
{
  uint32_t sample_rate = 0;
  uint8_t channels = 0;
  uint32_t frame_duration_ms = 0;
};

// A single stage in a ProcessingPipeline (NS, VAD, AEC, or a test double).
// Implementations MUST be real-time safe in process(): no blocking I/O, no
// locking against anything the audio capture thread might also need, and
// ideally no dynamic allocation on the steady-state path. See
// refactoring_plan.md section 3.1 (ROS interface layer threading) and 6.4
// (processor failure policy) for the constraints this interface exists to
// satisfy. process() runs on the capture/render callback thread (same thread
// as the PulseAudio stream callbacks), per the current threading decision -
// revisit this doc if that changes.
class IAudioProcessor
{
public:
  virtual ~IAudioProcessor() = default;

  virtual std::string name() const = 0;

  // Called once before the first process() call, and again whenever the
  // pipeline reconfigures this slot (e.g. sample_rate changed, which usually
  // requires a full stream rebuild rather than a live reconfigure - see
  // refactoring_plan.md section 5.3 on which fields support hot-reconfigure).
  virtual Result configure(const ProcessorConfig & config) = 0;

  // Mutates `frame` in place when status is kProcessed. Must not change
  // frame.sample_rate/channels/sample_format, frame.sequence or
  // frame.timestamp_usec - only frame.data and frame.flags are the
  // processor's to touch. Must return promptly: no sleeping, no blocking
  // locks, no file/network I/O.
  virtual ProcessResult process(AudioFrame & frame, const ProcessingContext & context) = 0;

  // Clears internal state (adaptive filter history, VAD hangover counters,
  // etc.) without requiring a full configure() + object recreation. Called
  // when a stream restarts without a full teardown.
  virtual void reset() = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_PROCESSOR_HPP_
