// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_
#define SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_

#include <lib/fidl/llcpp/arena.h>
#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/message.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <algorithm>
#include <iostream>
#include <type_traits>

#include <perftest/perftest.h>

namespace llcpp_benchmarks {

template <typename BuilderFunc>
bool DecodeBenchmark(perftest::RepeatState* state, BuilderFunc builder) {
  using FidlType = std::invoke_result_t<BuilderFunc, fidl::AnyArena&>;
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");

  state->DeclareStep("Setup/WallTime");
  state->DeclareStep("Decode/WallTime");
  state->DeclareStep("Teardown/WallTime");

  while (state->KeepRunning()) {
    // construct a new object each iteration, so that the handle close cost is included in the
    // decode time.
    fidl::Arena<65536> allocator;
    FidlType aligned_value = builder(allocator);
    // encode the value.
    fidl::unstable::OwnedEncodedMessage<FidlType> encoded(fidl::internal::WireFormatVersion::kV2,
                                                          &aligned_value);
    if (!encoded.ok()) {
      std::cerr << "Unexpected error: " << encoded.error() << std::endl;
    }
    ZX_ASSERT(encoded.ok());

    // Convert the outgoing message to incoming which is suitable for decoding.
    // This may involve expensive allocations and copies.
    // This step does not happen in production (we would receive an incoming message
    // from the channel), hence not counted as part of decode time.
    auto converted = fidl::OutgoingToIncomingMessage(encoded.GetOutgoingMessage());
    ZX_ASSERT(converted.ok());

    state->NextStep();  // End: Setup. Begin: Decode.

    {
      auto decoded = fidl::unstable::DecodedMessage<FidlType>(
          fidl::internal::WireFormatVersion::kV2, std::move(converted.incoming_message()));
      ZX_ASSERT_MSG(decoded.ok(), "%s", decoded.FormatDescription().c_str());
      // Include time taken to close handles in |FidlType|.
    }

    state->NextStep();  // End: Decode. Begin: Teardown.
  }
  return true;
}

}  // namespace llcpp_benchmarks

#endif  // SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_
