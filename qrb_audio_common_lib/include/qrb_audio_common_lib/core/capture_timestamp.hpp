// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__CAPTURE_TIMESTAMP_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__CAPTURE_TIMESTAMP_HPP_

#include <time.h>

#include <cstdint>
#include <limits>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Time domain used by AudioFrame::timestamp_usec: CLOCK_MONOTONIC, in
// microseconds. Never jumps (no NTP steps), does not count suspend, and is
// the same clock as pa_rtclock_now() and libstdc++'s std::chrono::steady_clock
// on Linux. It is NOT wall-clock / Unix epoch time and NOT ROS time; the ROS
// layer maps it onto its own clock (see map_monotonic_to_clock_ns()).
inline uint64_t monotonic_now_usec()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

// Turns "a chunk of N sample frames was just read" into "the first sample of
// that chunk was captured at monotonic time T".
//
// Per chunk the backend supplies an arrival time and (usually) the
// server-reported record latency, giving a raw estimate
//   est = arrival - latency          (or arrival - chunk duration if unknown)
// of when the chunk's first sample was captured. Measured on QCS9075 +
// pipewire-pulse 1.0.5 (test_report.md 5h):
// - per chunk neither input is accurate: arrivals bunch up to PipeWire
//   quantum boundaries (+-40 ms) and the reported latency is a sawtooth
//   (+-20 ms);
// - the FIRST chunk's latency is bogus (~0.2 ms, timing info not yet valid);
// - for the first ~6 s the stream delivers 5-9 % more samples than real time
//   (PipeWire rate matching converging), then ~1.000. A pure sample-count
//   timeline would run up to ~9 % fast during that phase.
//
// So the timestamp is a least-squares line fitted over a short sliding window
// (default 2 s, to follow the startup transient) of (cumulative frames, est)
// points:
//   ts = a + b * frames_before_this_chunk
// - b is the measured real duration per frame: drift is tracked, steps are
//   smooth (chunk duration / measured clock ratio), jitter averages out.
// - A point deviating from the prediction by more than gap_threshold_usec
//   (default 100 ms, well above the observed jitter) is a real discontinuity
//   (server dropped audio on overflow, or a clock jump): the window restarts
//   from that point; a forward jump is reported as gap_detected.
// - A latency shorter than the chunk itself is physically impossible (the
//   whole chunk is still buffered when it is read) and is treated as
//   unknown. Unknown-latency chunks are not added to the fit; they get the
//   current prediction, or timestamp 0 (invalid) if there is no valid point
//   yet - typically just the very first chunk of a stream.
// - Output is a slew-limited servo onto that line: each step is the measured
//   chunk duration with at most +-5 % correction, so steps stay smooth while
//   the fit wobbles. Right after start (before 0.5 s of data gives a measured
//   slope) and during the rate transient the stamps can lag the fit by a few
//   tens of ms and converge within seconds.
// - Output is clamped to be non-decreasing; b is clamped to +-15 % of nominal.
//
// Pure (no clock reads, no I/O, no allocation after the window fills) so it
// is unit-testable with synthetic input.
class CaptureTimestamper
{
public:
  static constexpr int64_t kUnknownLatency = -1;

  struct Output
  {
    uint64_t timestamp_usec = 0;  // 0 = invalid
    bool resynced = false;        // window restarted at this chunk
    bool gap_detected = false;    // restarted forward: audio was lost before this chunk
  };

  explicit CaptureTimestamper(uint32_t sample_rate,
      uint64_t window_usec = 2000000,
      uint64_t gap_threshold_usec = 100000)
    : sample_rate_(sample_rate), window_usec_(window_usec), gap_threshold_usec_(gap_threshold_usec)
  {
  }

  Output on_chunk(uint64_t frames_in_chunk, uint64_t arrival_mono_usec, int64_t latency_usec)
  {
    Output out;
    if (sample_rate_ == 0 || arrival_mono_usec == 0)
      return out;

    const double nominal = 1e6 / sample_rate_;  // usec per frame
    const double chunk_usec = frames_in_chunk * nominal;
    const uint64_t x = frames_total_;
    const bool latency_ok =
        latency_usec >= 0 && static_cast<double>(latency_usec) >= kMinLatencyOfChunk * chunk_usec;

    if (!latency_ok) {
      // No trustworthy capture-time estimate for this chunk: don't feed the
      // fit. Extrapolate if we already have a timeline, else no timestamp.
      if (count_ == 0) {
        frames_total_ += frames_in_chunk;
        return out;  // timestamp_usec = 0
      }
      return emit(x, frames_in_chunk, out);
    }
    uint64_t age = static_cast<uint64_t>(latency_usec);
    uint64_t est = arrival_mono_usec > age ? arrival_mono_usec - age : 1;

    if (count_ > 0) {
      double predicted = predict(x);
      double residual = static_cast<double>(est) - predicted;
      if (residual > static_cast<double>(gap_threshold_usec_) ||
          residual < -static_cast<double>(gap_threshold_usec_)) {
        out.resynced = true;
        out.gap_detected = residual > 0;
        resync_count_++;
        count_ = 0;  // restart the window from this point
      }
    }
    if (count_ == 0) {
      base_x_ = x;
      base_y_ = est;
    }
    push(static_cast<double>(x - base_x_), static_cast<double>(est) - static_cast<double>(base_y_));
    fit(nominal);
    return emit(x, frames_in_chunk, out);
  }

  uint64_t resync_count() const { return resync_count_; }
  // Measured usec per frame divided by nominal (1.0 = audio clock matches
  // CLOCK_MONOTONIC; < 1.0 = audio delivered faster than real time).
  double clock_ratio() const { return sample_rate_ ? slope_ * sample_rate_ / 1e6 : 0.0; }

private:
  static constexpr size_t kMaxPoints = 512;
  static constexpr double kMaxSlew = 0.05;
  static constexpr double kMinFitSpanUsec = 500000;  // trust a measured slope after 0.5 s of audio
  static constexpr double kMinLatencyOfChunk = 0.9;  // latency < 90 % of chunk = bogus

  Output & emit(uint64_t x, uint64_t frames_in_chunk, Output & out)
  {
    double pred = predict(x);
    double ts_d;
    if (count_ == 1 || last_ts_ == 0) {
      ts_d = pred;  // window (re)started: jump straight to the estimate
    } else {
      // Slew-limited servo towards the fitted line: advance by the measured
      // duration of the previous chunk, plus a correction capped at
      // kMaxSlew of that duration. Keeps steps within +-1 % even while the
      // fit wobbles as periodic arrival patterns slide through the window.
      double step = slope_ * static_cast<double>(last_frames_);
      double expected = static_cast<double>(last_ts_) + step;
      double corr = pred - expected;
      double lim = kMaxSlew * step;
      corr = corr > lim ? lim : (corr < -lim ? -lim : corr);
      ts_d = expected + corr;
    }
    uint64_t ts = ts_d < 1.0 ? 1 : static_cast<uint64_t>(ts_d + 0.5);
    if (ts < last_ts_)
      ts = last_ts_;  // never go backwards
    last_ts_ = ts;
    last_frames_ = frames_in_chunk;
    frames_total_ += frames_in_chunk;
    out.timestamp_usec = ts;
    return out;
  }

  double predict(uint64_t x) const
  {
    return static_cast<double>(base_y_) + intercept_ +
           slope_ * static_cast<double>(x - base_x_);
  }

  void push(double px, double py)
  {
    // Drop points older than the window (by x span, i.e. audio time) or
    // beyond capacity; ring buffer, oldest at head_.
    while (count_ > 0 && (count_ == kMaxPoints ||
                          (px - xs_[head_]) * (1e6 / sample_rate_) > static_cast<double>(window_usec_))) {
      head_ = (head_ + 1) % kMaxPoints;
      count_--;
    }
    if (count_ == 0)
      head_ = 0;
    size_t idx = (head_ + count_) % kMaxPoints;
    xs_[idx] = px;
    ys_[idx] = py;
    count_++;
  }

  void fit(double nominal)
  {
    double mx = 0, my = 0;
    for (size_t i = 0; i < count_; ++i) {
      size_t k = (head_ + i) % kMaxPoints;
      mx += xs_[k];
      my += ys_[k];
    }
    mx /= count_;
    my /= count_;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < count_; ++i) {
      size_t k = (head_ + i) % kMaxPoints;
      sxx += (xs_[k] - mx) * (xs_[k] - mx);
      sxy += (xs_[k] - mx) * (ys_[k] - my);
    }
    double b = nominal;
    // Need half a second of audio in the window before trusting a
    // measured slope; until then assume the nominal rate.
    double span_usec = count_ > 1 ? (xs_[(head_ + count_ - 1) % kMaxPoints] - xs_[head_]) * nominal : 0;
    if (count_ >= 3 && sxx > 0 && span_usec >= kMinFitSpanUsec) {
      b = sxy / sxx;
      if (b < 0.85 * nominal)
        b = 0.85 * nominal;
      if (b > 1.15 * nominal)
        b = 1.15 * nominal;
    }
    slope_ = b;
    intercept_ = my - b * mx;
  }

  uint32_t sample_rate_;
  uint64_t window_usec_;
  uint64_t gap_threshold_usec_;

  double xs_[kMaxPoints] = {};
  double ys_[kMaxPoints] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  uint64_t base_x_ = 0;  // frames at window start
  uint64_t base_y_ = 0;  // est at window start (usec)
  double slope_ = 0;     // usec per frame
  double intercept_ = 0;

  uint64_t frames_total_ = 0;
  uint64_t last_frames_ = 0;
  uint64_t last_ts_ = 0;
  uint64_t resync_count_ = 0;
};

// Maps a CLOCK_MONOTONIC microsecond timestamp onto a target clock using a
// base pair (base_mono_ns, base_target_ns) sampled back-to-back:
//   T_target = base_target + (ts_mono - base_mono)
// Returns false (and leaves *out_ns untouched) on invalid input or overflow.
inline bool map_monotonic_to_clock_ns(uint64_t ts_mono_usec,
    int64_t base_mono_ns,
    int64_t base_target_ns,
    int64_t * out_ns)
{
  if (ts_mono_usec == 0 || out_ns == nullptr)
    return false;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  if (ts_mono_usec > static_cast<uint64_t>(kMax / 1000))
    return false;
  int64_t ts_ns = static_cast<int64_t>(ts_mono_usec) * 1000;
  int64_t delta = ts_ns - base_mono_ns;  // both well inside int64 range for monotonic time
  if ((delta > 0 && base_target_ns > kMax - delta) ||
      (delta < 0 && base_target_ns < std::numeric_limits<int64_t>::min() - delta))
    return false;
  *out_ns = base_target_ns + delta;
  return true;
}

// Splits nanoseconds into builtin_interfaces/Time's (int32 sec, uint32 nanosec)
// without depending on ROS headers. Returns false for negative times or
// seconds beyond int32 range (caller then publishes a zero stamp).
inline bool split_time_ns(int64_t ns, int32_t * sec, uint32_t * nanosec)
{
  if (ns < 0)
    return false;
  int64_t s = ns / 1000000000;
  if (s > std::numeric_limits<int32_t>::max())
    return false;
  *sec = static_cast<int32_t>(s);
  *nanosec = static_cast<uint32_t>(ns % 1000000000);
  return true;
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__CAPTURE_TIMESTAMP_HPP_
