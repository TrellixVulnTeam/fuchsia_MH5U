// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <tuple>
#include <vector>

#include "src/storage/fs_test/truncate_fixture.h"

namespace fs_test {
namespace {

std::vector<LargeTruncateTestParamType> GetTestCombinations(
    const std::vector<std::tuple</*buf_size=*/size_t, /*iterations=*/size_t,
                                 LargeTruncateTestType>>& variations) {
  std::vector<LargeTruncateTestParamType> test_combinations;
  for (TestFilesystemOptions options : AllTestFilesystems()) {
    for (const auto& variation : variations) {
      if (std::get<2>(variation) == LargeTruncateTestType::Remount &&
          options.filesystem->GetTraits().in_memory) {
        continue;
      }
      if (options.filesystem->GetTraits().is_slow && std::get<0>(variation) > (1 << 20)) {
        // Skip larger buffer sizes on slow filesystems.
        continue;
      }
      constexpr int64_t kBlockCount = 3 * (1LL << 16);
      constexpr int64_t kBlockSize = 1LL << 9;
      if (!options.has_min_volume_size ||
          options.device_block_count * options.device_block_size < kBlockCount * kBlockSize) {
        options.device_block_count = kBlockCount;
        options.device_block_size = kBlockSize;
      }
      options.fvm_slice_size = 1LLU << 23;
      test_combinations.push_back(std::make_tuple(options, variation));
    }
  }
  return test_combinations;
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, LargeTruncateTest,
                         testing::ValuesIn(GetTestCombinations(
                             {std::make_tuple(1 << 20, 50, LargeTruncateTestType::KeepOpen),
                              std::make_tuple(1 << 20, 50, LargeTruncateTestType::Reopen),
                              std::make_tuple(1 << 20, 50, LargeTruncateTestType::Remount),
                              std::make_tuple(1 << 25, 50, LargeTruncateTestType::KeepOpen),
                              std::make_tuple(1 << 25, 50, LargeTruncateTestType::Reopen),
                              std::make_tuple(1 << 25, 50, LargeTruncateTestType::Remount)})),
                         GetDescriptionForLargeTruncateTestParamType);

}  // namespace
}  // namespace fs_test
