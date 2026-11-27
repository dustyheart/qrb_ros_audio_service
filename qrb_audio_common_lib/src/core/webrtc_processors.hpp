// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Internal header (not installed): the LEGACY implementation on the system
// webrtc-audio-processing 0.3.1 (QRB_AUDIO_WEBRTC_IMPL=legacy, or auto when
// 1.x is not found). Users get these through core::create_processor("ns" /
// "vad"). With webrtc-audio-processing 1.x, webrtc1_processors.* replaces this
// file entirely (the two libraries export ~400 identical symbols and must
// never be linked into one process).

#ifndef QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_PROCESSORS_HPP_
#define QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_PROCESSORS_HPP_

#include <memory>
#include <string>

#include "qrb_audio_common_lib/core/audio_processor.hpp"
#include "qrb_audio_common_lib/core/vad_hangover.hpp"

namespace webrtc
{
class AudioProcessing;
class AudioFrame;
}  // namespace webrtc

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Shared format rules of webrtc-audio-processing 0.3.1 (system package on
// Ubuntu 24.04): int16 interleaved, 8/16/32/48 kHz, exactly 10 ms per call,
// 1 or 2 channels (what this wrapper supports and was tested with).
Result check_webrtc_format(const ProcessingContext & context);

// Owns one webrtc::AudioProcessing with only the wanted component enabled.
class WebRtcApmStage : public IAudioProcessor
{
public:
  ~WebRtcApmStage() override;
  Result prepare(const ProcessingContext & context) override;
  void reset() override;

protected:
  WebRtcApmStage();
  // Enables this stage's component on a freshly created APM.
  virtual Result enable_component(webrtc::AudioProcessing & apm) = 0;
  // Copies `frame` into the APM frame, runs ProcessStream. On error returns
  // kFailed without touching `frame`.
  ProcessResult run(AudioFrame & frame, const ProcessingContext & context, bool write_back);

  // Parses "highpass" = true|false into highpass_ (default given by caller).
  Result parse_highpass(const ProcessorConfig & config, bool default_value);

  std::unique_ptr<webrtc::AudioProcessing> apm_;
  std::unique_ptr<webrtc::AudioFrame> apm_frame_;
  ProcessingContext prepared_;
  // APM's 80 Hz high-pass filter (removes DC offset / rumble) in front of
  // this stage's component.
  bool highpass_ = false;
};

// "ns": WebRTC noise suppression. Config: level = low | moderate | high |
// very_high (default moderate); highpass = true | false (default false: the
// 80 Hz high-pass would change the delivered audio, so it is opt-in).
// Modifies the audio.
class WebRtcNsProcessor : public WebRtcApmStage
{
public:
  std::string name() const override { return "ns"; }
  Result configure(const ProcessorConfig & config) override;
  ProcessResult process(AudioFrame & frame, const ProcessingContext & context) override;
  uint32_t latency_frames() const override;

private:
  Result enable_component(webrtc::AudioProcessing & apm) override;
  int level_ = 1;  // webrtc::NoiseSuppression::Level
};

// "vad": WebRTC voice activity detection (APM VoiceDetection, 10 ms
// decisions) + hangover smoothing. Config: likelihood = very_low | low |
// moderate | high (default very_low; higher = more readily reports voice),
// hangover_ms = 0..2000 (default 300), highpass = true | false (default
// false). Defaults come from QCS9075 measurements (test_report 5j): with
// moderate, the idle board mic was reported as voice 48-88 % of the time,
// with very_low 3 %, while clean speech recall stayed 97-100 %; the 80 Hz
// high-pass made idle-mic false positives worse (56 -> 72 %).
// Never modifies the audio; sets frame.voice.
class WebRtcVadProcessor : public WebRtcApmStage
{
public:
  std::string name() const override { return "vad"; }
  Result configure(const ProcessorConfig & config) override;
  ProcessResult process(AudioFrame & frame, const ProcessingContext & context) override;
  void reset() override;

private:
  Result enable_component(webrtc::AudioProcessing & apm) override;
  int likelihood_ = 0;  // webrtc::VoiceDetection::Likelihood, kVeryLowLikelihood
  VadHangover hangover_{300};
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__SRC__CORE__WEBRTC_PROCESSORS_HPP_
