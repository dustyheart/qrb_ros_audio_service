// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// VadHangover: exact hold time after the last raw-voiced block, immediate
// onset, gap bridging, 0 = pass-through, reset. No library, no hardware.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "qrb_audio_common_lib/core/vad_hangover.hpp"

using qrb::audio_common_lib::core::VadHangover;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

namespace
{

std::vector<bool> run(VadHangover & h, const std::vector<bool> & raw)
{
  std::vector<bool> out;
  for (bool r : raw)
    out.push_back(h.update(r, 10));
  return out;
}

void test_zero_hangover_is_passthrough()
{
  VadHangover h(0);
  std::vector<bool> raw{false, true, false, true, true, false, false};
  CHECK(run(h, raw) == raw);
  std::printf("PASS test_zero_hangover_is_passthrough\n");
}

void test_holds_exactly_hangover_after_last_voice()
{
  VadHangover h(300);  // 30 blocks of 10 ms
  std::vector<bool> raw(5, false);
  raw.push_back(true);
  raw.insert(raw.end(), 50, false);
  auto out = run(h, raw);
  for (int i = 0; i < 5; ++i)
    CHECK(!out[i]);  // no false onset
  CHECK(out[5]);     // onset is immediate
  for (int i = 6; i <= 35; ++i)
    CHECK(out[i]);  // 30 blocks = 300 ms hold
  for (size_t i = 36; i < out.size(); ++i)
    CHECK(!out[i]);
  std::printf("PASS test_holds_exactly_hangover_after_last_voice\n");
}

void test_bridges_short_gaps_not_long_ones()
{
  VadHangover h(100);  // 10 blocks
  // voice, 8-block gap (bridged), voice, 15-block gap (not bridged).
  std::vector<bool> raw{true};
  raw.insert(raw.end(), 8, false);
  raw.push_back(true);
  raw.insert(raw.end(), 15, false);
  auto out = run(h, raw);
  for (int i = 0; i <= 9; ++i)
    CHECK(out[i]);  // gap bridged: no 1/0/1 toggle between "words"
  int toggles = 0;
  for (size_t i = 1; i < out.size(); ++i)
    toggles += out[i] != out[i - 1];
  CHECK(toggles == 1);  // single switch-off after the long gap
  CHECK(!out.back());
  std::printf("PASS test_bridges_short_gaps_not_long_ones\n");
}

void test_reset_and_non_multiple_hangover()
{
  VadHangover h(25);  // not a multiple of 10: rounds down to 2 blocks
  auto out = run(h, {true, false, false, false});
  CHECK(out[0] && out[1] && out[2] && !out[3]);
  h.update(true, 10);
  h.reset();
  CHECK(!h.update(false, 10));  // reset drops the pending hold
  std::printf("PASS test_reset_and_non_multiple_hangover\n");
}

}  // namespace

int main()
{
  test_zero_hangover_is_passthrough();
  test_holds_exactly_hangover_after_last_voice();
  test_bridges_short_gaps_not_long_ones();
  test_reset_and_non_multiple_hangover();
  std::printf("all core_vad_hangover_test cases passed\n");
  return 0;
}
