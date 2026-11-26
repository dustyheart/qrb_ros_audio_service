// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PROCESSOR_FACTORY_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PROCESSOR_FACTORY_HPP_

#include <memory>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/core/audio_processor.hpp"
#include "qrb_audio_common_lib/core/audio_stream_config.hpp"
#include "qrb_audio_common_lib/core/processing_pipeline.hpp"
#include "qrb_audio_common_lib/core/result.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Names create_processor() understands:
//   passthrough  - touches nothing, flags frames kBypassed
//   test_delay   - sleeps delay_us (default 1000) per block, then passes the
//                  block through; TEST ONLY, used to prove a slow processor
//                  cannot stall capture
//   ns           - WebRTC noise suppression; level=low|moderate|high|very_high,
//                  highpass=true|false (default false)
//   vad          - WebRTC voice activity detection (sets AudioFrame::voice);
//                  likelihood=very_low|low|moderate|high (default very_low),
//                  hangover_ms=0..2000 (default 300), highpass=true|false
//                  (default false; analysis copy only)
// ns/vad exist only if the library was built with webrtc-audio-processing
// (see webrtc_processors_available()); recommended order: ns, vad.
std::vector<std::string> known_processor_names();
bool webrtc_processors_available();

std::unique_ptr<IAudioProcessor> create_processor(const std::string & name);

// Parses one processor spec: "name" or "name:key=value,key=value".
// Whitespace is not trimmed. Fails with kInvalidConfig on an empty name,
// unknown name, or a malformed key=value pair.
Result parse_processor_spec(const std::string & spec, ProcessorSlot & out);

// Builds a pipeline from `slots` (bypassed slots are skipped). Fails with
// kProcessorInitFailed if any processor's configure() fails: per
// refactoring_plan.md section 6.4 an init failure fails stream creation
// instead of silently running without that processor.
Result build_pipeline(const std::vector<ProcessorSlot> & slots,
    std::unique_ptr<ProcessingPipeline> & out);

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PROCESSOR_FACTORY_HPP_
