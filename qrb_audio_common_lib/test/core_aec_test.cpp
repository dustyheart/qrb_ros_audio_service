// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// "aec" (WebRTC AEC3, webrtc-audio-processing 1.x) end to end through
// ProcessingCaptureBackend with scripted mic + reference backends, offline:
//  - echo = far-end delayed by 20 / 100 / 200 ms: steady ERLE >= 25 dB,
//    with mic chunks of 20 ms and reference chunks of 10 ms on one clock;
//  - "aec, ns" runs as one stage "aec+ns" with the measured latency;
//  - timestamps: mic backend forced to need_timestamp=true, delivered
//    stamps moved back by the APM latency, or stripped when not requested;
//  - no reference backend / reference that fails to configure / reference
//    stalling mid-stream: every chunk still delivered, blocks without a
//    reference flagged kBypassed, AEC back to >= 20 dB after the reference
//    resumes;
//  - double talk: the near-end talker survives (loose bound, digital echo).
// Only built with QRB_AUDIO_WEBRTC=v1.

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/core/processing_capture_backend.hpp"
#include "qrb_audio_common_lib/core/processor_factory.hpp"

using namespace qrb::audio_common_lib::core;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

namespace
{

constexpr uint64_t kT0 = 7'000'000'000ULL;  // monotonic origin, us
constexpr double kPi = 3.14159265358979323846;

// Records the config it was given and forwards scripted chunks.
class ScriptedBackend : public IAudioCaptureBackend
{
public:
  explicit ScriptedBackend(bool fail_configure = false) : fail_configure_(fail_configure) {}
  Result configure(const AudioStreamConfig & c) override
  {
    if (fail_configure_)
      return Result::Error(ErrorCode::kDeviceUnavailable, "scripted: no such source");
    config = c;
    configured = true;
    return Result::Ok();
  }
  Result start(FrameCallback cb) override
  {
    std::lock_guard<std::mutex> l(m_);
    cb_ = std::move(cb);
    return Result::Ok();
  }
  Result stop() override
  {
    std::lock_guard<std::mutex> l(m_);
    cb_ = nullptr;
    return Result::Ok();
  }
  AudioBackendStatus status() const override { return AudioBackendStatus::kReady; }
  void emit(const int16_t * s, size_t n, uint32_t rate, uint64_t ts, uint64_t seq)
  {
    AudioFrame f;
    f.sample_rate = rate;
    f.channels = 1;
    f.sample_format = 16;
    f.frame_count = static_cast<uint32_t>(n);
    f.sequence = seq;
    f.timestamp_usec = ts;
    f.data = std::make_shared<std::vector<uint8_t>>(n * 2);
    std::memcpy(f.data->data(), s, n * 2);
    std::lock_guard<std::mutex> l(m_);
    if (cb_)
      cb_(f);
  }
  AudioStreamConfig config;
  bool configured = false;

private:
  bool fail_configure_;
  std::mutex m_;
  FrameCallback cb_;
};

// Speech-like far-end: coloured noise switched on/off at a ~3.5 Hz syllable
// rate, after 1 s of silence. Both matter for AEC3 (measured on QCS9075,
// stage 6): with a smooth, never-silent envelope its render-stationarity
// logic keeps the residual-echo suppressor mild (ERLE ~20-25 dB instead of
// 60-70 dB), and with render already active in the very first block ERLE
// stays ~30 dB while 0.5 s of leading silence gives ~69 dB.
std::vector<int16_t> talker(size_t n, uint32_t rate, uint32_t seed, double amp, double smooth = 0.9)
{
  std::vector<int16_t> v(n, 0);
  uint32_t x = seed;
  double lp = 0;
  for (size_t i = rate; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    const double w = (x >> 8) / double(1u << 24) - 0.5;
    lp = smooth * lp + (1 - smooth) * w;
    const double t = double(i) / rate;
    const double env = std::sin(2 * kPi * 3.5 * t + seed) > 0 ? 1.0 : 0.0;
    v[i] = static_cast<int16_t>(std::lround(std::max(-32767.0, std::min(32767.0, lp * amp * env))));
  }
  return v;
}

double power_db(const std::vector<int16_t> & s, size_t a, size_t b)
{
  double e = 0;
  b = std::min(b, s.size());
  for (size_t i = a; i < b; ++i)
    e += double(s[i]) * s[i];
  return 10 * std::log10(e / double(b > a ? b - a : 1) + 1e-9);
}

struct RunResult
{
  std::vector<int16_t> out;        // delivered PCM, concatenated
  std::vector<uint64_t> stamps;    // per delivered chunk
  std::vector<uint32_t> flags;     // per delivered chunk
  std::vector<std::string> names;  // processor names
  uint64_t latency_usec = 0;
  bool mic_need_ts = false;
  size_t chunks = 0;
  std::string diagnostics;
};

enum class RefMode
{
  kNormal,
  kNoBackend,
  kConfigureFails,
  kStall,  // reference missing for [stall_from, stall_to) seconds, then gap-flagged
};

// mic[n] = gain * far[n - delay] + near[n]. Reference = far on the same
// clock. Mic in 20 ms chunks, reference in 10 ms chunks pushed just before
// the mic chunk that covers them (Pulse-like, reference slightly ahead).
RunResult run(uint32_t rate,
    uint32_t delay_ms,
    const std::vector<std::string> & chain,
    const std::vector<int16_t> & far,
    const std::vector<int16_t> & near,
    bool need_ts,
    RefMode mode = RefMode::kNormal,
    double stall_from = 0,
    double stall_to = 0)
{
  const size_t n = far.size();
  const size_t d = static_cast<size_t>(rate) * delay_ms / 1000;
  std::vector<int16_t> mic(n);
  for (size_t i = 0; i < n; ++i) {
    const double e = i >= d ? 0.5 * far[i - d] : 0.0;
    mic[i] = static_cast<int16_t>(std::lround(e + near[i]));
  }

  std::vector<ProcessorSlot> slots(chain.size());
  for (size_t i = 0; i < chain.size(); ++i)
    CHECK(parse_processor_spec(chain[i], slots[i]).ok());
  std::unique_ptr<ProcessingPipeline> pl;
  CHECK(build_pipeline(slots, pl).ok());

  auto mic_be = std::make_unique<ScriptedBackend>();
  ScriptedBackend * mic_raw = mic_be.get();
  std::unique_ptr<ScriptedBackend> ref_be;
  if (mode != RefMode::kNoBackend)
    ref_be = std::make_unique<ScriptedBackend>(mode == RefMode::kConfigureFails);
  // A reference whose configure() fails is destroyed by the backend; never
  // touch it afterwards.
  ScriptedBackend * ref_raw = mode == RefMode::kConfigureFails ? nullptr : ref_be.get();

  ProcessingCaptureBackend b(std::move(mic_be), std::move(pl), 1 << 20, std::move(ref_be));
  AudioStreamConfig c;
  c.sample_rate = rate;
  c.channels = 1;
  c.sample_format = 16;
  c.frame_duration_ms = 10;
  c.need_timestamp = need_ts;
  c.reference_lead_ms = 5;
  CHECK(b.configure(c).ok());

  RunResult r;
  r.mic_need_ts = mic_raw->config.need_timestamp;
  r.latency_usec = b.latency_compensation_usec();
  std::mutex m;
  std::condition_variable cv;
  CHECK(
      b.start([&](const AudioFrame & f) {
         std::lock_guard<std::mutex> l(m);
         const int16_t * s = reinterpret_cast<const int16_t *>(f.data->data());
         r.out.insert(r.out.end(), s, s + f.frame_count);
         r.stamps.push_back(f.timestamp_usec);
         r.flags.push_back(f.flags);
         ++r.chunks;
         cv.notify_all();
       }).ok());

  const size_t mic_chunk = rate / 50, ref_chunk = rate / 100;
  size_t ref_pos = 0;
  uint64_t seq = 0;
  for (size_t pos = 0; pos + mic_chunk <= n; pos += mic_chunk) {
    // Reference up to the end of this mic chunk (+ one chunk ahead).
    const size_t want = std::min(n, pos + 2 * mic_chunk);
    for (; ref_pos + ref_chunk <= want; ref_pos += ref_chunk) {
      const double t = double(ref_pos) / rate;
      if (mode == RefMode::kStall && t >= stall_from && t < stall_to)
        continue;  // reference delivers nothing; resumes unflagged (drift re-sync)
      if (ref_raw && ref_raw->configured)
        ref_raw->emit(&far[ref_pos], ref_chunk, rate, kT0 + ref_pos * 1000000ULL / rate, 0);
    }
    mic_raw->emit(&mic[pos], mic_chunk, rate, kT0 + pos * 1000000ULL / rate, seq++);
    // Keep the worker in step so the 2 s reference ring is never lapped.
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return r.chunks >= seq; });
  }
  CHECK(b.stop().ok());
  const auto st = b.stats();
  for (const auto & p : st.processors) {
    r.names.push_back(p.name);
    r.diagnostics += p.name + ": " + p.diagnostics + "\n";
  }
  CHECK(r.chunks == seq);  // nothing lost, whatever the reference did
  return r;
}

void test_erle_over_delays()
{
  for (uint32_t rate : {16000u, 48000u}) {
    for (uint32_t delay : {20u, 100u, 200u}) {
      if (rate == 48000 && delay != 100)
        continue;  // keep runtime down; 48 kHz once
      const size_t n = rate * 12;
      auto far = talker(n, rate, 3, 60000);
      std::vector<int16_t> near(n, 0);
      RunResult r = run(rate, delay, {"aec"}, far, near, true);
      CHECK(r.names.size() == 1 && r.names[0] == "aec");
      // Steady state: last 4 s. Output lags input by the APM latency, so
      // compare windows shifted by it (power over 4 s barely cares).
      const size_t lat = static_cast<size_t>(r.latency_usec * rate / 1000000);
      std::vector<int16_t> mic(n);
      const size_t d = rate * delay / 1000;
      for (size_t i = 0; i < n; ++i)
        mic[i] = static_cast<int16_t>(i >= d ? std::lround(0.5 * far[i - d]) : 0);
      const double erle = power_db(mic, n - 4 * rate, n - rate) -
                          power_db(r.out, n - 4 * rate + lat, n - rate + lat);
      size_t bypassed = 0;
      for (auto f : r.flags)
        bypassed += has_flag(f, FrameFlag::kBypassed);
      std::printf(
          "  aec @%u Hz, echo delay %u ms: steady ERLE %.1f dB, %zu/%zu chunk(s) "
          "flagged bypassed\n    %s",
          rate, delay, erle, bypassed, r.flags.size(), r.diagnostics.c_str());
      CHECK(erle >= 25.0);
      CHECK(bypassed <= 2);  // at most the very first chunk(s) before the first reference
    }
  }
  std::printf("PASS test_erle_over_delays\n");
}

void test_combined_stage_and_timestamps()
{
  const uint32_t rate = 16000;
  const size_t n = rate * 3;
  auto far = talker(n, rate, 5, 60000);
  std::vector<int16_t> near(n, 0);
  RunResult r = run(rate, 60, {"aec", "ns:level=moderate", "vad"}, far, near, true);
  CHECK(r.names.size() == 2 && r.names[0] == "aec+ns" && r.names[1] == "vad");
  CHECK(r.mic_need_ts);
  CHECK(r.latency_usec == 224ULL * 1000000 / rate);  // 14 ms, aec+ns @16 kHz
  for (size_t k = 0; k < r.stamps.size(); ++k)
    CHECK(r.stamps[k] == kT0 + k * 20000 - r.latency_usec);

  // Not requested: stripped although the mic backend had to produce them.
  RunResult r2 = run(rate, 60, {"aec"}, far, near, false);
  CHECK(r2.mic_need_ts);
  for (auto s : r2.stamps)
    CHECK(s == 0);
  std::printf("PASS test_combined_stage_and_timestamps (latency %llu us)\n",
      static_cast<unsigned long long>(r.latency_usec));
}

void test_reference_missing_or_broken_never_blocks_capture()
{
  const uint32_t rate = 16000;
  const size_t n = rate * 4;
  auto far = talker(n, rate, 9, 60000);
  std::vector<int16_t> near(n, 0);
  for (RefMode mode : {RefMode::kNoBackend, RefMode::kConfigureFails}) {
    RunResult r = run(rate, 50, {"aec", "ns"}, far, near, true, mode);
    size_t bypassed = 0;
    for (auto f : r.flags)
      bypassed += has_flag(f, FrameFlag::kBypassed);
    CHECK(bypassed == r.flags.size());  // AEC never ran, said so on every chunk
    CHECK(r.out.size() == n - n % (rate / 50));
    std::printf("  reference %s: %zu chunk(s) delivered, all flagged bypassed\n",
        mode == RefMode::kNoBackend ? "backend absent" : "configure failed", r.chunks);
  }
  std::printf("PASS test_reference_missing_or_broken_never_blocks_capture\n");
}

void test_reference_stall_and_recovery()
{
  const uint32_t rate = 16000;
  const size_t n = rate * 16;
  auto far = talker(n, rate, 11, 60000);
  std::vector<int16_t> near(n, 0);
  RunResult r = run(rate, 80, {"aec"}, far, near, true, RefMode::kStall, 5.0, 7.0);
  const size_t lat = static_cast<size_t>(r.latency_usec * rate / 1000000);
  std::vector<int16_t> mic(n);
  const size_t d = rate * 80 / 1000;
  for (size_t i = 0; i < n; ++i)
    mic[i] = static_cast<int16_t>(i >= d ? std::lround(0.5 * far[i - d]) : 0);
  auto erle = [&](double a, double b) {
    return power_db(mic, size_t(a * rate), size_t(b * rate)) -
           power_db(r.out, size_t(a * rate) + lat, size_t(b * rate) + lat);
  };
  size_t bypassed_in_stall = 0, bypassed_after = 0, chunks_in_stall = 0;
  for (size_t k = 0; k < r.flags.size(); ++k) {
    const double t = k * 0.02;
    const bool byp = has_flag(r.flags[k], FrameFlag::kBypassed);
    if (t >= 5.1 && t < 6.9) {
      ++chunks_in_stall;
      bypassed_in_stall += byp;
    } else if (t >= 7.5 && t < 15.9) {  // the last block's lead reaches past the signal end
      bypassed_after += byp;
      if (byp && bypassed_after <= 5)
        std::printf("  bypassed after the stall: chunk %zu (t=%.2f s)\n", k, t);
    }
  }
  const double before = erle(3, 5), during = erle(5.2, 6.8), after = erle(12, 15);
  std::printf(
      "  stall 5-7 s: ERLE before %.1f dB, during %.1f dB, after %.1f dB; bypassed "
      "%zu/%zu chunk(s) in the stall, %zu after 7.5 s\n    %s",
      before, during, after, bypassed_in_stall, chunks_in_stall, bypassed_after,
      r.diagnostics.c_str());
  CHECK(before >= 25.0);
  CHECK(bypassed_in_stall == chunks_in_stall);
  CHECK(bypassed_after == 0);
  CHECK(after >= 20.0);
  std::printf("PASS test_reference_stall_and_recovery\n");
}

void test_double_talk_keeps_near_end()
{
  const uint32_t rate = 16000;
  const size_t n = rate * 12;
  auto far = talker(n, rate, 13, 60000);
  // Near-end talker only in 8..11 s, broadband (so the AEC3-enforced 80 Hz
  // high-pass does not dominate the projection), about as loud as the echo.
  auto nt = talker(n, rate, 21, 9000, 0.3);
  std::vector<int16_t> near(n, 0);
  for (size_t i = 8 * rate; i < 11 * rate; ++i)
    near[i] = nt[i];
  RunResult r = run(rate, 100, {"aec"}, far, near, true);
  const size_t lat = static_cast<size_t>(r.latency_usec * rate / 1000000);
  // Least-squares gain of the near-end in the output during double talk.
  double num = 0, den = 0;
  for (size_t i = 8 * rate + rate / 2; i < 11 * rate; ++i) {
    num += double(r.out[i + lat]) * near[i];
    den += double(near[i]) * near[i];
  }
  const double gain_db = 20 * std::log10(std::fabs(num / den) + 1e-12);
  std::printf("  double talk: near-end gain %.1f dB\n", gain_db);
  CHECK(gain_db > -12.0);
  std::printf("PASS test_double_talk_keeps_near_end\n");
}

void test_chain_order_and_availability()
{
  CHECK(webrtc_aec_available());
  std::vector<ProcessorSlot> slots(2);
  CHECK(parse_processor_spec("ns", slots[0]).ok());
  CHECK(parse_processor_spec("aec", slots[1]).ok());
  std::unique_ptr<ProcessingPipeline> pl;
  Result bad = build_pipeline(slots, pl);
  CHECK(!bad.ok() && bad.code == ErrorCode::kInvalidConfig);
  CHECK(bad.message.find("aec, ns, vad") != std::string::npos);
  ProcessorSlot s;
  CHECK(parse_processor_spec("aec:reference_lead_ms=101", s).ok());
  std::vector<ProcessorSlot> one{s};
  CHECK(build_pipeline(one, pl).code == ErrorCode::kProcessorInitFailed);
  // Conflicting key on the shared APM.
  std::vector<ProcessorSlot> two(2);
  CHECK(parse_processor_spec("aec:level=high", two[0]).ok());
  CHECK(parse_processor_spec("ns:level=low", two[1]).ok());
  CHECK(!build_pipeline(two, pl).ok());
  // aec not adjacent to ns: two separate APM stages, still valid.
  std::vector<ProcessorSlot> three(3);
  CHECK(parse_processor_spec("aec", three[0]).ok());
  CHECK(parse_processor_spec("passthrough", three[1]).ok());
  CHECK(parse_processor_spec("ns", three[2]).ok());
  CHECK(build_pipeline(three, pl).ok());
  CHECK(pl->processor_count() == 3 && pl->needs_reference());
  std::printf("PASS test_chain_order_and_availability\n");
}

}  // namespace

int main()
{
  test_chain_order_and_availability();
  test_combined_stage_and_timestamps();
  test_erle_over_delays();
  test_reference_missing_or_broken_never_blocks_capture();
  test_reference_stall_and_recovery();
  test_double_talk_keeps_near_end();
  std::printf("all core_aec_test cases passed\n");
  return 0;
}
