// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_CAPTURE_BACKEND_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_CAPTURE_BACKEND_HPP_

#include <functional>

#include "qrb_audio_common_lib/core/audio_frame.hpp"
#include "qrb_audio_common_lib/core/audio_status.hpp"
#include "qrb_audio_common_lib/core/audio_stream_config.hpp"
#include "qrb_audio_common_lib/core/result.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

using FrameCallback = std::function<void(const AudioFrame &)>;

// Backend-agnostic capture interface, per refactoring_plan.md section 3.3.
// A concrete implementation owns exactly one stream: configure() then
// start() once; destroying the object implies stop(), callers do not need a
// separate close(). FrameCallback is invoked on whatever thread the backend's
// own data-ready callback runs on (for PipeWirePulseBackend, that is the
// PulseAudio mainloop thread) - same real-time constraints as
// IAudioProcessor::process() apply to the callback body.
class IAudioCaptureBackend
{
public:
  virtual ~IAudioCaptureBackend() = default;

  virtual Result configure(const AudioStreamConfig & config) = 0;
  virtual Result start(FrameCallback callback) = 0;
  virtual Result stop() = 0;
  virtual AudioBackendStatus status() const = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_CAPTURE_BACKEND_HPP_
