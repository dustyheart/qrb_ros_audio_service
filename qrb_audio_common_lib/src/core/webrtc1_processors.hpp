// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Internal header (not installed): WebRTC processors on webrtc-audio-processing
// 1.x (AEC3), compiled when CMake selected QRB_AUDIO_WEBRTC_IMPL=v1 (default
// when webrtc-audio-processing-1 is found). Replaces webrtc_processors.* (the
// 0.3.1 implementation) entirely; the two must never be linked into one
// process - the libraries export ~400 identical symbols (aec3_ab_test.md).
// Users get these through core::create_processor() / build_pipeline().

#ifndef QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC1_PROCESSORS_HPP_
#define QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC1_PROCESSORS_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/core/audio_processor.hpp"
#include "qrb_audio_common_lib/core/vad_hangover.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

Result check_webrtc_format(const ProcessingContext & context);

// One webrtc::AudioProcessing running AEC3 and/or NS (in that order, inside
// the APM), so an "aec, ns" chain converts the block once and the NS sees the
// echo-cancelled signal. Names: "aec", "ns", or "aec+ns" when both.
//
// Config keys (all optional):
//   ns:  level = low | moderate | high | very_high (default moderate)
//   aec: reference_device (consumed by AudioManager), reference_lead_ms =
//        0..100, reference_resync_ms = 0..1000 (0 = no drift re-sync)
//        (both consumed by ProcessingCaptureBackend; validated here)
//   highpass = true | false: APM 80 Hz high-pass. Default false for NS alone
//        (stage-5 behavior); with AEC it is always on (AEC3 enforces it).
//
// AEC reference: ProcessingContext::reference. Every block feeds the APM a
// render block - the paired reference, or zeros when the reference is
// unavailable (no source, stalled, failed, no timestamps yet). With zeros
// AEC3 cancels nothing but keeps the APM running, so the output latency and
// the NS stay unchanged; such blocks are flagged kBypassed and the
// available <-> unavailable transitions are logged.
class WebRtc1ApmProcessor : public IAudioProcessor
{
public:
  WebRtc1ApmProcessor(bool aec, bool ns);
  ~WebRtc1ApmProcessor() override;

  std::string name() const override;
  Result configure(const ProcessorConfig & config) override;
  Result prepare(const ProcessingContext & context) override;
  ProcessResult process(AudioFrame & frame, const ProcessingContext & context) override;
  void reset() override;
  uint32_t latency_frames() const override;
  bool needs_reference() const override { return aec_; }
  std::string diagnostics() const override;

  // Fixed APM input->output delay (sample frames), measured on QCS9075 with
  // webrtc-audio-processing 1.3 (stage 6). 0 for an unsupported rate.
  static uint32_t apm_latency_frames(uint32_t rate, bool aec, bool ns);

private:
  struct Impl;
  const bool aec_;
  const bool ns_;
  int ns_level_ = 1;
  bool highpass_ = false;
  std::unique_ptr<Impl> impl_;
};

// "vad": WebRTC VAD (the same detector 0.3.1's VoiceDetection wrapped),
// called directly through the C functions webrtc-audio-processing 1.x
// exports, because 1.x's APM VoiceDetection lost its likelihood setting.
// Config as before: likelihood = very_low | low | moderate | high (default
// very_low; VAD mode = 3 - index, the 0.3.1 mapping), hangover_ms = 0..2000
// (default 300), highpass = true | false (default false; filters the analysis
// copy only). Stereo is averaged to mono. Never modifies the audio.
class WebRtc1VadProcessor : public IAudioProcessor
{
public:
  WebRtc1VadProcessor();
  ~WebRtc1VadProcessor() override;

  std::string name() const override { return "vad"; }
  Result configure(const ProcessorConfig & config) override;
  Result prepare(const ProcessingContext & context) override;
  ProcessResult process(AudioFrame & frame, const ProcessingContext & context) override;
  void reset() override;

private:
  struct Impl;
  int likelihood_ = 0;
  bool highpass_ = false;
  VadHangover hangover_{300};
  std::unique_ptr<Impl> impl_;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC1_PROCESSORS_HPP_
