// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// validate_capture_config(): pure format/buffer validation, no audio server
// needed. Uses explicit checks instead of assert() so it still tests
// something when built with NDEBUG.

#include <cstdio>

#include "qrb_audio_common_lib/core/config_validation.hpp"

using namespace qrb::audio_common_lib::core;

static int g_failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

static AudioStreamConfig valid_config()
{
  AudioStreamConfig c;
  c.sample_rate = 16000;
  c.channels = 1;
  c.sample_format = 16;
  c.frame_duration_ms = 20;
  c.fragment_duration_ms = 0;
  return c;
}

int main()
{
  CHECK(validate_capture_config(valid_config()).ok());

  for (uint8_t bits : {8, 16, 24, 32}) {
    auto c = valid_config();
    c.sample_format = bits;
    CHECK(validate_capture_config(c).ok());
  }

  // The exact case the legacy path used to silently record as S16.
  for (uint8_t bits : {0, 12, 20, 64}) {
    auto c = valid_config();
    c.sample_format = bits;
    Result r = validate_capture_config(c);
    CHECK(r.code == ErrorCode::kUnsupportedFormat);
    CHECK(r.to_log_string().rfind("[unsupported_format] ", 0) == 0);
  }

  {
    auto c = valid_config();
    c.channels = 0;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
    c.channels = kMaxChannels;
    CHECK(validate_capture_config(c).ok());
    c.channels = kMaxChannels + 1;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
  }
  {
    auto c = valid_config();
    c.sample_rate = 0;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
    c.sample_rate = kMaxSampleRate;
    CHECK(validate_capture_config(c).ok());
    c.sample_rate = kMaxSampleRate + 1;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
  }
  {
    auto c = valid_config();
    c.fragment_duration_ms = kMaxDurationMs;
    CHECK(validate_capture_config(c).ok());
    c.fragment_duration_ms = kMaxDurationMs + 1;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
  }
  {
    auto c = valid_config();
    c.frame_duration_ms = 0;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
    c.frame_duration_ms = kMaxDurationMs + 1;
    CHECK(validate_capture_config(c).code == ErrorCode::kInvalidConfig);
  }

  if (g_failures) {
    std::fprintf(stderr, "core_config_validation_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("core_config_validation_test: all checks passed\n");
  return 0;
}
