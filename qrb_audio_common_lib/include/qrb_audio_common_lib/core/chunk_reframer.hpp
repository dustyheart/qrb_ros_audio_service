// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__CHUNK_REFRAMER_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__CHUNK_REFRAMER_HPP_

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "qrb_audio_common_lib/core/audio_frame.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Bridges "whatever chunk size the backend delivers" and "the fixed block
// size a processor needs" (WebRTC APM: exactly 10 ms), in both directions:
//
//   capture chunks (any size) -> fixed blocks -> BlockFn (process in place)
//                             -> re-assembled chunks with the ORIGINAL sizes,
//                                sequence, timestamp_usec and flags -> ChunkFn
//
// Re-assembling to the original chunks means subscribers see exactly the
// same message sizes and timestamps whether processing is on or off; the
// only cost is up to one block of extra latency (a chunk is emitted once the
// block containing its last sample has been processed).
//
// Blocks get their own timestamp: the stamp of the chunk holding the block's
// first sample plus the sample offset into that chunk (0 stays 0 = invalid).
// A block is flagged kDropped if any chunk starting inside it was. Any flag a
// BlockFn adds (e.g. kBypassed) is OR-ed into every chunk the block overlaps.
//
// Pure and single-threaded; no clock reads. Assumes every chunk holds whole
// sample frames (true for PipeWirePulseBackend).
class ChunkReframer
{
public:
  using BlockFn = std::function<void(AudioFrame & block)>;
  using ChunkFn = std::function<void(AudioFrame && chunk)>;

  // `block_frames` samples per channel per block; `bytes_per_frame` =
  // channels * bytes per sample. Both must be non-zero.
  ChunkReframer(uint32_t block_frames, uint32_t bytes_per_frame)
    : block_frames_(block_frames)
    , bytes_per_frame_(bytes_per_frame)
    , block_bytes_(static_cast<size_t>(block_frames) * bytes_per_frame)
  {
  }

  void push(const AudioFrame & chunk, const BlockFn & on_block, const ChunkFn & on_chunk)
  {
    Pending p;
    p.meta = chunk;
    p.meta.data.reset();
    p.start_frame = total_in_frames_;
    p.bytes = chunk.byte_size();
    total_in_frames_ += p.bytes / bytes_per_frame_;
    pending_.push_back(p);
    if (p.bytes > 0)
      in_.insert(in_.end(), chunk.data->begin(), chunk.data->end());

    while (in_.size() - in_off_ >= block_bytes_) {
      run_block(block_bytes_, on_block);
    }
    compact();
    emit_ready(on_chunk);
  }

  // Emits every pending chunk. A tail shorter than one block cannot be
  // processed, so it is passed through untouched and its chunk(s) are flagged
  // kBypassed. Call once at end of stream.
  void flush(const ChunkFn & on_chunk)
  {
    const size_t tail = in_.size() - in_off_;
    if (tail > 0) {
      const uint64_t tail_start = processed_frames_;
      out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(in_off_), in_.end());
      processed_frames_ += tail / bytes_per_frame_;
      mark_overlap(tail_start, processed_frames_, static_cast<uint32_t>(FrameFlag::kBypassed));
    }
    in_.clear();
    in_off_ = 0;
    emit_ready(on_chunk);
  }

  uint32_t block_frames() const { return block_frames_; }
  uint64_t blocks_processed() const { return blocks_processed_; }
  size_t pending_chunks() const { return pending_.size(); }

private:
  struct Pending
  {
    AudioFrame meta;  // original chunk minus data
    uint64_t start_frame = 0;
    size_t bytes = 0;
    uint32_t extra_flags = 0;
  };

  void run_block(size_t bytes, const BlockFn & on_block)
  {
    const uint64_t first = processed_frames_;
    const uint64_t last = first + bytes / bytes_per_frame_;

    if (!block_buf_ || block_buf_.use_count() != 1)
      block_buf_ = std::make_shared<std::vector<uint8_t>>();  // a BlockFn kept a reference
    block_buf_->assign(in_.begin() + static_cast<std::ptrdiff_t>(in_off_),
        in_.begin() + static_cast<std::ptrdiff_t>(in_off_ + bytes));
    in_off_ += bytes;

    AudioFrame block;
    block.data = block_buf_;
    block.frame_count = static_cast<uint32_t>(bytes / bytes_per_frame_);
    block.sequence = blocks_processed_;
    for (const auto & p : pending_) {
      const uint64_t p_end = p.start_frame + p.bytes / bytes_per_frame_;
      if (p.start_frame <= first && first < p_end) {
        block.sample_rate = p.meta.sample_rate;
        block.channels = p.meta.channels;
        block.sample_format = p.meta.sample_format;
        if (p.meta.timestamp_usec != 0 && p.meta.sample_rate != 0) {
          block.timestamp_usec =
              p.meta.timestamp_usec + (first - p.start_frame) * 1000000ull / p.meta.sample_rate;
        }
      }
      if (p.start_frame >= first && p.start_frame < last &&
          has_flag(p.meta.flags, FrameFlag::kDropped)) {
        block.flags |= static_cast<uint32_t>(FrameFlag::kDropped);
      }
    }
    const uint32_t flags_in = block.flags;

    on_block(block);
    ++blocks_processed_;

    // A processor must not change the length; if one did, fall back to the
    // unprocessed samples rather than corrupt the chunk boundaries.
    const uint8_t * src = in_.data() + in_off_ - bytes;
    uint32_t added = block.flags & ~flags_in;
    if (block.data && block.data->size() == bytes) {
      src = block.data->data();
    } else {
      added |= static_cast<uint32_t>(FrameFlag::kBypassed);
    }
    out_.insert(out_.end(), src, src + bytes);
    processed_frames_ = last;
    mark_overlap(first, last, added & ~static_cast<uint32_t>(FrameFlag::kDropped));
  }

  void mark_overlap(uint64_t first, uint64_t last, uint32_t flags)
  {
    if (flags == 0)
      return;
    for (auto & p : pending_) {
      const uint64_t p_end = p.start_frame + p.bytes / bytes_per_frame_;
      if (p.start_frame < last && p_end > first)
        p.extra_flags |= flags;
    }
  }

  void emit_ready(const ChunkFn & on_chunk)
  {
    while (!pending_.empty() && out_.size() - out_off_ >= pending_.front().bytes) {
      Pending p = std::move(pending_.front());
      pending_.pop_front();
      AudioFrame chunk = std::move(p.meta);
      chunk.flags |= p.extra_flags;
      chunk.data = std::make_shared<std::vector<uint8_t>>(
          out_.begin() + static_cast<std::ptrdiff_t>(out_off_),
          out_.begin() + static_cast<std::ptrdiff_t>(out_off_ + p.bytes));
      out_off_ += p.bytes;
      on_chunk(std::move(chunk));
    }
    // Always compact: with processing on there is usually a partial chunk
    // left in out_, so "clear when empty" alone would let it grow forever.
    if (out_off_ > 0) {
      out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_off_));
      out_off_ = 0;
    }
  }

  void compact()
  {
    if (in_off_ > 0) {
      in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(in_off_));
      in_off_ = 0;
    }
  }

  const uint32_t block_frames_;
  const uint32_t bytes_per_frame_;
  const size_t block_bytes_;

  std::vector<uint8_t> in_;  // received, not yet processed
  size_t in_off_ = 0;
  std::vector<uint8_t> out_;  // processed, not yet emitted
  size_t out_off_ = 0;
  std::deque<Pending> pending_;
  std::shared_ptr<std::vector<uint8_t>> block_buf_;

  uint64_t total_in_frames_ = 0;
  uint64_t processed_frames_ = 0;
  uint64_t blocks_processed_ = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__CHUNK_REFRAMER_HPP_
