// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/fuzzing/common/controller.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <gtest/gtest.h>

#include "src/sys/fuzzing/common/binding.h"
#include "src/sys/fuzzing/common/dispatcher.h"
#include "src/sys/fuzzing/common/status.h"
#include "src/sys/fuzzing/common/testing/corpus-reader.h"
#include "src/sys/fuzzing/common/testing/monitor.h"
#include "src/sys/fuzzing/common/testing/runner.h"
#include "src/sys/fuzzing/common/testing/transceiver.h"

namespace fuzzing {
namespace {

using fuchsia::fuzzer::ControllerSyncPtr;
using fuchsia::fuzzer::UpdateReason;

// Test fixtures.

// Base class for |Controller| unit tests.
class ControllerTest : public ::testing::Test {
 public:
  // Implicitly tests |Controller::SetRunner| and |Controller::Bind|.
  ControllerSyncPtr Bind() {
    auto runner = std::make_unique<FakeRunner>();
    runner_ = runner.get();
    controller_.SetRunner(std::move(runner));

    ControllerSyncPtr controller;
    controller_.Bind(controller.NewRequest());
    return controller;
  }

  void AddToCorpus(CorpusType corpus_type, Input input) {
    runner_->AddToCorpus(corpus_type, std::move(input));
  }
  Input ReadFromCorpus(CorpusType corpus_type, size_t offset) {
    return runner_->ReadFromCorpus(corpus_type, offset);
  }

  zx_status_t ParseDictionary(const Input& input) { return runner_->ParseDictionary(input); }

  void SetError(zx_status_t error) { runner_->set_error(error); }
  void SetResult(FuzzResult result) { runner_->set_result(result); }
  void SetResultInput(const Input& input) { runner_->set_result_input(input); }
  void SetStatus(Status status) { runner_->set_status(std::move(status)); }
  void UpdateMonitors(UpdateReason reason) { runner_->UpdateMonitors(reason); }

  FidlInput Transmit(const Input& input) { return transceiver_.Transmit(input.Duplicate()); }

  // Synchronously receives and returns an |Input| from a provided |FidlInput|.
  Input Receive(FidlInput fidl_input) { return transceiver_.Receive(std::move(fidl_input)); }

 private:
  ControllerImpl controller_;
  FakeRunner* runner_;
  FakeTransceiver transceiver_;
};

// Unit tests.

TEST_F(ControllerTest, ConfigureAndGetOptions) {
  auto controller = Bind();

  // GetOptions without Configure.
  Options options1;
  EXPECT_EQ(controller->GetOptions(&options1), ZX_OK);
  EXPECT_NE(options1.seed(), 0U);

  // Configure.
  uint32_t runs = 1000;
  zx::duration max_total_time = zx::sec(300);
  uint32_t seed = 42;
  uint32_t max_input_size = 1ULL << 10;
  uint16_t mutation_depth = 8;
  bool detect_exits = true;
  bool detect_leaks = false;
  zx::duration run_limit = zx::sec(20);
  options1.set_runs(runs);
  options1.set_max_total_time(max_total_time.get());
  options1.set_seed(seed);
  options1.set_max_input_size(max_input_size);
  options1.set_mutation_depth(mutation_depth);
  options1.set_detect_exits(detect_exits);
  options1.set_detect_leaks(detect_leaks);
  options1.set_run_limit(run_limit.get());
  zx_status_t status;
  auto options2 = CopyOptions(options1);
  EXPECT_EQ(controller->Configure(std::move(options1), &status), ZX_OK);
  EXPECT_EQ(status, ZX_OK);

  // Can Configure again.
  uint64_t malloc_limit = 64ULL << 10;
  uint64_t oom_limit = 1ULL << 20;
  zx::duration purge_interval = zx::sec(10);
  int32_t malloc_exitcode = 1000;
  int32_t death_exitcode = 1001;
  int32_t leak_exitcode = 1002;
  int32_t oom_exitcode = 1003;
  zx::duration pulse_interval = zx::sec(3);
  options2.set_malloc_limit(malloc_limit);
  options2.set_oom_limit(oom_limit);
  options2.set_purge_interval(purge_interval.get());
  options2.set_malloc_exitcode(malloc_exitcode);
  options2.set_death_exitcode(death_exitcode);
  options2.set_leak_exitcode(leak_exitcode);
  options2.set_oom_exitcode(oom_exitcode);
  options2.set_pulse_interval(pulse_interval.get());
  EXPECT_EQ(controller->Configure(std::move(options2), &status), ZX_OK);
  EXPECT_EQ(status, ZX_OK);

  // Changes are reflected.
  Options options3;
  EXPECT_EQ(controller->GetOptions(&options3), ZX_OK);
  EXPECT_EQ(options3.runs(), runs);
  EXPECT_EQ(options3.max_total_time(), max_total_time.get());
  EXPECT_EQ(options3.seed(), seed);
  EXPECT_EQ(options3.max_input_size(), max_input_size);
  EXPECT_EQ(options3.mutation_depth(), mutation_depth);
  EXPECT_EQ(options3.detect_exits(), detect_exits);
  EXPECT_EQ(options3.detect_leaks(), detect_leaks);
  EXPECT_EQ(options3.run_limit(), run_limit.get());
  EXPECT_EQ(options3.malloc_limit(), malloc_limit);
  EXPECT_EQ(options3.oom_limit(), oom_limit);
  EXPECT_EQ(options3.purge_interval(), purge_interval.get());
  EXPECT_EQ(options3.malloc_exitcode(), malloc_exitcode);
  EXPECT_EQ(options3.death_exitcode(), death_exitcode);
  EXPECT_EQ(options3.leak_exitcode(), leak_exitcode);
  EXPECT_EQ(options3.oom_exitcode(), oom_exitcode);
  EXPECT_EQ(options3.pulse_interval(), pulse_interval.get());
}

TEST_F(ControllerTest, AddToCorpus) {
  auto controller = Bind();
  Input input0;
  Input seed_input1({0xde, 0xad});
  Input seed_input2({0xbe, 0xef});
  Input live_input3({0xfe, 0xed});
  Input live_input4({0xfa, 0xce});
  zx_status_t result;

  // Interleave the calls.
  EXPECT_EQ(controller->AddToCorpus(CorpusType::LIVE, Transmit(live_input3), &result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);
  EXPECT_EQ(controller->AddToCorpus(CorpusType::SEED, Transmit(seed_input1), &result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);
  EXPECT_EQ(controller->AddToCorpus(CorpusType::SEED, Transmit(seed_input2), &result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);
  EXPECT_EQ(controller->AddToCorpus(CorpusType::LIVE, Transmit(live_input4), &result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);

  EXPECT_EQ(ReadFromCorpus(CorpusType::SEED, 0).ToHex(), input0.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::SEED, 1).ToHex(), seed_input1.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::SEED, 2).ToHex(), seed_input2.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::SEED, 3).ToHex(), input0.ToHex());

  EXPECT_EQ(ReadFromCorpus(CorpusType::LIVE, 0).ToHex(), input0.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::LIVE, 1).ToHex(), live_input3.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::LIVE, 2).ToHex(), live_input4.ToHex());
  EXPECT_EQ(ReadFromCorpus(CorpusType::LIVE, 3).ToHex(), input0.ToHex());
}

TEST_F(ControllerTest, ReadCorpus) {
  auto controller = Bind();
  Input input0;
  Input input1({0xde, 0xad});
  Input input2({0xbe, 0xef});
  Input input3({0xfe, 0xed});
  Input input4({0xfa, 0xce});

  AddToCorpus(CorpusType::SEED, input1.Duplicate());
  AddToCorpus(CorpusType::SEED, input2.Duplicate());

  AddToCorpus(CorpusType::LIVE, input3.Duplicate());
  AddToCorpus(CorpusType::LIVE, input4.Duplicate());

  FakeCorpusReader seed_reader;
  FakeCorpusReader live_reader;
  EXPECT_EQ(controller->ReadCorpus(CorpusType::SEED, seed_reader.NewBinding()), ZX_OK);
  EXPECT_EQ(controller->ReadCorpus(CorpusType::LIVE, live_reader.NewBinding()), ZX_OK);

  // Interleave the calls.
  ASSERT_TRUE(live_reader.AwaitNext());
  EXPECT_EQ(live_reader.GetNext().ToHex(), input3.ToHex());

  ASSERT_TRUE(seed_reader.AwaitNext());
  EXPECT_EQ(seed_reader.GetNext().ToHex(), input1.ToHex());

  ASSERT_TRUE(live_reader.AwaitNext());
  EXPECT_EQ(live_reader.GetNext().ToHex(), input4.ToHex());

  ASSERT_TRUE(seed_reader.AwaitNext());
  EXPECT_EQ(seed_reader.GetNext().ToHex(), input2.ToHex());

  // All inputs have been sent.
  EXPECT_FALSE(live_reader.AwaitNext());
  EXPECT_FALSE(live_reader.AwaitNext());
}

TEST_F(ControllerTest, WriteDictionary) {
  auto controller = Bind();
  auto invalid = FakeRunner::invalid_dictionary();
  auto valid = FakeRunner::valid_dictionary();
  zx_status_t result;

  EXPECT_EQ(controller->WriteDictionary(Transmit(invalid), &result), ZX_OK);
  EXPECT_EQ(result, ZX_ERR_INVALID_ARGS);

  EXPECT_EQ(controller->WriteDictionary(Transmit(valid), &result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);
}

TEST_F(ControllerTest, ReadDictionary) {
  auto controller = Bind();
  FidlInput result;

  auto dict = FakeRunner::valid_dictionary();
  EXPECT_EQ(ParseDictionary(dict), ZX_OK);
  EXPECT_EQ(controller->ReadDictionary(&result), ZX_OK);
  auto received = Receive(std::move(result));
  EXPECT_EQ(received.ToHex(), dict.ToHex());
}

TEST_F(ControllerTest, GetStatus) {
  auto controller = Bind();
  Status result;

  Status status;
  status.set_running(true);
  status.set_runs(42);
  status.set_elapsed(zx::sec(15).get());
  status.set_covered_pcs(5);
  status.set_covered_features(10);
  status.set_corpus_num_inputs(15);
  status.set_corpus_total_size(25);
  auto expected = CopyStatus(status);
  SetStatus(std::move(status));

  EXPECT_EQ(controller->GetStatus(&result), ZX_OK);
  EXPECT_EQ(result.running(), expected.running());
  EXPECT_EQ(result.runs(), expected.runs());
  EXPECT_EQ(result.elapsed(), expected.elapsed());
  EXPECT_EQ(result.covered_pcs(), expected.covered_pcs());
  EXPECT_EQ(result.covered_features(), expected.covered_features());
  EXPECT_EQ(result.corpus_num_inputs(), expected.corpus_num_inputs());
  EXPECT_EQ(result.corpus_total_size(), expected.corpus_total_size());
}

TEST_F(ControllerTest, AddMonitor) {
  auto controller = Bind();
  FakeMonitor monitor;

  Status status;
  status.set_runs(13);
  auto expected = CopyStatus(status);
  SetStatus(std::move(status));
  EXPECT_EQ(controller->AddMonitor(monitor.NewBinding()), ZX_OK);
  UpdateMonitors(UpdateReason::PULSE);

  UpdateReason reason;
  auto updated = monitor.NextStatus(&reason);
  EXPECT_EQ(updated.runs(), expected.runs());
  EXPECT_EQ(reason, UpdateReason::PULSE);
}

TEST_F(ControllerTest, GetResults) {
  auto controller = Bind();
  FuzzResult result;
  FidlInput fidl_input;
  Input result_input({0xde, 0xad, 0xbe, 0xef});

  SetResult(FuzzResult::DEATH);
  SetResultInput(result_input);
  EXPECT_EQ(controller->GetResults(&result, &fidl_input), ZX_OK);
  EXPECT_EQ(result, FuzzResult::DEATH);
  auto received = Receive(std::move(fidl_input));
  EXPECT_EQ(received.ToHex(), result_input.ToHex());
}

TEST_F(ControllerTest, Execute) {
  auto controller = Bind();
  ::fuchsia::fuzzer::Controller_Execute_Result result;
  Input input({0xde, 0xad, 0xbe, 0xef});

  SetError(ZX_ERR_WRONG_TYPE);
  EXPECT_EQ(controller->Execute(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.err(), ZX_ERR_WRONG_TYPE);

  SetError(ZX_OK);
  SetResult(FuzzResult::OOM);
  EXPECT_EQ(controller->Execute(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_response()) << zx_status_get_string(result.err());
  auto& response = result.response();
  EXPECT_EQ(response.result, FuzzResult::OOM);
}

TEST_F(ControllerTest, Minimize) {
  auto controller = Bind();
  ::fuchsia::fuzzer::Controller_Minimize_Result result;
  Input input({0xde, 0xad, 0xbe, 0xef});
  Input minimized({0xde, 0xbe});

  SetError(ZX_ERR_WRONG_TYPE);
  EXPECT_EQ(controller->Minimize(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.err(), ZX_ERR_WRONG_TYPE);

  SetError(ZX_OK);
  SetResultInput(minimized);
  EXPECT_EQ(controller->Minimize(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_response()) << zx_status_get_string(result.err());
  auto& response = result.response();
  auto received = Receive(std::move(response.minimized));
  EXPECT_EQ(received.ToHex(), minimized.ToHex());
}

TEST_F(ControllerTest, Cleanse) {
  auto controller = Bind();
  ::fuchsia::fuzzer::Controller_Cleanse_Result result;
  Input input({0xde, 0xad, 0xbe, 0xef});
  Input cleansed({0x20, 0x20, 0xbe, 0xff});

  SetError(ZX_ERR_WRONG_TYPE);
  EXPECT_EQ(controller->Cleanse(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.err(), ZX_ERR_WRONG_TYPE);

  SetError(ZX_OK);
  SetResultInput(cleansed);
  EXPECT_EQ(controller->Cleanse(Transmit(input), &result), ZX_OK);
  ASSERT_TRUE(result.is_response()) << zx_status_get_string(result.err());
  auto& response = result.response();
  auto received = Receive(std::move(response.cleansed));
  EXPECT_EQ(received.ToHex(), cleansed.ToHex());
}

TEST_F(ControllerTest, Fuzz) {
  auto controller = Bind();
  ::fuchsia::fuzzer::Controller_Fuzz_Result result;
  Input fuzzed({0xde, 0xad, 0xbe, 0xef});

  SetError(ZX_ERR_WRONG_TYPE);
  EXPECT_EQ(controller->Fuzz(&result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.err(), ZX_ERR_WRONG_TYPE);

  SetError(ZX_OK);
  SetResult(FuzzResult::CRASH);
  SetResultInput(fuzzed);
  EXPECT_EQ(controller->Fuzz(&result), ZX_OK);
  ASSERT_TRUE(result.is_response()) << zx_status_get_string(result.err());
  auto& response = result.response();
  EXPECT_EQ(response.result, FuzzResult::CRASH);
  auto received = Receive(std::move(response.error_input));
  EXPECT_EQ(received.ToHex(), fuzzed.ToHex());
}

TEST_F(ControllerTest, Merge) {
  auto controller = Bind();
  zx_status_t result;

  SetError(ZX_ERR_WRONG_TYPE);
  EXPECT_EQ(controller->Merge(&result), ZX_OK);
  EXPECT_EQ(result, ZX_ERR_WRONG_TYPE);

  SetError(ZX_OK);
  EXPECT_EQ(controller->Merge(&result), ZX_OK);
  EXPECT_EQ(result, ZX_OK);
}

}  // namespace
}  // namespace fuzzing
