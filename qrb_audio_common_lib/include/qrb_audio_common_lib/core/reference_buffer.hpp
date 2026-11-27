// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__REFERENCE_BUFFER_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__REFERENCE_BUFFER_HPP_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Echo-cancellation reference ("what the speaker played", e.g. the default
// sink's monitor source) handed from the reference capture stream to the AEC
// stage on the capture worker thread. Mono int16 at the mic stream's rate.
//
// Pairing rule (stage-6 A/B, aec3_ab_test.md): AEC3 tolerates a constant
// render->capture delay of any size it can estimate, recovers within ~1 s when
// the reference moves EARLIER, but needs >3 s when the reference moves LATER.
// So reference and mic are paired on the SAMPLE timeline, not by when chunks
// happen to arrive (Pulse delivers in bursts):
//   - The first fetch after (re)sync places the read position by capture
//     timestamps: the reference sample captured at the mic block's capture
//     time, plus `lead_ms` towards newer reference (biased early, never late).
//   - After that the read position advances by exactly one block per mic
//     block, independent of arrival jitter.
//   - Re-sync (by timestamps) only when the alignment is known to be broken:
//     a gap in the reference stream (push(discontinuity=true)), a gap in the
//     mic stream (fetch(mic_discontinuity=true)), the reference coming back
//     after a stall, or the sample-count position drifting more than
//     `resync_threshold_ms` away from the timestamp-derived one (0 = never
//     re-sync on drift: trust sample counting, e.g. when mic and speaker are
//     known to share one hardware clock and only the timestamps wander).
//     On QCS9075 the real mic vs speaker-monitor timestamps drift ~5-8 ms/s
//     (and ~37 ms/s for ~15 s after a cold mic start); which side the real
//     acoustic echo follows is still to be measured (test_report 5k).
//
// Unavailable (stall / never started / failed / no timestamps yet): fetch()
// fills zeros and returns false; the AEC stage then passes audio through and
// flags the block kBypassed. fetch() waits at most `max_wait_ms` for samples
// that are due but not delivered yet - and only while the reference is live
// (a push within `stale_ms`), so a dead reference never slows the mic path.
//
// Thread-safety: push()/mark_failed() from the reference stream's thread,
// fetch() from the capture worker; everything is under one mutex, and the
// critical sections are a memcpy.
class ReferenceBuffer
{
public:
  struct Options
  {
    uint32_t capacity_ms = 2000;
    uint32_t lead_ms = 5;
    uint32_t max_wait_ms = 60;
    uint32_t stale_ms = 200;
    uint32_t resync_threshold_ms = 40;
  };

  struct Stats
  {
    uint64_t frames_pushed = 0;
    uint64_t blocks_fetched = 0;      // fetch() calls that returned reference
    uint64_t blocks_unavailable = 0;  // fetch() calls that returned zeros
    uint64_t syncs = 0;               // initial sync + re-syncs
    uint64_t drift_resyncs = 0;       // re-syncs triggered by drift beyond the threshold
    uint64_t waits = 0;               // fetch() calls that had to wait for data
    uint64_t pushes_with_gap = 0;
    int64_t last_sync_offset_frames = 0;  // read pos - timestamp target at last sync
    // At the last drift re-sync: sample-count position minus timestamp target
    // (> 0: counting had run ahead = reference was being paired too new).
    int64_t last_drift_frames = 0;
  };

  using Clock = std::chrono::steady_clock;

  ReferenceBuffer(uint32_t sample_rate, Options options)
    : rate_(sample_rate ? sample_rate : 1)
    , opt_(options)
    , ring_(static_cast<size_t>(rate_) * std::max<uint32_t>(opt_.capacity_ms, 100) / 1000, 0)
  {
  }
  explicit ReferenceBuffer(uint32_t sample_rate) : ReferenceBuffer(sample_rate, Options{}) {}

  uint32_t sample_rate() const { return rate_; }

  // Forget everything (new stream run). Keeps the options.
  void reset()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    write_pos_ = 0;
    read_pos_ = 0;
    synced_ = false;
    behind_ = false;
    have_anchor_ = false;
    failed_ = false;
    ever_pushed_ = false;
    stats_ = Stats{};
  }

  // Producer: `frames` mono samples whose first sample was captured at
  // `ts_first_usec` (CLOCK_MONOTONIC, 0 = unknown). `discontinuity`: samples
  // were lost right before this chunk.
  void push(const int16_t * samples, size_t frames, uint64_t ts_first_usec, bool discontinuity)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = false;
      if (discontinuity || stalled_locked(Clock::now())) {
        // Sample counting can't be trusted across a hole or a stall.
        synced_ = false;
        if (discontinuity)
          ++stats_.pushes_with_gap;
      }
      if (ts_first_usec != 0) {
        anchor_pos_ = write_pos_;
        anchor_ts_usec_ = ts_first_usec;
        have_anchor_ = true;
      }
      const size_t cap = ring_.size();
      for (size_t done = 0; done < frames;) {
        const size_t at = static_cast<size_t>((write_pos_ + done) % cap);
        const size_t n = std::min(frames - done, cap - at);
        std::memcpy(&ring_[at], samples + done, n * sizeof(int16_t));
        done += n;
      }
      write_pos_ += frames;
      behind_ = false;
      stats_.frames_pushed += frames;
      last_push_ = Clock::now();
      ever_pushed_ = true;
    }
    cv_.notify_all();
  }

  // Producer: the reference stream died (device gone, connection lost).
  void mark_failed()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
      synced_ = false;
    }
    cv_.notify_all();
  }

  // Consumer: reference for the mic block of `frames` samples whose first
  // sample was captured at `mic_ts_usec` (0 = unknown). Returns true and fills
  // `out` when reference is available; false + zeros otherwise.
  bool fetch(uint64_t mic_ts_usec, bool mic_discontinuity, int16_t * out, size_t frames)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (mic_discontinuity)
      synced_ = false;
    const auto now = Clock::now();
    if (failed_ || !ever_pushed_ || stalled_locked(now) || frames > ring_.size()) {
      synced_ = false;
      return unavailable(out, frames);
    }

    int64_t target = 0;
    const bool have_target = timestamp_target_locked(mic_ts_usec, target);
    if (!synced_) {
      if (!have_target)
        return unavailable(out, frames);  // need timestamps to place the first block
      read_pos_ = clamp_to_window_locked(target, frames);
      synced_ = true;
      ++stats_.syncs;
      stats_.last_sync_offset_frames = static_cast<int64_t>(read_pos_) - target;
    } else if (have_target) {
      const int64_t drift = static_cast<int64_t>(read_pos_) - target;
      const int64_t limit = static_cast<int64_t>(rate_) * opt_.resync_threshold_ms / 1000;
      if (opt_.resync_threshold_ms != 0 && (drift > limit || drift < -limit)) {
        read_pos_ = clamp_to_window_locked(target, frames);
        ++stats_.syncs;
        ++stats_.drift_resyncs;
        stats_.last_drift_frames = drift;
        stats_.last_sync_offset_frames = static_cast<int64_t>(read_pos_) - target;
      }
    }

    // Samples due but not delivered yet: wait (bounded) while the reference
    // is live - but only if what is missing could plausibly arrive within the
    // wait budget. A deficit larger than that (reference stalled or running
    // far behind) is served as unavailable at once, so a timed-out wait can
    // happen at most about once before the deficit exceeds the budget; the
    // worker is never slowed to a crawl by a sick reference.
    if (read_pos_ + frames > write_pos_) {
      const uint64_t deficit = read_pos_ + frames - write_pos_;
      // behind_: the previous wait already timed out and nothing arrived
      // since - don't wait again for every following block.
      if (behind_ || deficit * 1000 > static_cast<uint64_t>(rate_) * opt_.max_wait_ms) {
        read_pos_ += frames;
        return unavailable(out, frames);
      }
      ++stats_.waits;
      const auto deadline = now + std::chrono::milliseconds(opt_.max_wait_ms);
      cv_.wait_until(
          lock, deadline, [&] { return read_pos_ + frames <= write_pos_ || failed_ || !synced_; });
      if (failed_ || !synced_ || read_pos_ + frames > write_pos_) {
        // Keep the sample clock running so later blocks stay paired.
        if (synced_)
          read_pos_ += frames;
        behind_ = true;
        return unavailable(out, frames);
      }
    }
    // Overwritten already (consumer fell more than capacity behind).
    if (write_pos_ - read_pos_ > ring_.size()) {
      read_pos_ = write_pos_ - ring_.size();
    }
    const size_t cap = ring_.size();
    for (size_t done = 0; done < frames;) {
      const size_t at = static_cast<size_t>((read_pos_ + done) % cap);
      const size_t n = std::min(frames - done, cap - at);
      std::memcpy(out + done, &ring_[at], n * sizeof(int16_t));
      done += n;
    }
    read_pos_ += frames;
    ++stats_.blocks_fetched;
    return true;
  }

  // True while the last fetch() would have been served (for status logs).
  bool live() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return ever_pushed_ && !failed_ && !stalled_locked(Clock::now());
  }

  Stats stats() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

private:
  bool stalled_locked(Clock::time_point now) const
  {
    return ever_pushed_ && now - last_push_ > std::chrono::milliseconds(opt_.stale_ms);
  }

  // Ring index of the reference sample captured at mic_ts, plus the lead.
  bool timestamp_target_locked(uint64_t mic_ts_usec, int64_t & target) const
  {
    if (mic_ts_usec == 0 || !have_anchor_)
      return false;
    const int64_t dt_usec =
        static_cast<int64_t>(mic_ts_usec) - static_cast<int64_t>(anchor_ts_usec_);
    const int64_t lead_usec = static_cast<int64_t>(opt_.lead_ms) * 1000;
    // Round towards newer reference (early bias).
    const int64_t num = (dt_usec + lead_usec) * static_cast<int64_t>(rate_);
    int64_t offset = num / 1000000;
    if (num % 1000000 > 0)
      ++offset;
    target = static_cast<int64_t>(anchor_pos_) + offset;
    return true;
  }

  // Keeps the read position inside what the ring still holds; a target older
  // than the ring moves forward (towards newer = earlier reference, the safe
  // direction for AEC3).
  uint64_t clamp_to_window_locked(int64_t target, size_t frames) const
  {
    const int64_t oldest = static_cast<int64_t>(write_pos_) - static_cast<int64_t>(ring_.size());
    if (target < oldest)
      target = oldest;
    if (target < 0)
      target = 0;
    (void)frames;
    return static_cast<uint64_t>(target);
  }

  bool unavailable(int16_t * out, size_t frames)
  {
    std::memset(out, 0, frames * sizeof(int16_t));
    ++stats_.blocks_unavailable;
    return false;
  }

  const uint32_t rate_;
  const Options opt_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<int16_t> ring_;
  uint64_t write_pos_ = 0;
  uint64_t read_pos_ = 0;
  bool synced_ = false;
  bool behind_ = false;
  bool have_anchor_ = false;
  uint64_t anchor_pos_ = 0;
  uint64_t anchor_ts_usec_ = 0;
  bool failed_ = false;
  bool ever_pushed_ = false;
  Clock::time_point last_push_{};
  Stats stats_;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__REFERENCE_BUFFER_HPP_
