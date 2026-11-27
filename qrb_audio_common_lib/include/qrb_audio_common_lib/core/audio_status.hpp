// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STATUS_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STATUS_HPP_

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

enum class AudioBackendStatus
{
  kUnconnected,
  kConnecting,
  kReady,
  kStopped,
  kFailed,
};

// Per-processor health, tracked by ProcessingPipeline so a flaky NS/AEC
// implementation degrades (bypass) instead of silently corrupting output or
// killing the whole stream - see refactoring_plan.md section 6.4.
enum class ProcessorHealth
{
  kHealthy,
  kDegraded,  // recent failures, currently bypassed, still being retried
  kFailed,    // init failed, never ran
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__AUDIO_STATUS_HPP_
