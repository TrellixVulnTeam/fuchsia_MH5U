// Copyright 2019 The Fuchsia Authors. All rights reserved.
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
constexpr int kInputFileFrameCount = 30;

// Not actually checked, since output buffers are secure.  See use_h264_decoder_test for a test that
// does check this hash.
const char* kGoldenSha256 = "a4418265eaa493604731d6871523ac2a0d606f40cddd48e2a8cd0b0aa5f152e1";

}  // namespace

int main(int argc, char* argv[]) {
  UseVideoDecoderTestParams test_params = {
      .golden_sha256 = kGoldenSha256,
  };
  return use_video_decoder_test(kInputFilePath, kInputFileFrameCount, use_h264_multi_decoder,
                                /*is_secure_output=*/true, /*is_secure_input=*/false,
                                /*min_output_buffer_count=*/0, &test_params);
}
