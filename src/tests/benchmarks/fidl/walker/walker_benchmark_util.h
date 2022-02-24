// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_TESTS_BENCHMARKS_FIDL_WALKER_WALKER_BENCHMARK_UTIL_H_
#define SRC_TESTS_BENCHMARKS_FIDL_WALKER_WALKER_BENCHMARK_UTIL_H_

#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/message.h>
#include <lib/fidl/visitor.h>
#include <lib/fidl/walker.h>

#include <perftest/perftest.h>

namespace walker_benchmarks {

namespace internal {

void Walk(const fidl_type_t* fidl_type, uint8_t* data);

}  // namespace internal

template <typename FidlType, typename BuilderFunc>
bool WalkerBenchmark(perftest::RepeatState* state, BuilderFunc builder) {
  builder([state](FidlType value) {
    FidlType aligned_value = std::move(value);

    // Linearize the object by encoding and then decoding it.
    // TODO(fxbug.dev/53743) Change the walker to walk encoded bytes.
    fidl::unstable::OwnedEncodedMessage<FidlType> encoded(fidl::internal::WireFormatVersion::kV2,
                                                          &aligned_value);
    ZX_ASSERT(encoded.ok());
    auto converted = fidl::OutgoingToIncomingMessage(encoded.GetOutgoingMessage());
    ZX_ASSERT(converted.ok());
    auto decoded = fidl::unstable::DecodedMessage<FidlType>(
        fidl::internal::WireFormatVersion::kV2, std::move(converted.incoming_message()));
    ZX_ASSERT_MSG(decoded.ok(), "%s", decoded.FormatDescription().c_str());

    while (state->KeepRunning()) {
      internal::Walk(fidl::TypeTraits<FidlType>::kType,
                     reinterpret_cast<uint8_t*>(decoded.PrimaryObject()));
    }
  });

  return true;
}

}  // namespace walker_benchmarks

#endif  // SRC_TESTS_BENCHMARKS_FIDL_WALKER_WALKER_BENCHMARK_UTIL_H_
