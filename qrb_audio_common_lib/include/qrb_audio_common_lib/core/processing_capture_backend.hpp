// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_CAPTURE_BACKEND_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_CAPTURE_BACKEND_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "qrb_audio_common_lib/core/audio_capture_backend.hpp"
#include "qrb_audio_common_lib/core/chunk_reframer.hpp"
#include "qrb_audio_common_lib/core/processing_pipeline.hpp"
#include "qrb_audio_common_lib/core/reference_buffer.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

struct CaptureProcessingStats
{
  uint64_t chunks_in = 0;         // accepted from the inner backend
  uint64_t chunks_out = 0;        // delivered to the FrameCallback
  uint64_t chunks_dropped = 0;    // rejected because the queue was full
  size_t queue_high_water = 0;    // max chunks waiting at once
  uint64_t blocks_processed = 0;  // re-framed blocks run through the pipeline
  std::vector<ProcessorStats> processors;
};

// Decorator that moves everything after "a chunk arrived" off the capture
// backend's callback thread (the PulseAudio mainloop for
// PipeWirePulseBackend):
//
//   inner backend thread:  chunk -> bounded queue (no PCM copy, no I/O)
//   worker thread:         queue -> [ChunkReframer -> ProcessingPipeline]
//                                -> FrameCallback (e.g. ROS publish)
//
// So a slow subscriber or processor can only delay or drop output; it can
// never stall the mainloop, which every PulseAudio stream in the process
// shares. Without processors the reframer is skipped and chunks are passed
// on unchanged. With processors, chunks are processed in frame_duration_ms
// blocks and re-assembled to their original sizes/timestamps (see
// ChunkReframer), so subscribers see the same messages either way.
//
// Queue full: the newest chunk is dropped and counted, and the next accepted
// chunk is flagged kDropped (same meaning as a backend-detected gap).
//
// FrameCallback runs on the worker thread. stop() stops the inner backend
// first (no more input), then drains the queue, flushes the reframer and
// joins the worker; after stop() returns the callback is never invoked again.
//
// Echo reference (stage 6): if the pipeline has a stage that
// needs_reference() (AEC) and a `reference` backend was given, configure()
// opens it on config.reference_device (mono, sample_rate, 10 ms fragments,
// timestamps on) and start() starts it BEFORE the mic stream; its chunks go
// into a ReferenceBuffer that the AEC stage reads (ProcessingContext::
// reference). The reference is best effort: if it cannot be opened/started,
// stalls or dies, the mic stream keeps running and the AEC stage passes audio
// through (flagged kBypassed, logged) - it never fails the stream.
//
// Timestamps: when a stage needs the reference, the mic backend is
// configured with need_timestamp=true regardless of the request (pairing
// needs capture times); they are stripped again before delivery if the
// caller did not ask for them. Delivered non-zero timestamps are moved back
// by the pipeline's latency_frames() (APM delay), so they keep meaning
// "capture time of the audio in the first sample".
class ProcessingCaptureBackend : public IAudioCaptureBackend
{
public:
  static constexpr size_t kDefaultQueueCapacity = 64;

  // `pipeline` may be null or empty (pass-through, thread decoupling only).
  // `reference` is only used when the pipeline needs_reference().
  ProcessingCaptureBackend(std::unique_ptr<IAudioCaptureBackend> inner,
      std::unique_ptr<ProcessingPipeline> pipeline,
      size_t queue_capacity = kDefaultQueueCapacity,
      std::unique_ptr<IAudioCaptureBackend> reference = nullptr);
  ~ProcessingCaptureBackend() override;

  Result configure(const AudioStreamConfig & config) override;
  Result start(FrameCallback callback) override;
  Result stop() override;
  AudioBackendStatus status() const override;

  // Safe to call from any thread. Counters are live; `blocks_processed` and
  // `processors` are filled in when the worker exits, i.e. after stop().
  CaptureProcessingStats stats() const;

  // Pipeline delay subtracted from delivered timestamps (usec); valid after
  // configure().
  uint64_t latency_compensation_usec() const { return latency_usec_; }
  // Null unless a stage needs the reference (valid after configure()).
  const ReferenceBuffer * reference_buffer() const { return reference_buffer_.get(); }

private:
  void enqueue(const AudioFrame & frame);
  void worker_loop();
  void deliver(AudioFrame && frame);
  void shutdown_worker();
  void on_reference_frame(const AudioFrame & frame);

  std::unique_ptr<IAudioCaptureBackend> inner_;
  std::unique_ptr<ProcessingPipeline> pipeline_;
  std::unique_ptr<ChunkReframer> reframer_;
  const size_t queue_capacity_;

  std::unique_ptr<IAudioCaptureBackend> reference_;
  std::unique_ptr<ReferenceBuffer> reference_buffer_;
  bool reference_running_ = false;
  bool strip_timestamps_ = false;  // forced on for pairing, not requested
  uint64_t latency_usec_ = 0;

  AudioStreamConfig config_;
  ProcessingContext context_;
  bool configured_ = false;
  FrameCallback callback_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<AudioFrame> queue_;
  bool stopping_ = false;
  bool mark_next_dropped_ = false;
  CaptureProcessingStats stats_;  // guarded by mutex_
  std::thread worker_;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_CAPTURE_BACKEND_HPP_
