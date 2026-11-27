// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// ReferenceBuffer (AEC echo reference pairing): timestamp-placed first block
// with the early (newer-reference) lead, strict sample-count pairing after
// that despite bursty delivery, re-sync on reference/mic gaps and on drift,
// unavailable (zeros, no waiting) when never started / failed / far behind,
// bounded wait for samples that are due, and a producer/consumer thread run
// for ThreadSanitizer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "qrb_audio_common_lib/core/reference_buffer.hpp"

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
constexpr uint32_t kRate = 16000;
constexpr size_t kBlk = 160;                // 10 ms
constexpr uint64_t kT0 = 5'000'000'000ULL;  // arbitrary monotonic origin, us

// Reference sample i carries value (i % 30000) so a fetched block tells
// exactly which reference position it came from.
std::vector<int16_t> ramp(uint64_t first, size_t n)
{
  std::vector<int16_t> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<int16_t>((first + i) % 30000);
  return v;
}
uint64_t ts_of(uint64_t sample)
{
  return kT0 + sample * 1000000 / kRate;
}

// Pushes reference samples [from, to) in chunks of `chunk` samples.
void push_range(ReferenceBuffer & rb, uint64_t from, uint64_t to, size_t chunk, bool gap = false)
{
  for (uint64_t s = from; s < to; s += chunk) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, to - s));
    auto v = ramp(s, n);
    rb.push(v.data(), n, ts_of(s), gap && s == from);
  }
}

ReferenceBuffer::Options opts(uint32_t lead_ms, uint32_t wait_ms = 0)
{
  ReferenceBuffer::Options o;
  o.lead_ms = lead_ms;
  o.max_wait_ms = wait_ms;
  return o;
}

void test_unavailable_before_any_reference()
{
  ReferenceBuffer rb(kRate, opts(5));
  std::vector<int16_t> out(kBlk, 7);
  CHECK(!rb.fetch(ts_of(0), false, out.data(), kBlk));
  for (auto s : out)
    CHECK(s == 0);
  CHECK(rb.stats().blocks_unavailable == 1);
  std::printf("PASS test_unavailable_before_any_reference\n");
}

void test_first_block_placed_by_timestamp_with_lead()
{
  ReferenceBuffer rb(kRate, opts(5));
  push_range(rb, 0, kRate, 320);  // 1 s of reference
  std::vector<int16_t> out(kBlk);
  // Mic block captured at sample 4000's time: reference should start 5 ms
  // (80 samples) later = newer reference.
  CHECK(rb.fetch(ts_of(4000), false, out.data(), kBlk));
  CHECK(out[0] == 4080);
  CHECK(rb.stats().syncs == 1);
  // No timestamps on the mic side: cannot place a first block.
  ReferenceBuffer rb2(kRate, opts(5));
  push_range(rb2, 0, kRate, 320);
  CHECK(!rb2.fetch(0, false, out.data(), kBlk));
  std::printf("PASS test_first_block_placed_by_timestamp_with_lead\n");
}

void test_sample_count_pairing_ignores_jittered_timestamps()
{
  ReferenceBuffer rb(kRate, opts(0));
  push_range(rb, 0, kRate * 3 / 2, 480);  // 1.5 s, inside the 2 s ring
  std::vector<int16_t> out(kBlk);
  CHECK(rb.fetch(ts_of(1600), false, out.data(), kBlk));
  CHECK(out[0] == 1600);
  // Mic timestamps wobble by +-15 ms (Pulse latency sawtooth): pairing must
  // keep advancing exactly one block per block.
  for (int k = 1; k < 100; ++k) {
    const int64_t jitter_us = (k % 2 ? 15000 : -15000);
    const uint64_t mic_ts = ts_of(1600 + k * kBlk) + jitter_us;
    CHECK(rb.fetch(mic_ts, false, out.data(), kBlk));
    CHECK(out[0] == static_cast<int16_t>(1600 + k * kBlk));
  }
  CHECK(rb.stats().syncs == 1);
  CHECK(rb.stats().drift_resyncs == 0);
  std::printf("PASS test_sample_count_pairing_ignores_jittered_timestamps\n");
}

void test_resync_on_reference_gap_mic_gap_and_drift()
{
  ReferenceBuffer rb(kRate, opts(0));
  std::vector<int16_t> out(kBlk);
  push_range(rb, 0, 8000, 320);
  CHECK(rb.fetch(ts_of(1600), false, out.data(), kBlk));
  // Reference lost samples 8000..9600: the next chunk is flagged as a gap.
  push_range(rb, 9600, 16000, 320, /*gap=*/true);
  CHECK(rb.fetch(ts_of(10000), false, out.data(), kBlk));
  CHECK(out[0] == 10000);  // re-placed by timestamp, not by counting
  CHECK(rb.stats().syncs == 2);
  // Mic gap (block flagged kDropped upstream): re-place by timestamp.
  CHECK(rb.fetch(ts_of(12000), true, out.data(), kBlk));
  CHECK(out[0] == 12000);
  CHECK(rb.stats().syncs == 3);
  // Mic timestamps jump 50 ms ahead (> 40 ms threshold) without a flag:
  // treated as drift, re-placed.
  CHECK(rb.fetch(ts_of(12160 + 800), false, out.data(), kBlk));
  CHECK(out[0] == 12960);
  CHECK(rb.stats().drift_resyncs == 1);
  CHECK(rb.stats().last_drift_frames == -800);
  // resync_threshold_ms = 0: the same jump is ignored, counting continues.
  ReferenceBuffer::Options o = opts(0);
  o.resync_threshold_ms = 0;
  ReferenceBuffer rb0(kRate, o);
  push_range(rb0, 0, 16000, 320);
  CHECK(rb0.fetch(ts_of(1600), false, out.data(), kBlk));
  CHECK(rb0.fetch(ts_of(1760 + 1600), false, out.data(), kBlk));
  CHECK(out[0] == 1760);
  CHECK(rb0.stats().drift_resyncs == 0);
  std::printf("PASS test_resync_on_reference_gap_mic_gap_and_drift\n");
}

void test_failed_and_far_behind_reference_are_unavailable_without_waiting()
{
  ReferenceBuffer rb(kRate, opts(0, /*wait_ms=*/60));
  std::vector<int16_t> out(kBlk);
  push_range(rb, 0, 1600, 160);
  CHECK(rb.fetch(ts_of(0), false, out.data(), kBlk));
  // Mic is 1 s ahead of everything delivered: deficit >> wait budget.
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!rb.fetch(ts_of(17600), false, out.data(), kBlk));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited < std::chrono::milliseconds(20));
  rb.mark_failed();
  CHECK(!rb.live());
  CHECK(!rb.fetch(ts_of(1760), false, out.data(), kBlk));
  // Pushing again revives it (re-sync by timestamp).
  push_range(rb, 1600, 3200, 160);
  CHECK(rb.fetch(ts_of(2000), false, out.data(), kBlk));
  CHECK(out[0] == 2000);
  std::printf("PASS test_failed_and_far_behind_reference_are_unavailable_without_waiting\n");
}

void test_stall_then_resume()
{
  ReferenceBuffer::Options o = opts(0, 10);
  o.stale_ms = 50;
  ReferenceBuffer rb(kRate, o);
  std::vector<int16_t> out(kBlk);
  push_range(rb, 0, 1600, 160);
  CHECK(rb.fetch(ts_of(0), false, out.data(), kBlk));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));  // > stale_ms
  CHECK(!rb.live());
  CHECK(!rb.fetch(ts_of(160), false, out.data(), kBlk));
  // Resume (the stall itself forces a re-sync even without a gap flag).
  push_range(rb, 4800, 8000, 160);
  CHECK(rb.fetch(ts_of(5000), false, out.data(), kBlk));
  CHECK(out[0] == 5000);
  std::printf("PASS test_stall_then_resume\n");
}

void test_overwritten_target_moves_to_newer_reference()
{
  ReferenceBuffer::Options o = opts(0);
  o.capacity_ms = 500;  // 8000 samples
  ReferenceBuffer rb(kRate, o);
  std::vector<int16_t> out(kBlk);
  push_range(rb, 0, 16000, 320);
  // Target sample 1000 was overwritten long ago: serve the oldest held one
  // (newer than the target = early, the safe side for AEC3).
  CHECK(rb.fetch(ts_of(1000), false, out.data(), kBlk));
  CHECK(out[0] == 8000);
  std::printf("PASS test_overwritten_target_moves_to_newer_reference\n");
}

void test_threads_bursty_producer()
{
  // Producer delivers 30 ms bursts in real time; consumer asks for each
  // 10 ms block as soon as its capture time has passed, so it regularly has
  // to wait for the burst carrying it. Every block must pair exactly.
  ReferenceBuffer rb(kRate, opts(0, /*wait_ms=*/60));
  std::atomic<bool> done{false};
  const uint64_t total = kRate * 2;  // 2 s
  const auto start = std::chrono::steady_clock::now();
  std::thread prod([&] {
    for (uint64_t s = 0; s < total; s += 480) {
      std::this_thread::sleep_until(start + std::chrono::microseconds((s + 480) * 1000000 / kRate));
      auto v = ramp(s, 480);
      rb.push(v.data(), v.size(), ts_of(s), false);
    }
    done = true;
  });
  std::vector<int16_t> out(kBlk);
  size_t ok = 0, exact = 0;
  // Before the first reference chunk exists there is nothing to pair (the
  // AEC stage would pass audio through); start once it has arrived.
  while (!rb.live())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (uint64_t b = 0; b + kBlk <= total; b += kBlk) {
    std::this_thread::sleep_until(start + std::chrono::microseconds((b + kBlk) * 1000000 / kRate));
    if (rb.fetch(ts_of(b), false, out.data(), kBlk)) {
      ++ok;
      exact += out[0] == static_cast<int16_t>(b % 30000);
    }
  }
  prod.join();
  const auto st = rb.stats();
  std::printf("  threaded: %zu/%llu blocks served, %zu exact, %llu waits, %llu syncs\n", ok,
      static_cast<unsigned long long>(total / kBlk), exact,
      static_cast<unsigned long long>(st.waits), static_cast<unsigned long long>(st.syncs));
  CHECK(ok == total / kBlk);
  CHECK(exact == ok);
  CHECK(st.waits > 0);
  CHECK(st.syncs == 1);
  std::printf("PASS test_threads_bursty_producer\n");
}

}  // namespace

int main()
{
  test_unavailable_before_any_reference();
  test_first_block_placed_by_timestamp_with_lead();
  test_sample_count_pairing_ignores_jittered_timestamps();
  test_resync_on_reference_gap_mic_gap_and_drift();
  test_failed_and_far_behind_reference_are_unavailable_without_waiting();
  test_stall_then_resume();
  test_overwritten_target_moves_to_newer_reference();
  test_threads_bursty_producer();
  std::printf("all core_reference_buffer_test cases passed\n");
  return 0;
}
