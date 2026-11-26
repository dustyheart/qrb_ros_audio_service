// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// ProcessingCaptureBackend: thread decoupling, bounded-queue drop policy,
// re-framed processing, stop() drain/join semantics and processor factory /
// init-failure handling. No hardware, no ROS.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
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

// Inner backend driven by the test from its own "audio thread", like the
// PulseAudio mainloop would drive PipeWirePulseBackend.
class ScriptedBackend : public IAudioCaptureBackend
{
public:
  Result configure(const AudioStreamConfig & c) override
  {
    config = c;
    status_ = AudioBackendStatus::kStopped;
    return configure_result;
  }
  Result start(FrameCallback cb) override
  {
    std::lock_guard<std::mutex> l(m_);
    cb_ = std::move(cb);
    status_ = AudioBackendStatus::kReady;
    return start_result;
  }
  Result stop() override
  {
    std::lock_guard<std::mutex> l(m_);  // like PA: no callback after stop returns
    cb_ = nullptr;
    status_ = AudioBackendStatus::kStopped;
    ++stop_calls;
    return Result::Ok();
  }
  AudioBackendStatus status() const override { return status_; }

  // Returns how long the callback took, i.e. how long the "audio thread" was
  // blocked by the decorator.
  std::chrono::microseconds emit(uint32_t frames, uint64_t seq)
  {
    AudioFrame f;
    f.sample_rate = config.sample_rate;
    f.channels = config.channels;
    f.sample_format = config.sample_format;
    f.frame_count = frames;
    f.sequence = seq;
    f.timestamp_usec = 1000000 + seq * 1000;
    f.data = std::make_shared<std::vector<uint8_t>>(frames * 2, static_cast<uint8_t>(seq));
    std::lock_guard<std::mutex> l(m_);
    auto t0 = std::chrono::steady_clock::now();
    if (cb_)
      cb_(f);
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0);
  }

  AudioStreamConfig config;
  Result configure_result = Result::Ok();
  Result start_result = Result::Ok();
  int stop_calls = 0;

private:
  std::mutex m_;
  FrameCallback cb_;
  AudioBackendStatus status_ = AudioBackendStatus::kUnconnected;
};

AudioStreamConfig mono16k(uint32_t frame_ms)
{
  AudioStreamConfig c;
  c.sample_rate = 16000;
  c.channels = 1;
  c.sample_format = 16;
  c.frame_duration_ms = frame_ms;
  return c;
}

std::unique_ptr<ProcessingPipeline> pipeline_of(const std::vector<std::string> & specs)
{
  std::vector<ProcessorSlot> slots;
  for (const auto & s : specs) {
    ProcessorSlot slot;
    CHECK(parse_processor_spec(s, slot).ok());
    slots.push_back(slot);
  }
  std::unique_ptr<ProcessingPipeline> p;
  CHECK(build_pipeline(slots, p).ok());
  return p;
}

void test_passthrough_runs_callback_on_worker_thread()
{
  auto inner = std::make_unique<ScriptedBackend>();
  ScriptedBackend * raw = inner.get();
  ProcessingCaptureBackend b(std::move(inner), nullptr);
  CHECK(b.configure(mono16k(20)).ok());

  std::mutex m;
  std::vector<AudioFrame> got;
  std::thread::id cb_thread;
  CHECK(
      b.start([&](const AudioFrame & f) {
         std::lock_guard<std::mutex> l(m);
         cb_thread = std::this_thread::get_id();
         got.push_back(f);
       }).ok());

  std::thread audio([&] {
    for (uint64_t i = 0; i < 50; ++i)
      raw->emit(1280, i);
  });
  const auto audio_id = audio.get_id();
  audio.join();
  CHECK(b.stop().ok());  // drains: every accepted chunk is delivered

  CHECK(got.size() == 50);
  CHECK(cb_thread != audio_id);
  for (uint64_t i = 0; i < 50; ++i) {
    CHECK(got[i].sequence == i);
    CHECK(got[i].frame_count == 1280);
    CHECK(got[i].timestamp_usec == 1000000 + i * 1000);
    CHECK(got[i].flags == 0);  // no processors -> not even kBypassed
  }
  auto s = b.stats();
  CHECK(s.chunks_in == 50 && s.chunks_out == 50 && s.chunks_dropped == 0);
  CHECK(s.blocks_processed == 0);
  std::printf("PASS test_passthrough_runs_callback_on_worker_thread\n");
}

void test_slow_consumer_drops_without_blocking_audio_thread()
{
  auto inner = std::make_unique<ScriptedBackend>();
  ScriptedBackend * raw = inner.get();
  ProcessingCaptureBackend b(std::move(inner), nullptr, 4);
  CHECK(b.configure(mono16k(20)).ok());

  std::vector<AudioFrame> got;
  CHECK(
      b.start([&](const AudioFrame & f) {
         std::this_thread::sleep_for(std::chrono::milliseconds(20));  // e.g. stuck publish
         got.push_back(f);
       }).ok());

  std::chrono::microseconds worst{0};
  for (uint64_t i = 0; i < 40; ++i) {
    auto d = raw->emit(320, i);
    if (d > worst)
      worst = d;
  }
  // Let the worker drain, then one more chunk: the first one accepted after
  // the drops must carry kDropped.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  raw->emit(320, 40);
  CHECK(b.stop().ok());

  // The audio thread was never held for anything close to the 20 ms the
  // consumer takes per chunk.
  CHECK(worst < std::chrono::milliseconds(5));
  auto s = b.stats();
  CHECK(s.chunks_dropped > 0);
  CHECK(s.chunks_in + s.chunks_dropped == 41);
  CHECK(s.chunks_out == s.chunks_in);
  CHECK(s.queue_high_water <= 4);
  CHECK(got.size() == s.chunks_out);
  // The first chunk delivered after a drop carries kDropped; sequence shows
  // the gap.
  bool saw_dropped = false;
  for (size_t i = 1; i < got.size(); ++i) {
    if (got[i].sequence != got[i - 1].sequence + 1) {
      CHECK(has_flag(got[i].flags, FrameFlag::kDropped));
      saw_dropped = true;
    }
  }
  CHECK(saw_dropped);
  CHECK(got.back().sequence == 40 && has_flag(got.back().flags, FrameFlag::kDropped));
  std::printf("PASS test_slow_consumer_drops_without_blocking_audio_thread (worst %lld us)\n",
      static_cast<long long>(worst.count()));
}

void test_processing_reframes_and_preserves_chunks()
{
  auto inner = std::make_unique<ScriptedBackend>();
  ScriptedBackend * raw = inner.get();
  ProcessingCaptureBackend b(
      std::move(inner), pipeline_of({"passthrough", "test_delay:delay_us=0"}));
  CHECK(b.configure(mono16k(10)).ok());
  std::vector<AudioFrame> got;
  CHECK(b.start([&](const AudioFrame & f) { got.push_back(f); }).ok());
  const uint32_t sizes[] = {1280, 320, 333, 1000, 7};
  uint64_t total = 0;
  for (uint64_t i = 0; i < 5; ++i) {
    raw->emit(sizes[i], i);
    total += sizes[i];
  }
  CHECK(b.stop().ok());
  CHECK(got.size() == 5);  // stop() flushed the sub-block tail
  for (uint64_t i = 0; i < 5; ++i) {
    CHECK(got[i].sequence == i);
    CHECK(got[i].frame_count == sizes[i]);
    CHECK(got[i].byte_size() == sizes[i] * 2ull);
    CHECK(got[i].timestamp_usec == 1000000 + i * 1000);
    CHECK((*got[i].data)[0] == static_cast<uint8_t>(i));
  }
  auto s = b.stats();
  CHECK(s.blocks_processed == total / 160);
  CHECK(s.processors.size() == 2);
  CHECK(s.processors[0].name == "passthrough");
  CHECK(s.processors[0].bypassed_count == total / 160);
  CHECK(s.processors[1].processed_count == total / 160);
  std::printf("PASS test_processing_reframes_and_preserves_chunks\n");
}

void test_stop_is_idempotent_and_no_callback_after_stop()
{
  auto inner = std::make_unique<ScriptedBackend>();
  ScriptedBackend * raw = inner.get();
  std::atomic<bool> stopped{false};
  std::atomic<int> late{0};
  {
    ProcessingCaptureBackend b(std::move(inner), nullptr);
    CHECK(b.configure(mono16k(20)).ok());
    CHECK(
        b.start([&](const AudioFrame &) {
           if (stopped)
             ++late;
         }).ok());
    for (uint64_t i = 0; i < 10; ++i)
      raw->emit(320, i);
    CHECK(b.stop().ok());
    stopped = true;
    raw->emit(320, 99);  // inner ignores it after stop, like PA
    CHECK(b.stop().ok());
  }  // destructor calls stop() again
  CHECK(late == 0);
  std::printf("PASS test_stop_is_idempotent_and_no_callback_after_stop\n");
}

void test_start_failure_cleans_up_worker()
{
  auto inner = std::make_unique<ScriptedBackend>();
  inner->start_result = Result::Error(ErrorCode::kBackendInitFailed, "no device");
  ProcessingCaptureBackend b(std::move(inner), nullptr);
  CHECK(b.configure(mono16k(20)).ok());
  Result r = b.start([](const AudioFrame &) {});
  CHECK(!r.ok() && r.code == ErrorCode::kBackendInitFailed);
  CHECK(b.stop().ok());  // must not hang on a dead worker
  std::printf("PASS test_start_failure_cleans_up_worker\n");
}

void test_config_and_factory_errors()
{
  {
    // 16 kHz x 10 ms is fine; 44.1 kHz x 15 ms = 661.5 samples is not.
    AudioStreamConfig c = mono16k(15);
    c.sample_rate = 44100;
    ProcessingCaptureBackend b(std::make_unique<ScriptedBackend>(), pipeline_of({"passthrough"}));
    Result r = b.configure(c);
    CHECK(!r.ok() && r.code == ErrorCode::kInvalidConfig);
  }
  ProcessorSlot slot;
  CHECK(!parse_processor_spec("", slot).ok());
  CHECK(!parse_processor_spec("webrtc_aec3", slot).ok());
  CHECK(!parse_processor_spec("test_delay:delay_us", slot).ok());
  CHECK(!parse_processor_spec("test_delay:", slot).ok());
  CHECK(parse_processor_spec("test_delay:delay_us=5,x=y", slot).ok());
  CHECK(
      slot.name == "test_delay" && slot.config.at("delay_us") == "5" && slot.config.at("x") == "y");

  // configure() failure must fail pipeline construction with the reason.
  ProcessorSlot bad;
  CHECK(parse_processor_spec("test_delay:delay_us=-1", bad).ok());
  std::unique_ptr<ProcessingPipeline> p;
  Result r = build_pipeline({bad}, p);
  CHECK(!r.ok() && r.code == ErrorCode::kProcessorInitFailed);
  CHECK(r.message.find("delay_us") != std::string::npos);
  CHECK(!p);

  ProcessorSlot dup;
  CHECK(parse_processor_spec("passthrough", dup).ok());
  r = build_pipeline({dup, dup}, p);
  CHECK(!r.ok());

  ProcessorSlot skipped = dup;
  skipped.bypassed = true;
  CHECK(build_pipeline({skipped}, p).ok());
  CHECK(p && p->processor_count() == 0);
  std::printf("PASS test_config_and_factory_errors\n");
}

}  // namespace

int main()
{
  test_passthrough_runs_callback_on_worker_thread();
  test_slow_consumer_drops_without_blocking_audio_thread();
  test_processing_reframes_and_preserves_chunks();
  test_stop_is_idempotent_and_no_callback_after_stop();
  test_start_failure_cleans_up_worker();
  test_config_and_factory_errors();
  std::printf("all core_processing_backend_test cases passed\n");
  return 0;
}
