// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef QRB_AUDIO_COMMON_LIB__CORE__VAD_HANGOVER_HPP_
#define QRB_AUDIO_COMMON_LIB__CORE__VAD_HANGOVER_HPP_

#include <cstdint>

namespace qrb
{
namespace audio_common_lib
{
namespace core
{

// Hangover smoothing for a raw per-block VAD decision: once voice is seen,
// stay "active" for hangover_ms after the LAST raw-active block, so the short
// gaps between words/syllables don't toggle the output 1/0/1. Onset is not
// delayed (a single raw-active block switches on immediately). hangover_ms 0
// passes the raw decision through. Pure, no clock reads.
class VadHangover
{
public:
  explicit VadHangover(uint32_t hangover_ms = 0) : hangover_ms_(hangover_ms) {}

  void set_hangover_ms(uint32_t ms) { hangover_ms_ = ms; }
  uint32_t hangover_ms() const { return hangover_ms_; }

  // `block_ms`: duration of the block the raw decision covers.
  bool update(bool raw_active, uint32_t block_ms)
  {
    if (raw_active) {
      remaining_ms_ = hangover_ms_;
      return true;
    }
    if (remaining_ms_ >= block_ms && remaining_ms_ > 0) {
      remaining_ms_ -= block_ms;
      return true;
    }
    remaining_ms_ = 0;
    return false;
  }

  void reset() { remaining_ms_ = 0; }

private:
  uint32_t hangover_ms_;
  uint32_t remaining_ms_ = 0;
};

}  // namespace core
}  // namespace audio_common_lib
}  // namespace qrb

#endif  // QRB_AUDIO_COMMON_LIB__CORE__VAD_HANGOVER_HPP_
