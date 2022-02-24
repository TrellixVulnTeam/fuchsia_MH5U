// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/utils/fit/promise.h"

#include <lib/async/cpp/executor.h>
#include <lib/fpromise/promise.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

namespace forensics {
namespace fit {
namespace {

class PromiseTest : public gtest::TestLoopFixture {
 public:
  PromiseTest() : executor_(dispatcher()) {}

 protected:
  async::Executor executor_;
};

class StringCombiner {
 public:
  StringCombiner(const std::vector<std::string>& strings) : strings_(strings) {}
  ::fpromise::promise<> Combine(std::function<void(std::string*)> callback) {
    return ::fpromise::make_promise([this, cb = callback]() -> ::fpromise::result<> {
      std::string str;
      for (const auto& string : strings_) {
        str += string;
      }
      cb(&str);
      return ::fpromise::ok();
    });
  }

 private:
  std::vector<std::string> strings_;
};

// This example will not compile.
//
// TEST_F(PromiseTest, Wont_Compile) {
//  ::fpromise::promise<> promise;
//  std::string result;
//  auto combiner = std::make_unique<StringCombiner>(std::vector<std::string>({
//      "s1, ",
//      "s2, ",
//      "s3",
//  }));
//  auto combine = combiner->Combine([&result](std::string* str) { result = *str; });
//  std::string bad_type = "bad_type";
//  promise = ExtendArgsLifetimeBeyondPromise(std::move(combine), std::move(combiner),
//  std::move(bad_type));
//}

// This test will trigger ASAN every so often so it needs to be left commented out.
//
// TEST_F(PromiseTest, Check_WillDieIfNotKeptAlive) {
//  ASSERT_DEATH(
//      {
//        ::fpromise::promise<> promise;
//        std::string result;
//        {
//          auto combiner = std::make_unique<StringCombiner>(std::vector<std::string>({
//              "s1, ",
//              "s2, ",
//              "s3",
//          }));
//          promise = combiner->Combine([&result](std::string* str) { result = *str; });
//        }
//
//        executor_.schedule_task(std::move(promise));
//        RunLoopUntilIdle();
//        EXPECT_EQ(result, "s1, s2, s3");
//      },
//      testing::HasSubstr(""));
//}

TEST_F(PromiseTest, Check_UniquePtrStaysAlive) {
  ::fpromise::promise<> promise;
  std::string result;
  {
    auto combiner = std::make_unique<StringCombiner>(std::vector<std::string>({
        "s1, ",
        "s2, ",
        "s3",
    }));
    auto combine = combiner->Combine([&result](std::string* str) { result = *str; });

    // We cannot directly use obj->Combine() as a parameter in ExtendArgsLifetimeBeyondPromise due
    // to the fact that the order of evaluation of function arguments in undefined.
    promise = ExtendArgsLifetimeBeyondPromise(/*promise=*/std::move(combine),
                                              /*args=*/std::move(combiner));
  }
  executor_.schedule_task(std::move(promise));
  RunLoopUntilIdle();
  EXPECT_EQ(result, "s1, s2, s3");
}

TEST_F(PromiseTest, Check_SharedPtrStaysAlive) {
  ::fpromise::promise<> promise;
  std::string result;
  {
    auto combiner = std::make_shared<StringCombiner>(std::vector<std::string>({
        "s1, ",
        "s2, ",
        "s3",
    }));
    auto combine = combiner->Combine([&result](std::string* str) { result = *str; });

    // We cannot directly use obj->Combine() as a parameter in ExtendArgsLifetimeBeyondPromise due
    // to the fact that the order of evaluation of function arguments in undefined.
    promise = ExtendArgsLifetimeBeyondPromise(/*promise=*/std::move(combine),
                                              /*args=*/std::move(combiner));
  }
  executor_.schedule_task(std::move(promise));
  RunLoopUntilIdle();
  EXPECT_EQ(result, "s1, s2, s3");
}

TEST_F(PromiseTest, Check_TwoPtrsStaysAlive) {
  ::fpromise::promise<> promise;
  std::string result;
  {
    auto combiner = std::make_unique<StringCombiner>(std::vector<std::string>({
        "s1, ",
        "s2, ",
        "s3",
    }));

    auto suffix = std::make_unique<std::string>(" suffix");

    // We must capture the string managed by suffix, otherwise if we moved suffix into the lambda
    // the purpose of attempting to keep it alive through other means would be defeated
    auto combine = combiner->Combine(
        [&result, suffix = suffix.get()](std::string* str) { result = (*str) + (*suffix); });

    // We cannot directly use obj->Combine() as a parameter in ExtendArgsLifetimeBeyondPromise due
    // to the fact that the order of evaluation of function arguments in undefined.
    promise = ExtendArgsLifetimeBeyondPromise(/*promise=*/std::move(combine),
                                              /*args=*/std::move(combiner), std::move(suffix));
  }
  executor_.schedule_task(std::move(promise));
  RunLoopUntilIdle();
  EXPECT_EQ(result, "s1, s2, s3 suffix");
}

}  // namespace
}  // namespace fit
}  // namespace forensics
