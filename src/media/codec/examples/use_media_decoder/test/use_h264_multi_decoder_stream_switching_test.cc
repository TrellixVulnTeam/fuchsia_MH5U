// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/media/codec_impl/fourcc.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <set>

#include "../use_video_decoder.h"
#include "../util.h"
#include "use_video_decoder_test.h"

namespace {

constexpr char kInputFilePath[] = "/pkg/data/bear.h264";
constexpr int kInputFileFrameCount = 300;

const char* kGoldenSha256 = "1cc5002b6d68f34f601813c5bbed775eb1a1033a8109bdf2fed59160f2edc207";

}  // namespace

int main(int argc, char* argv[]) {
  const UseVideoDecoderTestParams test_params{
      .keep_stream_modulo = 4,
      // Only ~half of these get hashed.
      .loop_stream_count = 20,
      .golden_sha256 = kGoldenSha256,
  };
  return use_video_decoder_test(kInputFilePath, kInputFileFrameCount, use_h264_multi_decoder,
                                /*is_secure_output=*/false, /*is_secure_input=*/false,
                                /*min_output_buffer_count=*/0, &test_params);
}
