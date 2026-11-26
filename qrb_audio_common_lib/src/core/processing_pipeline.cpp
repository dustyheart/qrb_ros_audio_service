// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qrb_audio_common_lib/core/processing_pipeline.hpp"

#include <chrono>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

bool ProcessingPipeline::add_processor(std::unique_ptr<IAudioProcessor> processor,
    const ProcessorConfig & config)
{
  if (!processor)
    return false;

  const std::string name = processor->name();
  for (const auto & stats : stats_) {
    if (stats.name == name)
      return false;  // duplicate name within this pipeline instance
  }

  ProcessorStats stats;
  stats.name = name;

  Result configure_result = processor->configure(config);
  if (!configure_result.ok()) {
    stats.health = ProcessorHealth::kFailed;
    stats.init_error = configure_result.to_log_string();
  }

  processors_.push_back(Entry{std::move(processor)});
  stats_.push_back(std::move(stats));
  return true;
}

void ProcessingPipeline::process(AudioFrame & frame, const ProcessingContext & context)
{
  for (size_t i = 0; i < processors_.size(); ++i) {
    ProcessorStats & stats = stats_[i];

    if (stats.health == ProcessorHealth::kFailed) {
      // Never runs (configure() or prepare() failed). The frame did not get
      // the processing the chain asked for, so say so.
      frame.flags = static_cast<uint32_t>(FrameFlag::kBypassed) | frame.flags;
      continue;
    }

    if (stats.health == ProcessorHealth::kDegraded) {
      frame.flags = static_cast<uint32_t>(FrameFlag::kBypassed) | frame.flags;
      stats.bypassed_count++;
      continue;
    }

    const auto t0 = std::chrono::steady_clock::now();
    ProcessResult result = processors_[i].processor->process(frame, context);
    const uint64_t usec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    stats.total_process_usec += usec;
    if (usec > stats.max_process_usec)
      stats.max_process_usec = usec;
    switch (result.status) {
      case ProcessResultStatus::kProcessed:
        stats.processed_count++;
        stats.consecutive_failures = 0;
        break;
      case ProcessResultStatus::kBypassed:
        frame.flags = static_cast<uint32_t>(FrameFlag::kBypassed) | frame.flags;
        stats.bypassed_count++;
        stats.consecutive_failures = 0;
        break;
      case ProcessResultStatus::kFailed:
        stats.failed_count++;
        stats.consecutive_failures++;
        if (stats.consecutive_failures >= ProcessingPipeline::kMaxConsecutiveFailures) {
          stats.health = ProcessorHealth::kDegraded;
        }
        break;
    }
  }
}

std::vector<std::string> ProcessingPipeline::prepare(const ProcessingContext & context)
{
  std::vector<std::string> bypassed;
  for (size_t i = 0; i < processors_.size(); ++i) {
    if (stats_[i].health == ProcessorHealth::kFailed)
      continue;
    Result r = processors_[i].processor->prepare(context);
    if (!r.ok()) {
      stats_[i].health = ProcessorHealth::kFailed;
      stats_[i].init_error = r.to_log_string();
      bypassed.push_back(stats_[i].name);
    }
  }
  return bypassed;
}

void ProcessingPipeline::reset_all()
{
  for (size_t i = 0; i < processors_.size(); ++i) {
    if (stats_[i].health == ProcessorHealth::kFailed)
      continue;  // configure() never succeeded; nothing to reset
    processors_[i].processor->reset();
    stats_[i].health = ProcessorHealth::kHealthy;
    stats_[i].consecutive_failures = 0;
  }
}

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb
