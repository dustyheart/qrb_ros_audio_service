// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_PIPELINE_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_PIPELINE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qrb_audio_common_lib/core/audio_processor.hpp"
#include "qrb_audio_common_lib/core/audio_status.hpp"

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

struct ProcessorStats
{
  std::string name;
  ProcessorHealth health = ProcessorHealth::kHealthy;
  uint64_t processed_count = 0;
  uint64_t bypassed_count = 0;
  uint64_t failed_count = 0;
  uint64_t consecutive_failures = 0;
  // Wall time spent inside this stage's process(), microseconds. Measured
  // around every call that actually ran (not for kFailed/kDegraded skips).
  uint64_t total_process_usec = 0;
  uint64_t max_process_usec = 0;
  // configure() failure reason ("[code] message"), empty when healthy.
  std::string init_error;
};

// Runs a fixed, ordered list of IAudioProcessor stages over each AudioFrame.
// Not thread-safe by design: it is driven from exactly one thread (for
// capture, ProcessingCaptureBackend's worker thread), so it does not take any
// internal locks.
//
// Failure policy (refactoring_plan.md section 6.4):
//  - A processor whose configure() fails is marked kFailed and skipped for
//    the lifetime of this pipeline instance (it never silently "comes back").
//  - A processor whose process() fails is counted; once
//    consecutive_failures reaches kMaxConsecutiveFailures it is marked
//    kDegraded and bypassed from then on, so one flaky stage can't take the
//    whole stream down or corrupt every subsequent frame.
//  - A degraded/bypassed stage still receives frames (so stats stay fresh and
//    a future reset() can revive it) but never mutates them; the frame is
//    flagged kBypassed.
class ProcessingPipeline
{
public:
  static constexpr uint64_t kMaxConsecutiveFailures = 10;

  // Takes ownership. `name()` must be unique within one pipeline instance;
  // duplicates are rejected by add_processor() returning false.
  bool add_processor(std::unique_ptr<IAudioProcessor> processor, const ProcessorConfig & config);

  // Runs every non-kFailed stage over `frame` in insertion order. Never
  // fails as a whole: a failing stage is counted and, past the threshold,
  // degraded (see class doc), so a processor failure can never block PCM
  // output (refactoring_plan.md section 6.4).
  void process(AudioFrame & frame, const ProcessingContext & context);

  void reset_all();

  size_t processor_count() const { return processors_.size(); }
  const std::vector<ProcessorStats> & stats() const { return stats_; }

private:
  struct Entry
  {
    std::unique_ptr<IAudioProcessor> processor;
  };

  std::vector<Entry> processors_;
  std::vector<ProcessorStats> stats_;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__PROCESSING_PIPELINE_HPP_
