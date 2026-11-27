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
    size_t queue_capacity,
    std::unique_ptr<IAudioCaptureBackend> reference)
  : inner_(std::move(inner))
  , pipeline_(std::move(pipeline))
  , queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity)
  , reference_(std::move(reference))
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

  const bool wants_reference = pipeline_ && pipeline_->needs_reference();
  AudioStreamConfig inner_config = config;
  if (wants_reference)
    inner_config.need_timestamp = true;  // pairing needs capture times
  Result r = inner_->configure(inner_config);
  if (!r.ok())
    return r;

  config_ = config;
  strip_timestamps_ = wants_reference && !config.need_timestamp;
  context_ = ProcessingContext{};
  context_.sample_rate = config.sample_rate;
  context_.channels = config.channels;
  context_.frame_duration_ms = config.frame_duration_ms;
  context_.sample_format = config.sample_format;
  reference_buffer_.reset();
  if (wants_reference) {
    ReferenceBuffer::Options opt;
    opt.lead_ms = config.reference_lead_ms;
    opt.resync_threshold_ms = config.reference_resync_ms;
    reference_buffer_ = std::make_unique<ReferenceBuffer>(config.sample_rate, opt);
    context_.reference = reference_buffer_.get();
    if (!reference_) {
      LOGE("echo reference: no reference backend for this stream; AEC will pass audio through");
    } else {
      AudioStreamConfig ref_config;
      ref_config.device = config.reference_device;
      ref_config.sample_rate = config.sample_rate;
      ref_config.channels = 1;
      ref_config.sample_format = 16;
      ref_config.frame_duration_ms = 10;
      ref_config.fragment_duration_ms = 10;
      ref_config.need_timestamp = true;
      Result rr = reference_->configure(ref_config);
      if (!rr.ok()) {
        LOGE("echo reference '%s' unavailable: %s; the stream runs, AEC passes audio through",
            config.reference_device.c_str(), rr.to_log_string().c_str());
        reference_.reset();
      } else {
        LOGI("echo reference: '%s', mono %u Hz, lead %u ms", config.reference_device.c_str(),
            config.sample_rate, config.reference_lead_ms);
      }
    }
  }
  if (pipeline_) {
    // Format the processor can't handle: keep streaming unprocessed rather
    // than failing create - the chain is node-wide, and a client asking for
    // e.g. 24-bit capture has no per-request way to opt out (until stage 3).
    for (const auto & name : pipeline_->prepare(context_)) {
      for (const auto & st : pipeline_->stats()) {
        if (st.name == name) {
          LOGE("processor %s BYPASSED for this stream (%u Hz, %u ch, %u-bit): %s", name.c_str(),
              config.sample_rate, static_cast<unsigned>(config.channels),
              static_cast<unsigned>(config.sample_format), st.init_error.c_str());
        }
      }
    }
    const uint32_t lat = pipeline_->latency_frames();
    latency_usec_ =
        config.sample_rate ? static_cast<uint64_t>(lat) * 1000000 / config.sample_rate : 0;
    if (lat)
      LOGI("capture processing latency %u frames (%llu us), subtracted from timestamps", lat,
          static_cast<unsigned long long>(latency_usec_));
  }
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

  // Reference first, so samples for the first mic blocks already exist.
  if (reference_buffer_)
    reference_buffer_->reset();
  reference_running_ = false;
  if (reference_ && reference_buffer_) {
    Result rr = reference_->start([this](const AudioFrame & f) { on_reference_frame(f); });
    if (rr.ok()) {
      reference_running_ = true;
    } else {
      LOGE("echo reference start failed: %s; AEC passes audio through", rr.to_log_string().c_str());
    }
  }

  Result r = inner_->start([this](const AudioFrame & frame) { enqueue(frame); });
  if (!r.ok()) {
    shutdown_worker();
    callback_ = nullptr;
    if (reference_running_) {
      reference_->stop();
      reference_running_ = false;
    }
  }
  return r;
}

void ProcessingCaptureBackend::on_reference_frame(const AudioFrame & frame)
{
  // Reference backend's callback thread (PulseAudio mainloop): a memcpy into
  // the ring under its mutex, nothing else.
  if (!reference_buffer_ || !frame.data || frame.sample_format != 16 || frame.channels != 1)
    return;
  reference_buffer_->push(reinterpret_cast<const int16_t *>(frame.data->data()),
      frame.byte_size() / 2, frame.timestamp_usec, has_flag(frame.flags, FrameFlag::kDropped));
}

Result ProcessingCaptureBackend::stop()
{
  // Inner first: once it returns, no more enqueue() calls can arrive, so
  // the worker can drain to empty and exit.
  Result r = inner_ ? inner_->stop() : Result::Ok();
  if (!worker_.joinable()) {
    if (reference_running_) {
      reference_->stop();
      reference_running_ = false;
    }
    return r;
  }

  shutdown_worker();
  callback_ = nullptr;
  // After the worker: an AEC stage may be waiting in fetch() until it drains.
  if (reference_running_) {
    reference_->stop();
    reference_running_ = false;
  }

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
    if (!p.diagnostics.empty())
      LOGI("  processor %s: %s", p.name.c_str(), p.diagnostics.c_str());
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
  if (strip_timestamps_) {
    frame.timestamp_usec = 0;  // only forced on internally for reference pairing
  } else if (frame.timestamp_usec != 0 && latency_usec_ != 0) {
    frame.timestamp_usec =
        frame.timestamp_usec > latency_usec_ ? frame.timestamp_usec - latency_usec_ : 1;
  }
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
    pipeline_->snapshot_diagnostics();
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
