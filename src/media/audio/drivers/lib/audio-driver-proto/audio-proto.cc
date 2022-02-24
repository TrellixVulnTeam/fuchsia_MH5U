// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <audio-proto/audio-proto.h>

namespace audio {
namespace audio_proto {

#define WITH_FLAGS(_str)                                                                          \
  ((sample_format & AUDIO_SAMPLE_FORMAT_FLAG_UNSIGNED)                                            \
       ? ((sample_format & AUDIO_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN) ? _str "_UNSIGNED [InvEndian]" \
                                                                   : _str "_UNSIGNED")            \
       : ((sample_format & AUDIO_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN) ? _str " [InvEndian]" : _str))

const char* SampleFormatToString(SampleFormat sample_format) {
  auto fmt = static_cast<SampleFormat>(sample_format & ~AUDIO_SAMPLE_FORMAT_FLAG_MASK);
  switch (fmt) {
    case AUDIO_SAMPLE_FORMAT_BITSTREAM:
      return WITH_FLAGS("BITSTREAM");
    case AUDIO_SAMPLE_FORMAT_8BIT:
      return WITH_FLAGS("8BIT");
    case AUDIO_SAMPLE_FORMAT_16BIT:
      return WITH_FLAGS("16BIT");
    case AUDIO_SAMPLE_FORMAT_20BIT_PACKED:
      return WITH_FLAGS("20BIT_PACKED");
    case AUDIO_SAMPLE_FORMAT_24BIT_PACKED:
      return WITH_FLAGS("24BIT_PACKED");
    case AUDIO_SAMPLE_FORMAT_20BIT_IN32:
      return WITH_FLAGS("20BIT_IN32");
    case AUDIO_SAMPLE_FORMAT_24BIT_IN32:
      return WITH_FLAGS("24BIT_IN32");
    case AUDIO_SAMPLE_FORMAT_32BIT_FLOAT:
      return WITH_FLAGS("32BIT_FLOAT");
    default:
      return WITH_FLAGS("<unknown>");
  }
}
#undef WITH_FLAGS

bool operator==(const GainState& left, const GainState& right) {
  return (left.cur_gain == right.cur_gain && left.cur_mute == right.cur_mute &&
          left.cur_agc == right.cur_agc && left.can_mute == right.can_mute &&
          left.can_agc == right.can_agc && left.min_gain == right.min_gain &&
          left.max_gain == right.max_gain && left.gain_step == right.gain_step);
}

bool operator!=(const GainState& left, const GainState& right) { return !(left == right); }

}  // namespace audio_proto
}  // namespace audio
