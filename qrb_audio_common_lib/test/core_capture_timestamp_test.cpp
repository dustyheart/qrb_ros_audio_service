// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// CaptureTimestamper / map_monotonic_to_clock_ns / split_time_ns with
// synthetic input: no audio server, no ROS. Explicit checks (not assert) so
// it still tests something under NDEBUG.

#include <cstdint>
#include <cstdio>
#include <limits>

#include "qrb_audio_common_lib/core/capture_timestamp.hpp"

using namespace qrb::audio_common_lib::core;

static int g_failures = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                \
    }                                                                              \
  } while (0)

// Ideal input (no jitter, stable 85 ms latency, audio clock == monotonic):
// first stamp = arrival - latency, every step exactly 80 ms.
static void test_steady_stream()
{
  CaptureTimestamper t(16000);
  const uint64_t base = 1000000000ull;
  uint64_t prev = 0;
  for (int i = 0; i < 100; ++i) {
    uint64_t arrival = base + static_cast<uint64_t>(i) * 80000 + 85000;
    auto out = t.on_chunk(1280, arrival, 85000);
    CHECK(out.timestamp_usec != 0);
    if (i == 0)
      CHECK(out.timestamp_usec == base);
    else
      CHECK(out.timestamp_usec - prev >= 79999 && out.timestamp_usec - prev <= 80001);
    CHECK(!out.resynced);
    prev = out.timestamp_usec;
  }
  CHECK(t.resync_count() == 0);
}

// Reproduces the hardware pattern from test_report.md 5h: audio delivered
// 1.9 % faster than CLOCK_MONOTONIC, arrivals bunched (every 8th chunk 40 ms
// early), latency a +-20 ms sawtooth. After the window has a few seconds of
// data the steps must be smooth (~80 ms / 1.019) and the stamps must track
// the true capture time within a few ms, with no resyncs.
static void test_drift_and_jitter_are_absorbed()
{
  CaptureTimestamper t(16000);
  const double ratio = 1.019;  // audio seconds per monotonic second
  const uint64_t base = 3000000000ull;
  uint64_t prev = 0;
  double max_err_late = 0, min_step = 1e9, max_step = 0;
  for (int i = 0; i < 300; ++i) {
    double true_capture = base + i * 80000.0 / ratio;  // first sample, monotonic us
    double arrival = true_capture + 160000.0 - ((i % 8) == 7 ? 40000.0 : 0.0);
    double reported_latency = 160000.0 + ((i % 8) - 3.5) * 5000.0;  // sawtooth
    auto out = t.on_chunk(1280, static_cast<uint64_t>(arrival), static_cast<int64_t>(reported_latency));
    CHECK(out.timestamp_usec != 0);
    CHECK(!out.resynced);
    if (i > 0) {
      CHECK(out.timestamp_usec >= prev);  // non-decreasing, always
      if (i >= 150) {  // window full (8 s ~ 100 chunks) and settled
        double step = static_cast<double>(out.timestamp_usec - prev);
        min_step = step < min_step ? step : min_step;
        max_step = step > max_step ? step : max_step;
        double err = static_cast<double>(out.timestamp_usec) - true_capture;
        err = err < 0 ? -err : err;
        max_err_late = err > max_err_late ? err : max_err_late;
      }
    }
    prev = out.timestamp_usec;
  }
  const double expected_step = 80000.0 / ratio;  // ~78508 us (window 2 s, slew 2 %)
  std::printf("drift test: step min/max %.1f/%.1f us (expected %.1f), max |err| %.1f us, ratio %.5f\n",
      min_step, max_step, expected_step, max_err_late, t.clock_ratio());
  CHECK(min_step >= expected_step * 0.94 && max_step <= expected_step * 1.06);  // +-5 % slew + wobble
  CHECK(max_err_late < 10000);  // within 10 ms of the true capture time
  CHECK(t.clock_ratio() > 0.975 && t.clock_ratio() < 0.987);  // measured ~1/1.019
  CHECK(t.resync_count() == 0);
}

// Server dropped 300 ms of audio: arrival jumps far beyond the prediction ->
// window restarts forward, gap flagged, stamps keep stepping normally after.
static void test_gap_resyncs_forward()
{
  CaptureTimestamper t(16000);
  const uint64_t a0 = 5000000;
  uint64_t ts = 0;
  for (int i = 0; i < 20; ++i)
    ts = t.on_chunk(1280, a0 + i * 80000 + 80000, 80000).timestamp_usec;
  CHECK(ts == a0 + 19 * 80000);
  auto g = t.on_chunk(1280, a0 + 20 * 80000 + 80000 + 300000, 80000);
  CHECK(g.resynced && g.gap_detected);
  CHECK(g.timestamp_usec == a0 + 20 * 80000 + 300000);
  auto n = t.on_chunk(1280, a0 + 21 * 80000 + 80000 + 300000, 80000);
  CHECK(!n.resynced);
  CHECK(n.timestamp_usec - g.timestamp_usec == 80000);
  CHECK(t.resync_count() == 1);
}

// A large backward jump in the raw estimate restarts the window but must not
// make the output go backwards.
static void test_backward_jump_stays_monotonic()
{
  CaptureTimestamper t(16000);
  uint64_t a = 10000000, prev = 0;
  for (int i = 0; i < 10; ++i)
    prev = t.on_chunk(1280, a + i * 80000 + 80000, 80000).timestamp_usec;
  auto o = t.on_chunk(1280, a + 10 * 80000 + 80000 - 500000, 80000);
  CHECK(o.resynced && !o.gap_detected);
  CHECK(o.timestamp_usec >= prev);
}

// Unknown latency: no timestamp (0) before the first valid point; afterwards
// the chunk is extrapolated from the fit and NOT added to it.
static void test_unknown_latency()
{
  CaptureTimestamper t(48000);
  auto o0 = t.on_chunk(960, 2000000, CaptureTimestamper::kUnknownLatency);
  CHECK(o0.timestamp_usec == 0);
  auto o1 = t.on_chunk(960, 2020000, 25000);  // first valid point: chunk 1 began at 1995000
  CHECK(o1.timestamp_usec == 1995000);
  auto o2 = t.on_chunk(960, 2040000, CaptureTimestamper::kUnknownLatency);
  CHECK(!o2.resynced);
  CHECK(o2.timestamp_usec - o1.timestamp_usec == 20000);
}

// Hardware pattern: the first chunk reports ~0.2 ms latency (shorter than the
// chunk itself, impossible) - it must get timestamp 0 rather than anchor the
// timeline ~160 ms late (which, before this rule, then had to be clamped and
// stayed late for seconds).
static void test_bogus_first_latency()
{
  CaptureTimestamper t(16000);
  const uint64_t base = 7000000;  // true capture time of chunk 0
  auto o0 = t.on_chunk(1280, base + 160000, 200);
  CHECK(o0.timestamp_usec == 0);
  uint64_t prev = 0;
  for (int i = 1; i < 40; ++i) {
    auto o = t.on_chunk(1280, base + i * 80000 + 160000, 160000);
    CHECK(o.timestamp_usec == base + static_cast<uint64_t>(i) * 80000);
    CHECK(o.timestamp_usec > prev);
    CHECK(!o.resynced);
    prev = o.timestamp_usec;
  }
}

// Hardware pattern: for the first ~6 s the stream delivers ~9 % more samples
// than real time, then settles at 1.0. Timestamps must stay monotonic, not
// resync, track within tens of ms during the transient and within 10 ms once
// settled.
static void test_startup_rate_transient()
{
  CaptureTimestamper t(16000);
  const uint64_t base = 9000000;
  double true_capture = base;  // real (monotonic) time of each chunk's first sample
  uint64_t prev = 0;
  double max_err_transient = 0, max_err_settled = 0;
  for (int i = 0; i < 250; ++i) {
    double ratio = i < 75 ? 1.09 : 1.0;  // 75 chunks = 6 s of audio
    double arrival = true_capture + 160000.0 - ((i % 8) == 7 ? 40000.0 : 0.0);
    double lat = 160000.0 + ((i % 8) - 3.5) * 5000.0;
    auto o = t.on_chunk(1280, static_cast<uint64_t>(arrival), static_cast<int64_t>(lat));
    CHECK(o.timestamp_usec != 0);
    CHECK(o.timestamp_usec >= prev);
    CHECK(!o.resynced);
    double err = static_cast<double>(o.timestamp_usec) - true_capture;
    err = err < 0 ? -err : err;
    if (i < 140)
      max_err_transient = err > max_err_transient ? err : max_err_transient;
    else
      max_err_settled = err > max_err_settled ? err : max_err_settled;
    prev = o.timestamp_usec;
    true_capture += 80000.0 / ratio;
  }
  std::printf("transient test: max |err| during/after transient %.1f / %.1f us\n", max_err_transient,
      max_err_settled);
  CHECK(max_err_transient < 40000);
  CHECK(max_err_settled < 10000);
}

static void test_invalid_input()
{
  CaptureTimestamper zero_rate(0);
  CHECK(zero_rate.on_chunk(100, 123456, 0).timestamp_usec == 0);
  CaptureTimestamper t(16000);
  CHECK(t.on_chunk(100, 0, 0).timestamp_usec == 0);  // no arrival time = no stamp
}

static void test_map_and_split()
{
  int64_t out = 0;
  // base: mono 1000 s <-> ROS 1.7e9 s (2023-ish epoch)
  const int64_t base_mono = 1000LL * 1000000000LL;
  const int64_t base_ros = 1700000000LL * 1000000000LL;
  CHECK(map_monotonic_to_clock_ns(1000500000ull, base_mono, base_ros, &out));  // +0.5 s
  CHECK(out == base_ros + 500000000LL);
  CHECK(map_monotonic_to_clock_ns(999000000ull, base_mono, base_ros, &out));  // -1 s
  CHECK(out == base_ros - 1000000000LL);
  CHECK(!map_monotonic_to_clock_ns(0, base_mono, base_ros, &out));  // invalid sentinel
  CHECK(!map_monotonic_to_clock_ns(std::numeric_limits<uint64_t>::max(), base_mono, base_ros, &out));
  CHECK(!map_monotonic_to_clock_ns(2000000ull, 0, std::numeric_limits<int64_t>::max(), &out));

  int32_t sec = -1;
  uint32_t nsec = 1;
  CHECK(split_time_ns(1700000000123456789LL, &sec, &nsec));
  CHECK(sec == 1700000000 && nsec == 123456789u);
  CHECK(split_time_ns(0, &sec, &nsec) && sec == 0 && nsec == 0);
  CHECK(!split_time_ns(-1, &sec, &nsec));
  CHECK(!split_time_ns((static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1) * 1000000000LL,
      &sec, &nsec));
}

static void test_monotonic_now()
{
  uint64_t a = monotonic_now_usec();
  uint64_t b = monotonic_now_usec();
  CHECK(a != 0 && b >= a);
}

int main()
{
  test_steady_stream();
  test_drift_and_jitter_are_absorbed();
  test_gap_resyncs_forward();
  test_backward_jump_stays_monotonic();
  test_unknown_latency();
  test_bogus_first_latency();
  test_startup_rate_transient();
  test_invalid_input();
  test_map_and_split();
  test_monotonic_now();
  if (g_failures) {
    std::fprintf(stderr, "core_capture_timestamp_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("core_capture_timestamp_test: all checks passed\n");
  return 0;
}
