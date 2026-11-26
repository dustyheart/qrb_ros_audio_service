// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qrb_audio_common_lib/core/processing_capture_backend.hpp"

#include <string>
#include <utility>

#include "qrb_audio_common_lib/audio_log.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

ProcessingCaptureBackend::ProcessingCaptureBackend(std::unique_ptr<IAudioCaptureBackend> inner,
    std::unique_ptr<ProcessingPipeline> pipeline,
    size_t queue_capacity)
  : inner_(std::move(inner))
  , pipeline_(std::move(pipeline))
  , queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity)
{
  if (pipeline_ && pipeline_->processor_count() == 0)
    pipeline_.reset();
}

ProcessingCaptureBackend::~ProcessingCaptureBackend()
{
  stop();
}

Result ProcessingCaptureBackend::configure(const AudioStreamConfig & config)
{
  if (!inner_)
    return Result::Error(ErrorCode::kInvalidConfig, "no inner capture backend");
  if (worker_.joinable())
    return Result::Error(ErrorCode::kStreamStateConflict, "configure() while started");

  if (pipeline_) {
    const uint64_t scaled = static_cast<uint64_t>(config.sample_rate) * config.frame_duration_ms;
    if (config.frame_duration_ms == 0 || scaled % 1000 != 0) {
      return Result::Error(ErrorCode::kInvalidConfig,
          "frame_duration_ms " + std::to_string(config.frame_duration_ms) +
              " is not a whole number of samples at " + std::to_string(config.sample_rate) + " Hz");
    }
    const uint32_t bytes_per_frame = static_cast<uint32_t>(config.channels) *
                                     ((static_cast<uint32_t>(config.sample_format) + 7) / 8);
    if (bytes_per_frame == 0)
      return Result::Error(ErrorCode::kInvalidConfig, "channels/sample_format must be non-zero");
    reframer_ =
        std::make_unique<ChunkReframer>(static_cast<uint32_t>(scaled / 1000), bytes_per_frame);
  }

  Result r = inner_->configure(config);
  if (!r.ok())
    return r;

  config_ = config;
  context_.sample_rate = config.sample_rate;
  context_.channels = config.channels;
  context_.frame_duration_ms = config.frame_duration_ms;
  configured_ = true;
  return Result::Ok();
}

Result ProcessingCaptureBackend::start(FrameCallback callback)
{
  if (!configured_)
    return Result::Error(ErrorCode::kStreamStateConflict, "configure() was not called");
  if (worker_.joinable())
    return Result::Error(ErrorCode::kStreamStateConflict, "already started");

  callback_ = std::move(callback);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    stopping_ = false;
    mark_next_dropped_ = false;
    stats_ = CaptureProcessingStats{};
  }
  worker_ = std::thread(&ProcessingCaptureBackend::worker_loop, this);

  Result r = inner_->start([this](const AudioFrame & frame) { enqueue(frame); });
  if (!r.ok()) {
    shutdown_worker();
    callback_ = nullptr;
  }
  return r;
}

Result ProcessingCaptureBackend::stop()
{
  // Inner first: once it returns, no more enqueue() calls can arrive, so
  // the worker can drain to empty and exit.
  Result r = inner_ ? inner_->stop() : Result::Ok();
  if (!worker_.joinable())
    return r;

  shutdown_worker();
  callback_ = nullptr;

  CaptureProcessingStats s = stats();
  LOGI(
      "capture processing: %llu chunk(s) in, %llu out, %llu dropped (queue full, capacity %zu, "
      "high water %zu), %llu block(s) processed",
      static_cast<unsigned long long>(s.chunks_in), static_cast<unsigned long long>(s.chunks_out),
      static_cast<unsigned long long>(s.chunks_dropped), queue_capacity_, s.queue_high_water,
      static_cast<unsigned long long>(s.blocks_processed));
  for (const auto & p : s.processors) {
    const uint64_t runs = p.processed_count + p.bypassed_count + p.failed_count;
    LOGI(
        "  processor %s: health %d, processed %llu, bypassed %llu, failed %llu, "
        "avg %llu us, max %llu us%s%s",
        p.name.c_str(), static_cast<int>(p.health),
        static_cast<unsigned long long>(p.processed_count),
        static_cast<unsigned long long>(p.bypassed_count),
        static_cast<unsigned long long>(p.failed_count),
        static_cast<unsigned long long>(runs ? p.total_process_usec / runs : 0),
        static_cast<unsigned long long>(p.max_process_usec), p.init_error.empty() ? "" : ", ",
        p.init_error.c_str());
  }
  return r;
}

AudioBackendStatus ProcessingCaptureBackend::status() const
{
  return inner_ ? inner_->status() : AudioBackendStatus::kUnconnected;
}

CaptureProcessingStats ProcessingCaptureBackend::stats() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void ProcessingCaptureBackend::enqueue(const AudioFrame & frame)
{
  // Inner backend's callback thread: keep this short and non-blocking
  // beyond the (uncontended in steady state) queue mutex.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_)
      return;
    if (queue_.size() >= queue_capacity_) {
      ++stats_.chunks_dropped;
      mark_next_dropped_ = true;
      return;
    }
    queue_.push_back(frame);  // copies metadata + shared_ptr, not the PCM
    if (mark_next_dropped_) {
      queue_.back().flags |= static_cast<uint32_t>(FrameFlag::kDropped);
      mark_next_dropped_ = false;
    }
    ++stats_.chunks_in;
    if (queue_.size() > stats_.queue_high_water)
      stats_.queue_high_water = queue_.size();
  }
  cv_.notify_one();
}

void ProcessingCaptureBackend::deliver(AudioFrame && frame)
{
  if (callback_)
    callback_(frame);
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.chunks_out;
}

void ProcessingCaptureBackend::worker_loop()
{
  const auto on_block = [this](AudioFrame & block) { pipeline_->process(block, context_); };
  const auto on_chunk = [this](AudioFrame && chunk) { deliver(std::move(chunk)); };

  for (;;) {
    AudioFrame frame;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty())
        break;  // stopping_ and fully drained
      frame = std::move(queue_.front());
      queue_.pop_front();
    }

    if (reframer_) {
      reframer_->push(frame, on_block, on_chunk);
    } else {
      deliver(std::move(frame));
    }
  }

  if (reframer_) {
    reframer_->flush(on_chunk);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.blocks_processed = reframer_->blocks_processed();
    stats_.processors = pipeline_->stats();
  }
}

void ProcessingCaptureBackend::shutdown_worker()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_one();
  if (worker_.joinable())
    worker_.join();
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
