// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// ChunkReframer: arbitrary chunk sizes in -> fixed blocks to the processor ->
// original chunk sizes/metadata out. No hardware, no ROS.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "qrb_audio_common_lib/core/chunk_reframer.hpp"

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
constexpr uint32_t kBlock = 160;  // 10 ms @ 16 kHz
constexpr uint32_t kBpf = 2;      // mono S16

// Chunk whose samples are their global sample index (mod 2^15).
AudioFrame make_chunk(uint32_t frames, uint64_t first_index, uint64_t seq, uint64_t ts)
{
  AudioFrame f;
  f.sample_rate = kRate;
  f.channels = 1;
  f.sample_format = 16;
  f.frame_count = frames;
  f.sequence = seq;
  f.timestamp_usec = ts;
  f.data = std::make_shared<std::vector<uint8_t>>(frames * kBpf);
  auto * s = reinterpret_cast<int16_t *>(f.data->data());
  for (uint32_t i = 0; i < frames; ++i)
    s[i] = static_cast<int16_t>((first_index + i) & 0x7fff);
  return f;
}

int16_t sample(const AudioFrame & f, size_t i)
{
  int16_t v;
  std::memcpy(&v, f.data->data() + i * 2, 2);
  return v;
}

// +1 to every sample, so "was this sample processed" is visible in output.
void add_one(AudioFrame & b)
{
  auto * s = reinterpret_cast<int16_t *>(b.data->data());
  for (uint32_t i = 0; i < b.frame_count; ++i)
    s[i] = static_cast<int16_t>(s[i] + 1);
}

void test_aligned_chunks_pass_straight_through()
{
  ChunkReframer r(kBlock, kBpf);
  std::vector<AudioFrame> out;
  int blocks = 0;
  for (int c = 0; c < 3; ++c) {
    r.push(
        make_chunk(1280, c * 1280ull, c, 1000000 + c * 80000ull),
        [&](AudioFrame & b) {
          CHECK(b.frame_count == kBlock);
          ++blocks;
          add_one(b);
        },
        [&](AudioFrame && f) { out.push_back(std::move(f)); });
    // 80 ms chunk = exactly 8 blocks: nothing is held back.
    CHECK(out.size() == static_cast<size_t>(c + 1));
  }
  CHECK(blocks == 24);
  for (int c = 0; c < 3; ++c) {
    CHECK(out[c].frame_count == 1280);
    CHECK(out[c].byte_size() == 1280 * kBpf);
    CHECK(out[c].sequence == static_cast<uint64_t>(c));
    CHECK(out[c].timestamp_usec == 1000000 + c * 80000ull);
    CHECK(out[c].flags == 0);
    CHECK(sample(out[c], 0) == static_cast<int16_t>(c * 1280 + 1));
  }
  std::printf("PASS test_aligned_chunks_pass_straight_through\n");
}

void test_irregular_chunks_keep_sizes_order_and_content()
{
  const uint32_t sizes[] = {100, 250, 37, 333, 160, 1, 519};
  ChunkReframer r(kBlock, kBpf);
  std::vector<AudioFrame> out;
  uint64_t idx = 0, total = 0;
  for (size_t c = 0; c < sizeof(sizes) / sizeof(sizes[0]); ++c) {
    r.push(make_chunk(sizes[c], idx, c, 0), add_one,
        [&](AudioFrame && f) { out.push_back(std::move(f)); });
    idx += sizes[c];
    total += sizes[c];
    CHECK(r.pending_chunks() <= 3);  // never more than ~one block of chunks held
  }
  r.flush([&](AudioFrame && f) { out.push_back(std::move(f)); });
  CHECK(out.size() == sizeof(sizes) / sizeof(sizes[0]));

  const uint64_t processed = (total / kBlock) * kBlock;
  uint64_t g = 0;
  for (size_t c = 0; c < out.size(); ++c) {
    CHECK(out[c].sequence == c);
    CHECK(out[c].frame_count == sizes[c]);
    CHECK(out[c].byte_size() == sizes[c] * kBpf);
    CHECK(out[c].timestamp_usec == 0);  // 0 in -> 0 out, never invented
    bool has_tail = false;
    for (uint32_t i = 0; i < sizes[c]; ++i, ++g) {
      const int16_t want = static_cast<int16_t>((g & 0x7fff) + (g < processed ? 1 : 0));
      CHECK(sample(out[c], i) == want);
      has_tail |= g >= processed;
    }
    // Only chunks holding unprocessed tail samples are flagged bypassed.
    CHECK(has_flag(out[c].flags, FrameFlag::kBypassed) == has_tail);
  }
  std::printf("PASS test_irregular_chunks_keep_sizes_order_and_content\n");
}

void test_block_timestamps_and_dropped_flag()
{
  ChunkReframer r(kBlock, kBpf);
  std::vector<AudioFrame> blocks, out;
  auto keep = [&](AudioFrame & b) { blocks.push_back(b); };
  auto sink = [&](AudioFrame && f) { out.push_back(std::move(f)); };

  // Chunk A: 250 frames @ t=1 s. Chunk B: 250 frames after a gap, t=2 s,
  // flagged kDropped by the backend.
  r.push(make_chunk(250, 0, 0, 1000000), keep, sink);
  AudioFrame b = make_chunk(250, 250, 1, 2000000);
  b.flags = static_cast<uint32_t>(FrameFlag::kDropped);
  r.push(b, keep, sink);

  CHECK(blocks.size() == 3);  // 500 frames -> 3 full blocks
  CHECK(blocks[0].timestamp_usec == 1000000);
  CHECK(blocks[1].timestamp_usec == 1000000 + 160 * 1000000ull / kRate);  // +10 ms
  // Block 2 starts at frame 320 = 70 frames into chunk B.
  CHECK(blocks[2].timestamp_usec == 2000000 + 70 * 1000000ull / kRate);
  CHECK(!has_flag(blocks[0].flags, FrameFlag::kDropped));
  CHECK(has_flag(blocks[1].flags, FrameFlag::kDropped));  // chunk B starts in block 1
  CHECK(!has_flag(blocks[2].flags, FrameFlag::kDropped));
  CHECK(blocks[0].sample_rate == kRate && blocks[0].channels == 1);

  CHECK(out.size() == 1);  // A complete; B's last 20 frames still pending
  CHECK(out[0].timestamp_usec == 1000000);
  CHECK(!has_flag(out[0].flags, FrameFlag::kDropped));  // the block's drop flag stays on the block
  r.flush(sink);
  CHECK(out.size() == 2);
  CHECK(out[1].timestamp_usec == 2000000);
  CHECK(has_flag(out[1].flags, FrameFlag::kDropped));
  std::printf("PASS test_block_timestamps_and_dropped_flag\n");
}

void test_processor_flags_and_bad_resize()
{
  ChunkReframer r(kBlock, kBpf);
  std::vector<AudioFrame> out;
  int n = 0;
  // Block 0: processor reports bypass. Block 1: processor wrongly shrinks the
  // buffer -> reframer must keep the original samples and flag bypassed.
  auto proc = [&](AudioFrame & b) {
    if (n == 0) {
      b.flags |= static_cast<uint32_t>(FrameFlag::kBypassed);
    } else if (n == 1) {
      b.data = std::make_shared<std::vector<uint8_t>>(10, 0xff);
    } else {
      add_one(b);
    }
    ++n;
  };
  auto sink = [&](AudioFrame && f) { out.push_back(std::move(f)); };
  r.push(make_chunk(160, 0, 0, 0), proc, sink);
  r.push(make_chunk(160, 160, 1, 0), proc, sink);
  r.push(make_chunk(160, 320, 2, 0), proc, sink);
  CHECK(out.size() == 3);
  CHECK(has_flag(out[0].flags, FrameFlag::kBypassed));
  CHECK(has_flag(out[1].flags, FrameFlag::kBypassed));
  CHECK(sample(out[1], 0) == 160);  // original samples, not the bogus 0xffff
  CHECK(out[1].byte_size() == 320);
  CHECK(!has_flag(out[2].flags, FrameFlag::kBypassed));
  CHECK(sample(out[2], 0) == 321);
  std::printf("PASS test_processor_flags_and_bad_resize\n");
}

void test_stereo_and_long_run_bounded()
{
  // 48 kHz stereo S16: 480-frame blocks, 4 bytes/frame, 20 ms chunks that
  // are not multiples of the block size after a 7-frame offset chunk.
  ChunkReframer r(480, 4);
  uint64_t in_frames = 0, out_frames = 0;
  auto noop = [](AudioFrame &) {};
  auto sink = [&](AudioFrame && f) {
    CHECK(f.byte_size() == f.frame_count * 4ull);
    out_frames += f.frame_count;
  };
  AudioFrame first;
  first.frame_count = 7;
  first.data = std::make_shared<std::vector<uint8_t>>(28, 1);
  r.push(first, noop, sink);
  in_frames += 7;
  for (int i = 0; i < 5000; ++i) {
    AudioFrame f;
    f.frame_count = 960;
    f.data = std::make_shared<std::vector<uint8_t>>(960 * 4, 2);
    r.push(f, noop, sink);
    in_frames += 960;
    CHECK(r.pending_chunks() <= 2);
  }
  r.flush(sink);
  CHECK(out_frames == in_frames);
  CHECK(r.pending_chunks() == 0);
  std::printf("PASS test_stereo_and_long_run_bounded\n");
}

void test_voice_decision_reaches_chunks()
{
  // 3 chunks of 250 frames; blocks of 160. Only block 2 ([320, 480)) is
  // voiced, so chunk 1 ([250, 500)) must be active and chunks 0 and 2 not.
  ChunkReframer r(kBlock, kBpf);
  std::vector<AudioFrame> out;
  int n = 0;
  auto vad = [&](AudioFrame & b) {
    b.voice = (n++ == 2) ? VoiceActivity::kActive : VoiceActivity::kInactive;
  };
  auto sink = [&](AudioFrame && f) { out.push_back(std::move(f)); };
  for (uint64_t c = 0; c < 3; ++c)
    r.push(make_chunk(250, c * 250, c, 0), vad, sink);
  r.flush(sink);
  CHECK(out.size() == 3);
  CHECK(out[0].voice == VoiceActivity::kInactive);
  CHECK(out[1].voice == VoiceActivity::kActive);
  // Chunk 2 ([500, 750)): blocks 3,4 inactive; its tail [640, 750) is never
  // processed but the processed part decided -> inactive, not unknown.
  CHECK(out[2].voice == VoiceActivity::kInactive);

  // No VAD stage: decisions stay unknown.
  ChunkReframer r2(kBlock, kBpf);
  std::vector<AudioFrame> out2;
  r2.push(
      make_chunk(320, 0, 0, 0), add_one, [&](AudioFrame && f) { out2.push_back(std::move(f)); });
  CHECK(out2.size() == 1 && out2[0].voice == VoiceActivity::kUnknown);
  std::printf("PASS test_voice_decision_reaches_chunks\n");
}

}  // namespace

int main()
{
  test_aligned_chunks_pass_straight_through();
  test_irregular_chunks_keep_sizes_order_and_content();
  test_block_timestamps_and_dropped_flag();
  test_processor_flags_and_bad_resize();
  test_stereo_and_long_run_bounded();
  test_voice_decision_reaches_chunks();
  std::printf("all core_chunk_reframer_test cases passed\n");
  return 0;
}
