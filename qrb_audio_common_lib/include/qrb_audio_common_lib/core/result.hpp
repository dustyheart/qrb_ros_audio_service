// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__RESULT_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__RESULT_HPP_

#include <string>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Unified error categories shared across backend, pipeline, and (eventually)
// ROS service responses, per refactoring_plan.md section 4.3. Keep this list
// flat and small; add a category only when a caller needs to branch on it
// differently, not for every distinct failure string.
enum class ErrorCode
{
  kOk = 0,
  kInvalidConfig,
  kDeviceUnavailable,
  kUnsupportedFormat,
  kBackendInitFailed,
  kStreamStateConflict,
  kProcessorInitFailed,
  kProcessorProcessFailed,
  kResourceExhausted,
  kInternalError,
};

inline const char * to_string(ErrorCode code)
{
  switch (code) {
    case ErrorCode::kOk:
      return "ok";
    case ErrorCode::kInvalidConfig:
      return "invalid_config";
    case ErrorCode::kDeviceUnavailable:
      return "device_unavailable";
    case ErrorCode::kUnsupportedFormat:
      return "unsupported_format";
    case ErrorCode::kBackendInitFailed:
      return "backend_init_failed";
    case ErrorCode::kStreamStateConflict:
      return "stream_state_conflict";
    case ErrorCode::kProcessorInitFailed:
      return "processor_init_failed";
    case ErrorCode::kProcessorProcessFailed:
      return "processor_process_failed";
    case ErrorCode::kResourceExhausted:
      return "resource_exhausted";
    case ErrorCode::kInternalError:
    default:
      return "internal_error";
  }
}

// Carries enough information for a log line, a diagnostics message, and a ROS
// service response, without each call site inventing its own string format.
struct Result
{
  ErrorCode code = ErrorCode::kOk;
  std::string message;

  bool ok() const { return code == ErrorCode::kOk; }

  static Result Ok() { return Result{ ErrorCode::kOk, "" }; }
  static Result Error(ErrorCode c, std::string msg) { return Result{ c, std::move(msg) }; }
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__RESULT_HPP_
