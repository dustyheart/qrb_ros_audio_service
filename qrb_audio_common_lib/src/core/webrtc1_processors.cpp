// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "webrtc1_processors.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "modules/audio_processing/include/audio_processing.h"
#include "qrb_audio_common_lib/audio_log.hpp"
#include "qrb_audio_common_lib/core/reference_buffer.hpp"
#include "webrtc_common.hpp"

// webrtc-audio-processing 1.x builds the WebRTC VAD (common_audio/vad) into
// the library and exports its C API, but does not install webrtc_vad.h.
// Declarations copied from webrtc/common_audio/vad/include/webrtc_vad.h
// (v1.3); a missing/renamed symbol is a link error, not silent misbehavior.
extern "C" {
typedef struct WebRtcVadInst VadInst;
VadInst * WebRtcVad_Create(void);
void WebRtcVad_Free(VadInst * handle);
int WebRtcVad_Init(VadInst * handle);
int WebRtcVad_set_mode(VadInst * handle, int mode);
int WebRtcVad_Process(VadInst * handle, int fs, const int16_t * audio_frame, size_t frame_length);
}

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

using webrtc_common::kNsLevels;
using webrtc_common::kVadLikelihoods;

Result check_webrtc_format(const ProcessingContext & c)
{
  return webrtc_common::check_format(c);
}

// ---------------------------------------------------------------------------
// WebRtc1ApmProcessor

struct WebRtc1ApmProcessor::Impl
{
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  ProcessingContext prepared;
  std::vector<int16_t> out;      // capture output scratch
  std::vector<int16_t> ref;      // render (reference) block
  std::vector<int16_t> ref_out;  // render output (unused, API needs it)
  bool ref_was_available = false;
  bool ever_available = false;
  bool start_error_logged = false;
  uint64_t drift_resyncs_seen = 0;
  uint64_t blocks = 0;
  uint64_t blocks_without_ref = 0;
};

WebRtc1ApmProcessor::WebRtc1ApmProcessor(bool aec, bool ns)
  : aec_(aec), ns_(ns), highpass_(aec), impl_(std::make_unique<Impl>())
{
}

WebRtc1ApmProcessor::~WebRtc1ApmProcessor() = default;

std::string WebRtc1ApmProcessor::name() const
{
  if (aec_ && ns_)
    return "aec+ns";
  return aec_ ? "aec" : "ns";
}

Result WebRtc1ApmProcessor::configure(const ProcessorConfig & config)
{
  Result err = Result::Ok();
  const std::string who = name();
  if (ns_ && !webrtc_common::parse_choice(config, "level", kNsLevels, 4, ns_level_, err, who))
    return err;
  bool hp = aec_;
  if (!webrtc_common::parse_bool(config, "highpass", hp, err, who))
    return err;
  // AEC3 enforces the high-pass filter itself (EchoCanceller::
  // enforce_high_pass_filtering); accept "false" but it has no effect.
  highpass_ = hp || aec_;
  if (aec_) {
    uint32_t v = 0;
    if (!webrtc_common::parse_uint(config, "reference_lead_ms", 0, 100, v, err, who))
      return err;
    if (!webrtc_common::parse_uint(config, "reference_resync_ms", 0, 1000, v, err, who))
      return err;
  }
  return Result::Ok();
}

Result WebRtc1ApmProcessor::prepare(const ProcessingContext & context)
{
  Result r = check_webrtc_format(context);
  if (!r.ok())
    return r;
  webrtc::AudioProcessing * raw = webrtc::AudioProcessingBuilder().Create();
  if (!raw)
    return Result::Error(ErrorCode::kProcessorInitFailed, name() + ": APM Create() failed");
  impl_->apm = rtc::scoped_refptr<webrtc::AudioProcessing>(raw);

  webrtc::AudioProcessing::Config cfg;
  cfg.echo_canceller.enabled = aec_;
  cfg.echo_canceller.mobile_mode = false;
  cfg.noise_suppression.enabled = ns_;
  cfg.noise_suppression.level =
      static_cast<webrtc::AudioProcessing::Config::NoiseSuppression::Level>(ns_level_);
  cfg.high_pass_filter.enabled = highpass_;
  cfg.gain_controller1.enabled = false;
  cfg.gain_controller2.enabled = false;
  impl_->apm->ApplyConfig(cfg);
  const webrtc::StreamConfig capture(static_cast<int>(context.sample_rate), context.channels);
  const webrtc::StreamConfig render(static_cast<int>(context.sample_rate), 1);
  webrtc::ProcessingConfig pc{{capture, capture, render, render}};
  const int err = impl_->apm->Initialize(pc);
  if (err != webrtc::AudioProcessing::kNoError) {
    impl_->apm = nullptr;
    return Result::Error(ErrorCode::kProcessorInitFailed,
        name() + ": APM Initialize() error " + std::to_string(err));
  }
  const size_t blk = context.sample_rate / 100;
  impl_->prepared = context;
  impl_->out.assign(blk * context.channels, 0);
  impl_->ref.assign(blk, 0);
  impl_->ref_out.assign(blk, 0);
  impl_->ref_was_available = false;
  impl_->ever_available = false;
  impl_->start_error_logged = false;
  impl_->drift_resyncs_seen = 0;
  impl_->blocks = impl_->blocks_without_ref = 0;
  if (aec_ && !context.reference) {
    LOGE(
        "%s: no echo reference source for this stream; AEC passes audio through (echo is NOT "
        "cancelled)",
        name().c_str());
  }
  return Result::Ok();
}

ProcessResult WebRtc1ApmProcessor::process(AudioFrame & frame, const ProcessingContext & context)
{
  Impl & m = *impl_;
  const ProcessingContext & p = m.prepared;
  const size_t blk = p.sample_rate / 100;
  if (!m.apm || !frame.data || frame.frame_count != blk || frame.channels != p.channels ||
      frame.data->size() != blk * p.channels * 2) {
    return ProcessResult{ProcessResultStatus::kFailed,
        Result::Error(ErrorCode::kProcessorProcessFailed, name() + ": block does not match the "
                                                                   "prepared format")};
  }
  const webrtc::StreamConfig capture(static_cast<int>(p.sample_rate), p.channels);

  bool ref_ok = true;
  if (aec_) {
    ReferenceBuffer * ref = context.reference;
    ref_ok = ref && ref->fetch(frame.timestamp_usec, has_flag(frame.flags, FrameFlag::kDropped),
                        m.ref.data(), blk);
    const webrtc::StreamConfig render(static_cast<int>(p.sample_rate), 1);
    const int rerr = m.apm->ProcessReverseStream(m.ref.data(), render, render, m.ref_out.data());
    if (rerr != webrtc::AudioProcessing::kNoError) {
      return ProcessResult{ProcessResultStatus::kFailed,
          Result::Error(ErrorCode::kProcessorProcessFailed,
              name() + ": ProcessReverseStream error " + std::to_string(rerr))};
    }
    // Log transitions. The reference stream needs a few hundred ms to deliver
    // its first chunk after start, so missing reference at start is only an
    // error once it lasts kStartGraceBlocks.
    constexpr uint64_t kStartGraceBlocks = 100;  // 1 s
    if (ref_ok && !m.ref_was_available) {
      LOGI("%s: echo reference available, AEC active", name().c_str());
    } else if (!ref_ok && context.reference) {
      if (m.ref_was_available) {
        LOGE(
            "%s: echo reference lost (stalled, disconnected or failed); AEC bypassed, audio "
            "passes through until it returns",
            name().c_str());
      } else if (!m.ever_available && !m.start_error_logged && m.blocks + 1 >= kStartGraceBlocks) {
        LOGE(
            "%s: no echo reference within 1 s of start; AEC bypassed, audio passes through "
            "until it arrives",
            name().c_str());
        m.start_error_logged = true;
      }
    }
    m.ever_available = m.ever_available || ref_ok;
    m.ref_was_available = ref_ok;
    if (ref) {
      const ReferenceBuffer::Stats rs = ref->stats();
      if (rs.drift_resyncs != m.drift_resyncs_seen) {
        // The mic and reference sample clocks disagreed with their capture
        // timestamps by more than the re-sync threshold; the pairing jumped.
        LOGI(
            "%s: echo reference re-synced at %.2f s: sample-count pairing had drifted %+.1f ms "
            "from the timestamps (%llu drift re-sync(s) so far)",
            name().c_str(), m.blocks / 100.0,
            rs.last_drift_frames * 1000.0 / static_cast<double>(p.sample_rate),
            static_cast<unsigned long long>(rs.drift_resyncs));
        m.drift_resyncs_seen = rs.drift_resyncs;
      }
    }
    // Render delay is estimated by AEC3; there is no external estimate.
    m.apm->set_stream_delay_ms(0);
  }

  const int16_t * in = reinterpret_cast<const int16_t *>(frame.data->data());
  const int err = m.apm->ProcessStream(in, capture, capture, m.out.data());
  if (err != webrtc::AudioProcessing::kNoError) {
    return ProcessResult{
        ProcessResultStatus::kFailed, Result::Error(ErrorCode::kProcessorProcessFailed,
                                          name() + ": ProcessStream error " + std::to_string(err))};
  }
  std::memcpy(frame.data->data(), m.out.data(), m.out.size() * sizeof(int16_t));
  ++m.blocks;
  if (!ref_ok) {
    // NS (if any) ran, the echo cancellation did not.
    ++m.blocks_without_ref;
    frame.flags |= static_cast<uint32_t>(FrameFlag::kBypassed);
  }
  return ProcessResult{};
}

void WebRtc1ApmProcessor::reset()
{
  if (impl_->apm)
    impl_->apm->Initialize();
  impl_->ref_was_available = false;
  impl_->ever_available = false;
  impl_->start_error_logged = false;
}

uint32_t WebRtc1ApmProcessor::apm_latency_frames(uint32_t rate, bool aec, bool ns)
{
  // White-noise cross-correlation through the APM, QCS9075, 1.3 (stage 6);
  // identical for 1 and 2 channels and with/without the high-pass filter
  // whenever AEC or NS is on. core_webrtc_processors_test re-measures it.
  struct Row
  {
    uint32_t rate, aec, ns, both;
  };
  static const Row kRows[] = {
      {8000, 88, 72, 136},
      {16000, 128, 96, 224},
      {32000, 260, 196, 452},
      {48000, 430, 334, 718},
  };
  for (const auto & row : kRows) {
    if (row.rate == rate)
      return aec && ns ? row.both : aec ? row.aec : ns ? row.ns : 0;
  }
  return 0;
}

uint32_t WebRtc1ApmProcessor::latency_frames() const
{
  return impl_->apm ? apm_latency_frames(impl_->prepared.sample_rate, aec_, ns_) : 0;
}

std::string WebRtc1ApmProcessor::diagnostics() const
{
  const Impl & m = *impl_;
  if (!m.apm)
    return std::string();
  char buf[512];
  int n = std::snprintf(
      buf, sizeof(buf), "%" PRIu64 " block(s), latency %u frames", m.blocks, latency_frames());
  if (aec_) {
    // AEC3's own GetStatistics() echo metrics (delay/ERL/ERLE) stayed at
    // defaults on QCS9075 with 1.3 even with has_remote_tracks=true, so they
    // are not logged; ERLE is measured externally (test_report 5k).
    n += std::snprintf(buf + n, sizeof(buf) - n, "; reference missing for %" PRIu64 " block(s)",
        m.blocks_without_ref);
    if (impl_->prepared.reference) {
      const ReferenceBuffer::Stats rs = impl_->prepared.reference->stats();
      std::snprintf(buf + n, sizeof(buf) - n,
          "; reference: %" PRIu64 " frame(s) in, %" PRIu64 " block(s) paired, %" PRIu64
          " unavailable, %" PRIu64 " sync(s) (%" PRIu64 " drift), %" PRIu64 " wait(s), %" PRIu64
          " gap(s), last sync offset %" PRId64 " frames",
          rs.frames_pushed, rs.blocks_fetched, rs.blocks_unavailable, rs.syncs, rs.drift_resyncs,
          rs.waits, rs.pushes_with_gap, rs.last_sync_offset_frames);
    }
  }
  return buf;
}

// ---------------------------------------------------------------------------
// WebRtc1VadProcessor

struct WebRtc1VadProcessor::Impl
{
  VadInst * vad = nullptr;
  rtc::scoped_refptr<webrtc::AudioProcessing> hpf;  // high-pass only, analysis copy
  ProcessingContext prepared;
  std::vector<int16_t> mono;
  ~Impl()
  {
    if (vad)
      WebRtcVad_Free(vad);
  }
};

WebRtc1VadProcessor::WebRtc1VadProcessor() : impl_(std::make_unique<Impl>()) {}
WebRtc1VadProcessor::~WebRtc1VadProcessor() = default;

Result WebRtc1VadProcessor::configure(const ProcessorConfig & config)
{
  Result err = Result::Ok();
  if (!webrtc_common::parse_choice(
          config, "likelihood", kVadLikelihoods, 4, likelihood_, err, "vad"))
    return err;
  uint32_t hang = 300;
  if (config.count("hangover_ms")) {
    if (!webrtc_common::parse_uint(config, "hangover_ms", 0, 2000, hang, err, "vad"))
      return err;
    hangover_.set_hangover_ms(hang);
  }
  highpass_ = false;
  if (!webrtc_common::parse_bool(config, "highpass", highpass_, err, "vad"))
    return err;
  return Result::Ok();
}

Result WebRtc1VadProcessor::prepare(const ProcessingContext & context)
{
  Result r = check_webrtc_format(context);
  if (!r.ok())
    return r;
  if (impl_->vad)
    WebRtcVad_Free(impl_->vad);
  impl_->vad = WebRtcVad_Create();
  if (!impl_->vad || WebRtcVad_Init(impl_->vad) != 0 ||
      WebRtcVad_set_mode(impl_->vad, 3 - likelihood_) != 0) {
    return Result::Error(ErrorCode::kProcessorInitFailed, "vad: WebRtcVad init failed");
  }
  impl_->hpf = nullptr;
  if (highpass_) {
    webrtc::AudioProcessing * raw = webrtc::AudioProcessingBuilder().Create();
    if (!raw)
      return Result::Error(ErrorCode::kProcessorInitFailed, "vad: high-pass APM Create() failed");
    impl_->hpf = rtc::scoped_refptr<webrtc::AudioProcessing>(raw);
    webrtc::AudioProcessing::Config cfg;
    cfg.high_pass_filter.enabled = true;
    impl_->hpf->ApplyConfig(cfg);
  }
  impl_->prepared = context;
  impl_->mono.assign(context.sample_rate / 100, 0);
  hangover_.reset();
  return Result::Ok();
}

ProcessResult WebRtc1VadProcessor::process(AudioFrame & frame, const ProcessingContext &)
{
  Impl & m = *impl_;
  const ProcessingContext & p = m.prepared;
  const size_t blk = p.sample_rate / 100;
  if (!m.vad || !frame.data || frame.frame_count != blk || frame.channels != p.channels ||
      frame.data->size() != blk * p.channels * 2) {
    return ProcessResult{
        ProcessResultStatus::kFailed, Result::Error(ErrorCode::kProcessorProcessFailed,
                                          "vad: block does not match the prepared format")};
  }
  // Analysis copy only: `frame` audio is never written.
  const int16_t * in = reinterpret_cast<const int16_t *>(frame.data->data());
  if (p.channels == 1) {
    std::memcpy(m.mono.data(), in, blk * sizeof(int16_t));
  } else {
    for (size_t i = 0; i < blk; ++i)
      m.mono[i] = static_cast<int16_t>((static_cast<int32_t>(in[2 * i]) + in[2 * i + 1]) / 2);
  }
  if (m.hpf) {
    const webrtc::StreamConfig sc(static_cast<int>(p.sample_rate), 1);
    if (m.hpf->ProcessStream(m.mono.data(), sc, sc, m.mono.data()) !=
        webrtc::AudioProcessing::kNoError) {
      return ProcessResult{ProcessResultStatus::kFailed,
          Result::Error(ErrorCode::kProcessorProcessFailed, "vad: high-pass ProcessStream error")};
    }
  }
  const int v = WebRtcVad_Process(m.vad, static_cast<int>(p.sample_rate), m.mono.data(), blk);
  if (v < 0) {
    return ProcessResult{ProcessResultStatus::kFailed,
        Result::Error(ErrorCode::kProcessorProcessFailed, "vad: WebRtcVad_Process error")};
  }
  frame.voice = hangover_.update(v == 1, 10) ? VoiceActivity::kActive : VoiceActivity::kInactive;
  return ProcessResult{};
}

void WebRtc1VadProcessor::reset()
{
  if (impl_->vad) {
    WebRtcVad_Init(impl_->vad);
    WebRtcVad_set_mode(impl_->vad, 3 - likelihood_);
  }
  if (impl_->hpf)
    impl_->hpf->Initialize();
  hangover_.reset();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
