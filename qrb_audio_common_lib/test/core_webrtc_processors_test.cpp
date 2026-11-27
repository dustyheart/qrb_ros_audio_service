// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// WebRTC "ns"/"vad" processors - whichever implementation the library was
// built with (webrtc-audio-processing 1.x, or the system 0.3.1): config
// validation, format guard (bypass, never crash), NS noise reduction, VAD on
// silence and on real speech, VAD never touches PCM, VAD failure never blocks
// PCM, VAD decisions reaching re-assembled chunks through
// ProcessingCaptureBackend, and latency_frames() equal to the delay actually
// measured through the APM (stage 6). Only built when a library is found.
//
// Real speech: /usr/share/sounds/alsa/Front_Center.wav (alsa-utils, 48 kHz
// mono S16). If it is missing, the speech case is skipped with a message.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
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

ProcessingContext ctx(uint32_t rate, uint8_t ch = 1, uint8_t bits = 16, uint32_t ms = 10)
{
  ProcessingContext c;
  c.sample_rate = rate;
  c.channels = ch;
  c.sample_format = bits;
  c.frame_duration_ms = ms;
  return c;
}

std::unique_ptr<IAudioProcessor> make(const std::string & name, const ProcessorConfig & cfg = {})
{
  auto p = create_processor(name);
  CHECK(p);
  CHECK(p->configure(cfg).ok());
  return p;
}

AudioFrame block_from(const int16_t * s, uint32_t rate, uint8_t ch = 1)
{
  AudioFrame f;
  f.sample_rate = rate;
  f.channels = ch;
  f.sample_format = 16;
  f.frame_count = rate / 100;
  f.data = std::make_shared<std::vector<uint8_t>>(f.frame_count * ch * 2);
  std::memcpy(f.data->data(), s, f.data->size());
  return f;
}

// Deterministic roughly-Gaussian white noise.
std::vector<int16_t> noise(size_t n, double amplitude, uint32_t seed = 12345)
{
  std::vector<int16_t> v(n);
  uint32_t x = seed;
  for (size_t i = 0; i < n; ++i) {
    double acc = 0;
    for (int k = 0; k < 4; ++k) {
      x = x * 1664525u + 1013904223u;
      acc += (x >> 8) / double(1u << 24) - 0.5;
    }
    v[i] = static_cast<int16_t>(std::lround(acc * amplitude));
  }
  return v;
}

double rms(const int16_t * s, size_t n)
{
  double e = 0;
  for (size_t i = 0; i < n; ++i)
    e += double(s[i]) * s[i];
  return std::sqrt(e / (n ? n : 1));
}

bool load_wav_mono16(const std::string & path, std::vector<int16_t> & out, uint32_t & rate)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
    return false;
  size_t pos = 12;
  uint16_t ch = 0, bits = 0;
  while (pos + 8 <= b.size()) {
    uint32_t len;
    std::memcpy(&len, b.data() + pos + 4, 4);
    if (!std::memcmp(b.data() + pos, "fmt ", 4)) {
      std::memcpy(&ch, b.data() + pos + 10, 2);
      std::memcpy(&rate, b.data() + pos + 12, 4);
      std::memcpy(&bits, b.data() + pos + 22, 2);
    } else if (!std::memcmp(b.data() + pos, "data", 4)) {
      if (ch != 1 || bits != 16)
        return false;
      len = std::min<uint32_t>(len, static_cast<uint32_t>(b.size() - pos - 8));
      out.resize(len / 2);
      std::memcpy(out.data(), b.data() + pos + 8, out.size() * 2);
      return true;
    }
    pos += 8 + len + (len & 1);
  }
  return false;
}

void test_config_validation()
{
  CHECK(webrtc_processors_available());
  auto ns = create_processor("ns");
  auto vad = create_processor("vad");
  CHECK(ns && vad);
  CHECK(ns->configure({{"level", "very_high"}}).ok());
  Result r = ns->configure({{"level", "max"}});
  CHECK(!r.ok() && r.code == ErrorCode::kInvalidConfig);
  CHECK(r.message.find("very_high") != std::string::npos);  // lists the allowed values
  CHECK(vad->configure({{"likelihood", "very_low"}, {"hangover_ms", "0"}}).ok());
  CHECK(!vad->configure({{"likelihood", "always"}}).ok());
  CHECK(!vad->configure({{"hangover_ms", "2001"}}).ok());
  CHECK(!vad->configure({{"hangover_ms", "-5"}}).ok());
  CHECK(!vad->configure({{"hangover_ms", "abc"}}).ok());
  CHECK(vad->configure({{"highpass", "false"}}).ok());
  CHECK(ns->configure({{"highpass", "true"}}).ok());
  CHECK(!ns->configure({{"highpass", "yes"}}).ok());
  std::printf("PASS test_config_validation\n");
}

void test_format_guard_bypasses_without_crashing()
{
  const ProcessingContext bad[] = {
      ctx(16000, 1, 24),      // 24-bit
      ctx(44100, 1, 16),      // 44.1 kHz
      ctx(22050, 1, 16),      // 22.05 kHz
      ctx(16000, 3, 16),      // 3 channels
      ctx(16000, 1, 16, 20),  // 20 ms blocks
  };
  for (const auto & c : bad) {
    for (const char * name : {"ns", "vad"}) {
      auto p = make(name);
      Result r = p->prepare(c);
      CHECK(!r.ok() && r.code == ErrorCode::kUnsupportedFormat);
      CHECK(!r.message.empty());
    }
  }
  // Pipeline level: both stages bypassed, PCM untouched, flagged, no voice.
  std::vector<ProcessorSlot> slots(2);
  slots[0].name = "ns";
  slots[1].name = "vad";
  std::unique_ptr<ProcessingPipeline> pl;
  CHECK(build_pipeline(slots, pl).ok());
  auto bypassed = pl->prepare(ctx(44100));
  CHECK(bypassed.size() == 2);
  CHECK(pl->stats()[0].init_error.find("44100") != std::string::npos);
  AudioFrame f;
  f.frame_count = 441;
  f.channels = 1;
  f.data = std::make_shared<std::vector<uint8_t>>(882, 0x5a);
  pl->process(f, ctx(44100));
  CHECK(has_flag(f.flags, FrameFlag::kBypassed));
  CHECK(f.voice == VoiceActivity::kUnknown);
  for (uint8_t b : *f.data)
    CHECK(b == 0x5a);
  std::printf("PASS test_format_guard_bypasses_without_crashing\n");
}

void test_ns_reduces_stationary_noise()
{
  for (uint32_t rate : {16000u, 48000u}) {
    for (const char * level : {"moderate", "very_high"}) {
      auto ns = make("ns", {{"level", level}});
      CHECK(ns->prepare(ctx(rate)).ok());
      const size_t n = rate * 4;  // 4 s
      auto in = noise(n, 4000);
      std::vector<int16_t> out(n);
      const size_t blk = rate / 100;
      for (size_t off = 0; off + blk <= n; off += blk) {
        AudioFrame f = block_from(in.data() + off, rate);
        ProcessResult r = ns->process(f, ctx(rate));
        CHECK(r.status == ProcessResultStatus::kProcessed);
        CHECK(f.data->size() == blk * 2);
        std::memcpy(out.data() + off, f.data->data(), blk * 2);
      }
      // Last second, after the noise estimate has converged.
      const double rin = rms(in.data() + n - rate, rate);
      const double rout = rms(out.data() + n - rate, rate);
      const double db = 20 * std::log10(rout / rin);
      std::printf(
          "  ns %s @%u Hz: white noise %.1f -> %.1f rms (%.1f dB)\n", level, rate, rin, rout, db);
      CHECK(db < -6.0);
    }
  }
  std::printf("PASS test_ns_reduces_stationary_noise\n");
}

void test_vad_silence_and_speech()
{
  // Digital silence: never voice.
  {
    auto vad = make("vad");
    CHECK(vad->prepare(ctx(16000)).ok());
    std::vector<int16_t> z(160, 0);
    for (int i = 0; i < 200; ++i) {
      AudioFrame f = block_from(z.data(), 16000);
      CHECK(vad->process(f, ctx(16000)).status == ProcessResultStatus::kProcessed);
      CHECK(f.voice == VoiceActivity::kInactive);
    }
  }

  std::vector<int16_t> speech;
  uint32_t rate = 0;
  const char * path = "/usr/share/sounds/alsa/Front_Center.wav";
  if (!load_wav_mono16(path, speech, rate) || (rate != 16000 && rate != 48000)) {
    std::printf(
        "SKIP test_vad_silence_and_speech speech part: %s not available as mono S16\n", path);
    return;
  }
  // 1 s silence + speech + 2 s silence, so onset and release are visible.
  std::vector<int16_t> sig(rate, 0);
  sig.insert(sig.end(), speech.begin(), speech.end());
  sig.insert(sig.end(), 2 * rate, 0);
  const size_t blk = rate / 100, nblk = sig.size() / blk;

  // "Loud" blocks (RMS > 300) are certainly speech and are used for recall.
  // They are NOT the boundary of speech: the recording's soft onset (the /f/
  // of "front", RMS 26..233 in the first 60 ms) is real speech that the VAD
  // rightly flags. The only ground-truth non-speech is our own zero padding.
  const size_t pad_blocks = rate / blk;
  std::vector<bool> loud(nblk);
  size_t first_loud = nblk, last_loud = 0;
  for (size_t b = 0; b < nblk; ++b) {
    loud[b] = rms(sig.data() + b * blk, blk) > 300;
    if (loud[b]) {
      first_loud = std::min(first_loud, b);
      last_loud = b;
    }
  }
  CHECK(first_loud < nblk);

  // very_low is the node default (see webrtc_processors.hpp); moderate kept
  // as the library's own default for comparison.
  const std::pair<const char *, uint32_t> cases[] = {
      {"moderate", 0}, {"moderate", 300}, {"very_low", 300}};
  for (const auto & cs : cases) {
    const uint32_t hang = cs.second;
    auto vad = make("vad", {{"likelihood", cs.first}, {"hangover_ms", std::to_string(hang)}});
    CHECK(vad->prepare(ctx(rate)).ok());
    size_t hits = 0, n_loud = 0, toggles = 0, before = 0, last_active = 0;
    bool prev = false;
    for (size_t b = 0; b < nblk; ++b) {
      AudioFrame f = block_from(sig.data() + b * blk, rate);
      const std::vector<uint8_t> orig = *f.data;
      CHECK(vad->process(f, ctx(rate)).status == ProcessResultStatus::kProcessed);
      CHECK(*f.data == orig);  // VAD never modifies PCM
      const bool act = f.voice == VoiceActivity::kActive;
      CHECK(f.voice != VoiceActivity::kUnknown);
      if (loud[b]) {
        ++n_loud;
        hits += act;
      }
      if (b < pad_blocks)
        before += act;  // inside the 1 s of exact digital zeros we prepended
      if (act)
        last_active = b;
      toggles += (b && act != prev);
      prev = act;
    }
    const double recall = double(hits) / n_loud;
    std::printf(
        "  vad %s hangover %u ms @%u Hz: %zu/%zu loud blocks voiced (%.0f%%), %zu voiced in "
        "leading zero padding, %zu toggles, last voiced %+lld ms after last loud block\n",
        cs.first, hang, rate, hits, n_loud, recall * 100, before, toggles,
        (long long)((long long)last_active - (long long)last_loud) * 10);
    CHECK(recall > 0.6);
    CHECK(before == 0);
    CHECK(!prev);  // back to inactive in the trailing 2 s of silence
    // Hangover keeps the decision on after the last loud block for about
    // hangover_ms (plus whatever the raw VAD itself held).
    if (hang == 300)
      CHECK((long long)last_active - (long long)last_loud >= 25);
  }
  std::printf("PASS test_vad_silence_and_speech\n");
}

void test_vad_failure_never_blocks_pcm()
{
  std::vector<ProcessorSlot> slots(1);
  slots[0].name = "vad";
  std::unique_ptr<ProcessingPipeline> pl;
  CHECK(build_pipeline(slots, pl).ok());
  CHECK(pl->prepare(ctx(16000)).empty());
  for (int i = 0; i < 15; ++i) {
    AudioFrame f;  // wrong size for a 16 kHz 10 ms block -> process() fails
    f.channels = 1;
    f.frame_count = 80;
    f.data = std::make_shared<std::vector<uint8_t>>(160, 0x11);
    pl->process(f, ctx(16000));
    CHECK(f.data->size() == 160 && (*f.data)[0] == 0x11);  // PCM untouched
    CHECK(f.voice == VoiceActivity::kUnknown);
    if (i >= static_cast<int>(ProcessingPipeline::kMaxConsecutiveFailures))
      CHECK(has_flag(f.flags, FrameFlag::kBypassed));  // degraded -> bypassed
  }
  CHECK(pl->stats()[0].health == ProcessorHealth::kDegraded);
  CHECK(pl->stats()[0].failed_count == ProcessingPipeline::kMaxConsecutiveFailures);
  std::printf("PASS test_vad_failure_never_blocks_pcm\n");
}

class OneShotBackend : public IAudioCaptureBackend
{
public:
  Result configure(const AudioStreamConfig &) override { return Result::Ok(); }
  Result start(FrameCallback cb) override
  {
    cb_ = std::move(cb);
    return Result::Ok();
  }
  Result stop() override
  {
    cb_ = nullptr;
    return Result::Ok();
  }
  AudioBackendStatus status() const override { return AudioBackendStatus::kReady; }
  void emit(const std::vector<int16_t> & s, uint64_t seq)
  {
    AudioFrame f;
    f.sample_rate = 16000;
    f.channels = 1;
    f.sample_format = 16;
    f.frame_count = static_cast<uint32_t>(s.size());
    f.sequence = seq;
    f.data = std::make_shared<std::vector<uint8_t>>(s.size() * 2);
    std::memcpy(f.data->data(), s.data(), f.data->size());
    if (cb_)
      cb_(f);
  }

private:
  FrameCallback cb_;
};

void test_ns_vad_chain_through_capture_backend()
{
  std::vector<ProcessorSlot> slots(2);
  CHECK(parse_processor_spec("ns:level=high", slots[0]).ok());
  CHECK(parse_processor_spec("vad:hangover_ms=0", slots[1]).ok());
  std::unique_ptr<ProcessingPipeline> pl;
  CHECK(build_pipeline(slots, pl).ok());
  auto inner = std::make_unique<OneShotBackend>();
  OneShotBackend * raw = inner.get();
  ProcessingCaptureBackend b(std::move(inner), std::move(pl));
  AudioStreamConfig c;
  c.sample_rate = 16000;
  c.channels = 1;
  c.sample_format = 16;
  c.frame_duration_ms = 10;
  CHECK(b.configure(c).ok());
  std::mutex m;
  std::vector<AudioFrame> got;
  CHECK(
      b.start([&](const AudioFrame & f) {
         std::lock_guard<std::mutex> l(m);
         got.push_back(f);
       }).ok());
  // 50 chunks of 20 ms (320 samples) of silence: every chunk gets a decision.
  std::vector<int16_t> z(320, 0);
  for (uint64_t i = 0; i < 50; ++i)
    raw->emit(z, i);
  CHECK(b.stop().ok());
  CHECK(got.size() == 50);
  for (const auto & f : got) {
    CHECK(f.frame_count == 320);
    CHECK(f.voice == VoiceActivity::kInactive);
  }
  auto s = b.stats();
  CHECK(s.processors.size() == 2 && s.processors[0].name == "ns" && s.processors[1].name == "vad");
  CHECK(s.processors[0].processed_count == 100 && s.processors[1].processed_count == 100);
  std::printf("PASS test_ns_vad_chain_through_capture_backend (ns avg %llu us, vad avg %llu us)\n",
      (unsigned long long)(s.processors[0].total_process_usec / 100),
      (unsigned long long)(s.processors[1].total_process_usec / 100));
}

// Measures the input->output lag of a chain by cross-correlating white
// noise, and checks it against ProcessingPipeline::latency_frames(), which is
// what ProcessingCaptureBackend subtracts from timestamps.
void test_latency_matches_measurement()
{
  std::vector<std::vector<std::string>> chains = {{"ns"}, {"ns:highpass=true"}, {"ns", "vad"}};
  if (webrtc_aec_available()) {
    chains.push_back({"aec"});
    chains.push_back({"aec", "ns"});
    chains.push_back({"aec", "ns", "vad"});
  }
  for (uint32_t rate : {8000u, 16000u, 32000u, 48000u}) {
    for (uint8_t ch : {uint8_t(1), uint8_t(2)}) {
      for (const auto & chain : chains) {
        std::vector<ProcessorSlot> slots(chain.size());
        std::string label;
        for (size_t i = 0; i < chain.size(); ++i) {
          CHECK(parse_processor_spec(chain[i], slots[i]).ok());
          label += (i ? "," : "") + chain[i];
        }
        std::unique_ptr<ProcessingPipeline> pl;
        CHECK(build_pipeline(slots, pl).ok());
        CHECK(pl->prepare(ctx(rate, ch)).empty());
        const size_t blk = rate / 100, nblk = 200;
        auto in = noise(blk * nblk * ch, 6000, 777);
        std::vector<int16_t> out(in.size());
        for (size_t b = 0; b < nblk; ++b) {
          AudioFrame f = block_from(in.data() + b * blk * ch, rate, ch);
          pl->process(f, ctx(rate, ch));
          std::memcpy(out.data() + b * blk * ch, f.data->data(), blk * ch * 2);
        }
        // Channel 0, skip the first second, search lags 0..50 ms.
        size_t best = 0;
        double bestv = -1e300;
        const size_t n0 = rate, n = rate / 2;
        for (size_t lag = 0; lag < rate / 20; ++lag) {
          double acc = 0;
          for (size_t i = 0; i < n; ++i)
            acc += double(out[(n0 + i + lag) * ch]) * in[(n0 + i) * ch];
          if (acc > bestv) {
            bestv = acc;
            best = lag;
          }
        }
        if (best != pl->latency_frames()) {
          std::printf("  [%s] @%u Hz %u ch: measured %zu frames, latency_frames() %u\n",
              label.c_str(), rate, ch, best, pl->latency_frames());
        }
        CHECK(best == pl->latency_frames());
      }
    }
  }
  std::printf(
      "PASS test_latency_matches_measurement (%zu chains x 4 rates x 2 channel counts, "
      "%s)\n",
      chains.size(), webrtc_implementation().c_str());
}

}  // namespace

int main()
{
  test_config_validation();
  test_format_guard_bypasses_without_crashing();
  test_ns_reduces_stationary_noise();
  test_vad_silence_and_speech();
  test_vad_failure_never_blocks_pcm();
  test_ns_vad_chain_through_capture_backend();
  test_latency_matches_measurement();
  std::printf("all core_webrtc_processors_test cases passed\n");
  return 0;
}
