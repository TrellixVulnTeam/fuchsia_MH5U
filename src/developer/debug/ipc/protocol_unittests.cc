// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/developer/debug/ipc/agent_protocol.h"
#include "src/developer/debug/ipc/client_protocol.h"
#include "src/developer/debug/ipc/message_reader.h"
#include "src/developer/debug/ipc/message_writer.h"
#include "src/developer/debug/ipc/protocol_helpers.h"
#include "src/developer/debug/shared/register_test_support.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_ipc {

namespace {

using debug::RegisterID;

template <typename RequestType>
bool SerializeDeserializeRequest(const RequestType& in, RequestType* out) {
  MessageWriter writer;
  uint32_t in_transaction_id = 32;
  WriteRequest(in, in_transaction_id, &writer);

  std::vector<char> serialized = writer.MessageComplete();

  MessageReader reader(std::move(serialized));
  uint32_t out_transaction_id = 0;
  if (!ReadRequest(&reader, out, &out_transaction_id))
    return false;
  EXPECT_EQ(in_transaction_id, out_transaction_id);
  return true;
}

template <typename ReplyType>
bool SerializeDeserializeReply(const ReplyType& in, ReplyType* out) {
  MessageWriter writer;
  uint32_t in_transaction_id = 32;
  WriteReply(in, in_transaction_id, &writer);

  std::vector<char> serialized = writer.MessageComplete();

  MessageReader reader(std::move(serialized));
  uint32_t out_transaction_id = 0;
  if (!ReadReply(&reader, out, &out_transaction_id))
    return false;
  EXPECT_EQ(in_transaction_id, out_transaction_id);
  return true;
}

template <typename NotificationType>
bool SerializeDeserializeNotification(const NotificationType& in, NotificationType* out,
                                      void (*write_fn)(const NotificationType&, MessageWriter*),
                                      bool (*read_fn)(MessageReader*, NotificationType*)) {
  MessageWriter writer;
  write_fn(in, &writer);

  MessageReader reader(writer.MessageComplete());
  return read_fn(&reader, out);
}

}  // namespace

constexpr uint64_t kTestTimestampDefault = 0x74657374l;  // hexadecimal for "test" in ascii

// ConfigAgent -------------------------------------------------------------------------------------

TEST(Protocol, ConfigAgentRequest) {
  ConfigAgentRequest initial;
  initial.actions.push_back({ConfigAction::Type::kQuitOnExit, "true"});
  initial.actions.push_back({ConfigAction::Type::kQuitOnExit, "false"});
  initial.actions.push_back({ConfigAction::Type::kQuitOnExit, "bla"});

  ConfigAgentRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  ASSERT_EQ(second.actions.size(), 3u);
  EXPECT_EQ(second.actions[0].type, initial.actions[0].type);
  EXPECT_EQ(second.actions[0].value, initial.actions[0].value);
  EXPECT_EQ(second.actions[1].type, initial.actions[1].type);
  EXPECT_EQ(second.actions[1].value, initial.actions[1].value);
  EXPECT_EQ(second.actions[2].type, initial.actions[2].type);
  EXPECT_EQ(second.actions[2].value, initial.actions[2].value);
}

TEST(Protocol, ConfigAgentReply) {
  ConfigAgentReply initial;
  initial.results.push_back(debug::Status());
  initial.results.push_back(debug::Status("This is an error message"));
  initial.results.push_back(
      debug::Status(debug::Status::InternalValues(), debug::Status::kPlatformError, 45, "foo"));

  ConfigAgentReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(second.results.size(), 3u);
  EXPECT_EQ(second.results[0], initial.results[0]);
  EXPECT_EQ(second.results[1], initial.results[1]);
  EXPECT_EQ(second.results[2], initial.results[2]);
}

// Hello -------------------------------------------------------------------------------------------

TEST(Protocol, HelloRequest) {
  HelloRequest initial;
  HelloRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
}

TEST(Protocol, HelloReply) {
  HelloReply initial;
  initial.version = 12345678;
  HelloReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(initial.version, second.version);
}

// Status ------------------------------------------------------------------------------------------

namespace {

ThreadRecord CreateThreadRecord(uint32_t process_koid, uint32_t thread_koid) {
  ThreadRecord record;
  record.id = {.process = process_koid, .thread = thread_koid};
  record.name = fxl::StringPrintf("thread-%u", thread_koid);
  return record;
}

ProcessRecord CreateProcessRecord(uint32_t process_koid, uint32_t thread_count) {
  ProcessRecord record;
  record.process_koid = process_koid;
  record.process_name = fxl::StringPrintf("process-%u", process_koid);

  record.threads.reserve(thread_count);
  for (uint32_t i = 0; i < thread_count; i++) {
    record.threads.push_back(CreateThreadRecord(process_koid, i));
  }

  return record;
}

}  // namespace

TEST(Protocol, StatusRequest) {
  StatusRequest initial;
  StatusRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
}

TEST(Protocol, StatusReply) {
  StatusReply one;
  one.processes.push_back(CreateProcessRecord(0x1, 1));
  one.processes.push_back(CreateProcessRecord(0x2, 2));

  one.limbo.push_back(CreateProcessRecord(0x3, 3));

  StatusReply two;
  ASSERT_TRUE(SerializeDeserializeReply(one, &two));

  // clang-format off
  ASSERT_EQ(two.processes.size(), 2u);
  EXPECT_EQ(two.processes[0].process_koid,    one.processes[0].process_koid);
  EXPECT_EQ(two.processes[0].process_name,    one.processes[0].process_name);
  ASSERT_EQ(two.processes[0].threads.size(), 1u);
  ASSERT_EQ(two.processes[0].threads[0].id,   one.processes[0].threads[0].id);
  ASSERT_EQ(two.processes[0].threads[0].name, one.processes[0].threads[0].name);

  EXPECT_EQ(two.processes[1].process_koid,    one.processes[1].process_koid);
  EXPECT_EQ(two.processes[1].process_name,    one.processes[1].process_name);
  ASSERT_EQ(two.processes[1].threads.size(),  2u);
  ASSERT_EQ(two.processes[1].threads[0].id,   one.processes[1].threads[0].id);
  ASSERT_EQ(two.processes[1].threads[0].name, one.processes[1].threads[0].name);
  ASSERT_EQ(two.processes[1].threads[1].id,   one.processes[1].threads[1].id);
  ASSERT_EQ(two.processes[1].threads[1].name, one.processes[1].threads[1].name);

  ASSERT_EQ(two.limbo.size(), 1u);
  EXPECT_EQ(two.limbo[0].process_koid,    one.limbo[0].process_koid);
  EXPECT_EQ(two.limbo[0].process_name,    one.limbo[0].process_name);
  ASSERT_EQ(two.limbo[0].threads.size(),  3u);
  ASSERT_EQ(two.limbo[0].threads[0].id,   one.limbo[0].threads[0].id);
  ASSERT_EQ(two.limbo[0].threads[0].name, one.limbo[0].threads[0].name);
  ASSERT_EQ(two.limbo[0].threads[1].id,   one.limbo[0].threads[1].id);
  ASSERT_EQ(two.limbo[0].threads[1].name, one.limbo[0].threads[1].name);
  ASSERT_EQ(two.limbo[0].threads[2].id,   one.limbo[0].threads[2].id);
  ASSERT_EQ(two.limbo[0].threads[2].name, one.limbo[0].threads[2].name);
  // clang-format on
}

// ProcessStatus -----------------------------------------------------------------------------------

TEST(Protocol, ProcessStatusRequest) {
  ProcessStatusRequest initial;
  initial.process_koid = 0x1234;

  ProcessStatusRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(second.process_koid, initial.process_koid);
}

TEST(Protocol, ProcessStatusReply) {
  ProcessStatusReply initial;
  initial.status = debug::Status();

  ProcessStatusReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(second.status, initial.status);
}

// Launch ------------------------------------------------------------------------------------------

TEST(Protocol, LaunchRequest) {
  LaunchRequest initial;
  initial.inferior_type = InferiorType::kBinary;
  initial.argv.push_back("/usr/bin/WINWORD.EXE");
  initial.argv.push_back("--dosmode");

  LaunchRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(second.inferior_type, InferiorType::kBinary);
  ASSERT_EQ(initial.argv.size(), second.argv.size());
  for (size_t i = 0; i < initial.argv.size(); i++)
    EXPECT_EQ(initial.argv[i], second.argv[i]);
}

TEST(Protocol, LaunchReply) {
  LaunchReply initial;
  initial.inferior_type = InferiorType::kComponent;
  initial.status = debug::Status(debug::Status::InternalValues(), debug::Status::kPlatformError,
                                 1346, "message");
  initial.process_id = 0x1234;
  initial.component_id = 0x5678;
  initial.process_name = "winword.exe";
  initial.timestamp = kTestTimestampDefault;

  LaunchReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(second.inferior_type, InferiorType::kComponent);
  EXPECT_EQ(initial.status, second.status);
  EXPECT_EQ(initial.process_id, second.process_id);
  EXPECT_EQ(initial.component_id, second.component_id);
  EXPECT_EQ(initial.process_name, second.process_name);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

// Kill --------------------------------------------------------------------------------------------

TEST(Protocol, KillRequest) {
  KillRequest initial;
  initial.process_koid = 5678;

  KillRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.process_koid, second.process_koid);
}

TEST(Protocol, KillReply) {
  KillReply initial;
  initial.timestamp = kTestTimestampDefault;
  initial.status = debug::Status();

  KillReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(initial.timestamp, second.timestamp);
  EXPECT_EQ(initial.status, second.status);
}

// Attach ------------------------------------------------------------------------------------------

TEST(Protocol, AttachRequest) {
  AttachRequest initial;
  initial.type = TaskType::kComponentRoot;
  initial.koid = 5678;

  AttachRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.type, second.type);
  EXPECT_EQ(initial.koid, second.koid);
}

TEST(Protocol, AttachReply) {
  AttachReply initial;
  initial.koid = 2312;
  initial.timestamp = kTestTimestampDefault;
  initial.status = debug::Status();
  initial.name = "virtual console";

  AttachReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(initial.timestamp, second.timestamp);
  EXPECT_EQ(initial.status, second.status);
  EXPECT_EQ(initial.name, second.name);
}

// Detach ------------------------------------------------------------------------------------------

TEST(Protocol, DetachRequest) {
  DetachRequest initial;
  initial.koid = 5678;
  initial.type = TaskType::kJob;

  DetachRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.koid, second.koid);
  EXPECT_EQ(initial.type, second.type);
}

TEST(Protocol, DetachReply) {
  DetachReply initial;
  initial.timestamp = kTestTimestampDefault;
  initial.status = debug::Status();

  DetachReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(initial.timestamp, second.timestamp);
  EXPECT_EQ(initial.status, second.status);
}

// Pause -------------------------------------------------------------------------------------------

TEST(Protocol, PauseRequest) {
  PauseRequest initial;
  initial.ids.push_back({.process = 12, .thread = 0});
  initial.ids.push_back({.process = 3746234, .thread = 123523});

  PauseRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.ids, second.ids);
}

TEST(Protocol, PauseReply) {
  PauseReply initial;
  initial.threads.resize(2);
  initial.threads[0].id = {.process = 41, .thread = 1234};
  initial.threads[0].name = "thread 0";
  initial.threads[1].id = {.process = 42, .thread = 5678};
  initial.threads[1].name = "thread 1";

  PauseReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  ASSERT_EQ(initial.threads.size(), second.threads.size());
  for (size_t i = 0; i < initial.threads.size(); i++) {
    EXPECT_EQ(initial.threads[i].id, second.threads[i].id);
    EXPECT_EQ(initial.threads[i].name, second.threads[i].name);
  }
}

// Resume ------------------------------------------------------------------------------------------

TEST(Protocol, ResumeRequest) {
  ResumeRequest initial;
  initial.ids.push_back({.process = 3746234, .thread = 123523});
  initial.how = ResumeRequest::How::kStepInRange;
  initial.range_begin = 0x12345;
  initial.range_end = 0x123456;

  ResumeRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.ids, second.ids);
  EXPECT_EQ(initial.how, second.how);
  EXPECT_EQ(initial.range_begin, second.range_begin);
  EXPECT_EQ(initial.range_end, second.range_end);
}

// ProcessTree -------------------------------------------------------------------------------------

TEST(Protocol, ProcessTreeRequest) {
  ProcessTreeRequest initial;
  ProcessTreeRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
}

TEST(Protocol, ProcessTreeReply) {
  ProcessTreeReply initial;
  initial.root.type = ProcessTreeRecord::Type::kJob;
  initial.root.koid = 1234;
  initial.root.name = "root";

  initial.root.children.resize(1);
  initial.root.children[0].type = ProcessTreeRecord::Type::kProcess;
  initial.root.children[0].koid = 3456;
  initial.root.children[0].name = "hello";

  ProcessTreeReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(initial.root.type, second.root.type);
  EXPECT_EQ(initial.root.koid, second.root.koid);
  EXPECT_EQ(initial.root.name, second.root.name);
  ASSERT_EQ(initial.root.children.size(), second.root.children.size());
  EXPECT_EQ(initial.root.children[0].type, second.root.children[0].type);
  EXPECT_EQ(initial.root.children[0].koid, second.root.children[0].koid);
  EXPECT_EQ(initial.root.children[0].name, second.root.children[0].name);
}

// Threads -----------------------------------------------------------------------------------------

TEST(Protocol, ThreadsRequest) {
  ThreadsRequest initial;
  initial.process_koid = 36473476;

  ThreadsRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.process_koid, second.process_koid);
}

TEST(Protocol, ThreadsReply) {
  ThreadsReply initial;
  initial.threads.resize(2);
  initial.threads[0].id = {.process = 41, .thread = 1234};
  initial.threads[0].name = "one";
  initial.threads[1].id = {.process = 42, .thread = 7634};
  initial.threads[1].name = "two";

  ThreadsReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(initial.threads.size(), second.threads.size());
  EXPECT_EQ(initial.threads[0].id, second.threads[0].id);
  EXPECT_EQ(initial.threads[0].name, second.threads[0].name);
  EXPECT_EQ(initial.threads[1].id, second.threads[1].id);
  EXPECT_EQ(initial.threads[1].name, second.threads[1].name);
}

// ReadMemory --------------------------------------------------------------------------------------

TEST(Protocol, ReadMemoryRequest) {
  ReadMemoryRequest initial;
  initial.process_koid = 91823765;
  initial.address = 983462384;
  initial.size = 93453926;

  ReadMemoryRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.process_koid, second.process_koid);
  EXPECT_EQ(initial.address, second.address);
  EXPECT_EQ(initial.size, second.size);
}

TEST(Protocol, ReadMemoryReply) {
  ReadMemoryReply initial;
  initial.blocks.resize(2);
  initial.blocks[0].address = 876234;
  initial.blocks[0].valid = true;
  initial.blocks[0].size = 12;
  for (uint64_t i = 0; i < initial.blocks[0].size; i++)
    initial.blocks[0].data.push_back(static_cast<uint8_t>(i));

  initial.blocks[1].address = 89362454;
  initial.blocks[1].valid = false;
  initial.blocks[1].size = 0;

  ReadMemoryReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(initial.blocks.size(), second.blocks.size());

  EXPECT_EQ(initial.blocks[0].address, second.blocks[0].address);
  EXPECT_EQ(initial.blocks[0].valid, second.blocks[0].valid);
  EXPECT_EQ(initial.blocks[0].size, second.blocks[0].size);
  EXPECT_EQ(second.blocks[0].size, second.blocks[0].data.size());
  for (uint64_t i = 0; i < second.blocks[0].size; i++)
    EXPECT_EQ(static_cast<uint8_t>(i), second.blocks[0].data[i]);

  EXPECT_EQ(initial.blocks[1].address, second.blocks[1].address);
  EXPECT_EQ(initial.blocks[1].valid, second.blocks[1].valid);
  EXPECT_EQ(initial.blocks[1].size, second.blocks[1].size);
  EXPECT_TRUE(second.blocks[1].data.empty());
}

// AddOrChangeBreakpoint ---------------------------------------------------------------------------

TEST(Protocol, AddOrChangeBreakpointRequest) {
  AddOrChangeBreakpointRequest initial;
  initial.breakpoint.id = 8976;
  initial.breakpoint.type = BreakpointType::kHardware;
  initial.breakpoint.name = "Some name";
  initial.breakpoint.stop = debug_ipc::Stop::kProcess;
  initial.breakpoint.locations.resize(1);
  initial.breakpoint.has_automation = true;

  initial.breakpoint.instructions.emplace_back();

  AutomationOperand address;
  AutomationOperand length;
  AutomationOperand struct_pointer_offset;
  AutomationOperand struct_length_offset;
  uint32_t item_size = 32;
  std::vector<AutomationCondition> conditions;
  address.InitRegister(RegisterID::kARMv8_x0);
  length.InitRegisterTimesConstant(RegisterID::kARMv8_x1, 4);
  struct_pointer_offset.InitConstant(8);
  struct_length_offset.InitConstant(16);

  conditions.emplace_back();
  conditions.emplace_back();
  conditions[0].InitEquals(address, 0xdeadbeef);
  conditions[1].InitMaskAndEquals(length, 0, 1);

  initial.breakpoint.instructions[0].InitLoopLoadMemory(
      address, length, struct_pointer_offset, struct_length_offset, item_size, conditions);

  ProcessBreakpointSettings& pr_settings = initial.breakpoint.locations.back();
  pr_settings.id = {.process = 1234, .thread = 14612};
  pr_settings.address = 0x723456234;
  pr_settings.address_range = {0x1234, 0x5678};

  AddOrChangeBreakpointRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.breakpoint.id, second.breakpoint.id);
  EXPECT_EQ(initial.breakpoint.type, second.breakpoint.type);
  EXPECT_EQ(initial.breakpoint.name, second.breakpoint.name);
  EXPECT_EQ(initial.breakpoint.stop, second.breakpoint.stop);
  EXPECT_EQ(initial.breakpoint.has_automation, second.breakpoint.has_automation);
  ASSERT_EQ(initial.breakpoint.locations.size(), second.breakpoint.locations.size());

  EXPECT_EQ(initial.breakpoint.locations[0].id, second.breakpoint.locations[0].id);
  EXPECT_EQ(initial.breakpoint.locations[0].address, second.breakpoint.locations[0].address);
  EXPECT_EQ(initial.breakpoint.locations[0].address_range,
            second.breakpoint.locations[0].address_range);

  ASSERT_EQ(initial.breakpoint.instructions.size(), second.breakpoint.instructions.size());

  EXPECT_EQ(initial.breakpoint.instructions[0].kind(), second.breakpoint.instructions[0].kind());

  EXPECT_EQ(initial.breakpoint.instructions[0].address().kind(),
            second.breakpoint.instructions[0].address().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].address().index(),
            second.breakpoint.instructions[0].address().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].address().value(),
            second.breakpoint.instructions[0].address().value());

  EXPECT_EQ(initial.breakpoint.instructions[0].length().kind(),
            second.breakpoint.instructions[0].length().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].length().index(),
            second.breakpoint.instructions[0].length().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].length().value(),
            second.breakpoint.instructions[0].length().value());

  EXPECT_EQ(initial.breakpoint.instructions[0].extra_1().kind(),
            second.breakpoint.instructions[0].extra_1().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].extra_1().index(),
            second.breakpoint.instructions[0].extra_1().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].extra_1().value(),
            second.breakpoint.instructions[0].extra_1().value());

  EXPECT_EQ(initial.breakpoint.instructions[0].extra_2().kind(),
            second.breakpoint.instructions[0].extra_2().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].extra_2().index(),
            second.breakpoint.instructions[0].extra_2().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].extra_2().value(),
            second.breakpoint.instructions[0].extra_2().value());

  EXPECT_EQ(initial.breakpoint.instructions[0].value(), second.breakpoint.instructions[0].value());

  ASSERT_EQ(initial.breakpoint.instructions[0].conditions().size(),
            second.breakpoint.instructions[0].conditions().size());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].kind(),
            second.breakpoint.instructions[0].conditions()[0].kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].operand().kind(),
            second.breakpoint.instructions[0].conditions()[0].operand().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].operand().index(),
            second.breakpoint.instructions[0].conditions()[0].operand().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].operand().value(),
            second.breakpoint.instructions[0].conditions()[0].operand().value());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].constant(),
            second.breakpoint.instructions[0].conditions()[0].constant());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[0].mask(),
            second.breakpoint.instructions[0].conditions()[0].mask());

  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].kind(),
            second.breakpoint.instructions[0].conditions()[1].kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].operand().kind(),
            second.breakpoint.instructions[0].conditions()[1].operand().kind());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].operand().index(),
            second.breakpoint.instructions[0].conditions()[1].operand().index());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].operand().value(),
            second.breakpoint.instructions[0].conditions()[1].operand().value());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].constant(),
            second.breakpoint.instructions[0].conditions()[1].constant());
  EXPECT_EQ(initial.breakpoint.instructions[0].conditions()[1].mask(),
            second.breakpoint.instructions[0].conditions()[1].mask());
}

TEST(Protocol, AddOrChangeBreakpointReply) {
  AddOrChangeBreakpointReply initial;
  initial.status = debug::Status("error");

  AddOrChangeBreakpointReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(initial.status, second.status);
}

// RemoveBreakpoint --------------------------------------------------------------------------------

TEST(Protocol, RemoveBreakpointRequest) {
  RemoveBreakpointRequest initial;
  initial.breakpoint_id = 8976;

  RemoveBreakpointRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.breakpoint_id, second.breakpoint_id);
}

TEST(Protocol, RemoveBreakpointReply) {
  RemoveBreakpointReply initial;
  RemoveBreakpointReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
}

// SysInfo -----------------------------------------------------------------------------------------

TEST(Protocol, SysInfoRequest) {
  SysInfoRequest initial;
  SysInfoRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
}

TEST(Protocol, SysInfoReply) {
  SysInfoReply initial;
  initial.version = "VERSION";
  initial.num_cpus = 16;
  initial.memory_mb = 4096;
  initial.hw_breakpoint_count = 6;
  initial.hw_watchpoint_count = 4;

  SysInfoReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(initial.version, second.version);
  EXPECT_EQ(initial.num_cpus, second.num_cpus);
  EXPECT_EQ(initial.memory_mb, second.memory_mb);
  EXPECT_EQ(initial.hw_breakpoint_count, second.hw_breakpoint_count);
  EXPECT_EQ(initial.hw_watchpoint_count, second.hw_watchpoint_count);
}

// ThreadStatus ------------------------------------------------------------------------------------

TEST(Protocol, ThreadStatusRequest) {
  ThreadStatusRequest initial;
  initial.id = {.process = 1234, .thread = 8976};

  ThreadStatusRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.id, second.id);
}

TEST(Protocol, ThreadStatusReply) {
  ThreadStatusReply initial;
  initial.record.id = {.process = 42, .thread = 1234};
  initial.record.name = "Spartacus";
  initial.record.state = ThreadRecord::State::kRunning;
  initial.record.stack_amount = ThreadRecord::StackAmount::kFull;
  initial.record.frames.emplace_back(
      1234, 9875, 89236413,
      std::vector<debug::RegisterValue>{{RegisterID::kX64_rsi, static_cast<uint64_t>(12)},
                                        {RegisterID::kX64_rdi, static_cast<uint64_t>(0)}});
  initial.record.frames.emplace_back(
      71562341, 89236413, 0,
      std::vector<debug::RegisterValue>{{RegisterID::kX64_rsi, static_cast<uint64_t>(11u)},
                                        {RegisterID::kX64_rdi, static_cast<uint64_t>(1u)}});

  ThreadStatusReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(2u, second.record.frames.size());
  EXPECT_EQ(initial.record.id, second.record.id);
  EXPECT_EQ(initial.record.name, second.record.name);
  EXPECT_EQ(initial.record.state, second.record.state);
  EXPECT_EQ(initial.record.stack_amount, second.record.stack_amount);
  EXPECT_EQ(initial.record.frames[0], second.record.frames[0]);
  EXPECT_EQ(initial.record.frames[1], second.record.frames[1]);
}

// Modules -----------------------------------------------------------------------------------------

TEST(Protocol, ModulesRequest) {
  ModulesRequest initial;
  initial.process_koid = 1234;

  ModulesRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.process_koid, second.process_koid);
}

TEST(Protocol, ModulesReply) {
  ModulesReply initial;
  initial.modules.resize(2);
  initial.modules[0].name = "winnt.dll";
  initial.modules[0].base = 0x1234567890;
  initial.modules[1].name = "libncurses.so.1.0.0";
  initial.modules[1].base = 0x1000;

  ModulesReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(2u, second.modules.size());
  EXPECT_EQ(initial.modules[0].name, second.modules[0].name);
  EXPECT_EQ(initial.modules[0].base, second.modules[0].base);
  EXPECT_EQ(initial.modules[1].name, second.modules[1].name);
  EXPECT_EQ(initial.modules[1].base, second.modules[1].base);
}

// ASpace ------------------------------------------------------------------------------------------

TEST(Protocol, AspaceRequest) {
  AddressSpaceRequest initial;
  initial.process_koid = 1234;
  initial.address = 0x717171;

  AddressSpaceRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.process_koid, second.process_koid);
  EXPECT_EQ(initial.address, second.address);
}

TEST(Protocol, AspaceReply) {
  const uint64_t kOneT = 1024 * 1024u * 1024u * 1024ull;
  AddressSpaceReply initial;

  initial.map.resize(4u);
  initial.map[0] = AddressRegion{"proc:5616", 0x1000000, 127 * kOneT, 0};
  initial.map[1] = AddressRegion{"root", 0x1000000, 127 * kOneT, 0};
  initial.map[2] = AddressRegion{"useralloc", 0x371f1276000, 12 * 1024, 1};
  initial.map[3] = AddressRegion{"initial-thread", 0x371f1277000, 4 * 1024, 2};

  AddressSpaceReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(4u, second.map.size());
  EXPECT_EQ(initial.map[0].name, second.map[0].name);
  EXPECT_EQ(initial.map[0].base, second.map[0].base);
  EXPECT_EQ(initial.map[0].size, second.map[0].size);
  EXPECT_EQ(initial.map[0].depth, second.map[0].depth);
  EXPECT_EQ(initial.map[1].name, second.map[1].name);
  EXPECT_EQ(initial.map[1].base, second.map[1].base);
  EXPECT_EQ(initial.map[1].size, second.map[1].size);
  EXPECT_EQ(initial.map[1].depth, second.map[1].depth);
  EXPECT_EQ(initial.map[2].name, second.map[2].name);
  EXPECT_EQ(initial.map[2].base, second.map[2].base);
  EXPECT_EQ(initial.map[2].size, second.map[2].size);
  EXPECT_EQ(initial.map[2].depth, second.map[2].depth);
  EXPECT_EQ(initial.map[3].name, second.map[3].name);
  EXPECT_EQ(initial.map[3].base, second.map[3].base);
  EXPECT_EQ(initial.map[3].size, second.map[3].size);
  EXPECT_EQ(initial.map[3].depth, second.map[3].depth);
}

// JobFilter --------------------------------------------------------------------------------------

TEST(Protocol, JobFilterRequest) {
  JobFilterRequest initial;
  initial.job_koid = 5678;
  initial.filters.push_back("Clock");
  initial.filters.push_back("Time");
  initial.filters.push_back("Network");

  JobFilterRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.job_koid, second.job_koid);
  EXPECT_EQ(initial.filters, second.filters);
}

TEST(Protocol, JobFilterReply) {
  JobFilterReply initial;
  initial.status = debug::Status();
  initial.matched_processes = {1234, 5678};

  JobFilterReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));
  EXPECT_EQ(initial.status, second.status);
  ASSERT_EQ(second.matched_processes.size(), 2u);
  EXPECT_EQ(second.matched_processes[0], initial.matched_processes[0]);
  EXPECT_EQ(second.matched_processes[1], initial.matched_processes[1]);
}

// WriteMemory -------------------------------------------------------------------------------------

TEST(Protocol, WriteMemoryRequest) {
  WriteMemoryRequest initial;
  initial.process_koid = 91823765;
  initial.address = 0x3468234;
  initial.data = {0, 1, 2, 3, 4, 5};

  WriteMemoryRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.process_koid, second.process_koid);
  EXPECT_EQ(initial.address, second.address);
  ASSERT_EQ(initial.data.size(), second.data.size());
  for (size_t i = 0; i < initial.data.size(); i++)
    EXPECT_EQ(initial.data[i], second.data[i]);
}

TEST(Protocol, WriteMemoryReply) {
  WriteMemoryReply initial;
  initial.status = debug::Status();

  WriteMemoryReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(initial.status, second.status);
}

// LoadInfoHandlTable ------------------------------------------------------------------------------

TEST(Protocol, LoadInfoHandleTableRequest) {
  LoadInfoHandleTableRequest initial;
  initial.process_koid = 91823765;

  LoadInfoHandleTableRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  EXPECT_EQ(initial.process_koid, second.process_koid);
}

TEST(Protocol, LoadInfoHandleTableReply) {
  LoadInfoHandleTableReply initial;
  initial.status = debug::Status();

  InfoHandle info;
  info.type = 4;
  info.handle_value = 0x1234;
  info.rights = 0xe;
  info.koid = 884422;
  info.related_koid = 91823766;
  info.peer_owner_koid = 91823800;
  initial.handles.push_back(info);

  info.type = 3;
  info.handle_value = 0x1235;
  info.rights = 0xc;
  info.koid = 884433;
  info.related_koid = 91823767;
  info.peer_owner_koid = 91823801;
  info.ext.vmo.metadata_bytes = 712;
  initial.handles.push_back(info);

  LoadInfoHandleTableReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(initial.status, second.status);
  ASSERT_EQ(initial.handles.size(), second.handles.size());
  for (size_t i = 0; i < initial.handles.size(); ++i) {
    ASSERT_EQ(initial.handles[i].type, second.handles[i].type);
    ASSERT_EQ(initial.handles[i].handle_value, second.handles[i].handle_value);
    ASSERT_EQ(initial.handles[i].rights, second.handles[i].rights);
    ASSERT_EQ(initial.handles[i].koid, second.handles[i].koid);
    ASSERT_EQ(initial.handles[i].related_koid, second.handles[i].related_koid);
    ASSERT_EQ(initial.handles[i].peer_owner_koid, second.handles[i].peer_owner_koid);
    ASSERT_EQ(initial.handles[i].ext.vmo.metadata_bytes, second.handles[i].ext.vmo.metadata_bytes);
  }
}

// UpdateGlobalSettings ---------------------------------------------------------------------------

TEST(Protocol, UpdateGlobalSettingsRequest) {
  UpdateGlobalSettingsRequest initial;
  initial.exception_strategies = {
      {
          .type = ExceptionType::kPageFault,
          .value = ExceptionStrategy::kSecondChance,
      },
      {
          .type = ExceptionType::kUnalignedAccess,
          .value = ExceptionStrategy::kFirstChance,
      },
  };

  UpdateGlobalSettingsRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));
  ASSERT_EQ(initial.exception_strategies.size(), 2u);
  ASSERT_EQ(initial.exception_strategies.size(), second.exception_strategies.size());
  EXPECT_EQ(initial.exception_strategies[0].type, second.exception_strategies[0].type);
  EXPECT_EQ(initial.exception_strategies[0].value, second.exception_strategies[0].value);
  EXPECT_EQ(initial.exception_strategies[1].type, second.exception_strategies[1].type);
  EXPECT_EQ(initial.exception_strategies[1].value, second.exception_strategies[1].value);
}

TEST(Protocol, UpdateGlobalSettingsReply) {
  UpdateGlobalSettingsReply initial;
  initial.status =
      debug::Status(debug::Status::InternalValues(), debug::Status::kPlatformError, 12345, "foo");

  UpdateGlobalSettingsReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(initial.status, second.status);
}

// Registers ---------------------------------------------------------------------------------------

TEST(Protocol, ReadRegistersRequest) {
  ReadRegistersRequest initial;
  initial.id = {.process = 0x1234, .thread = 0x5678};
  initial.categories.push_back(debug::RegisterCategory::kGeneral);
  initial.categories.push_back(debug::RegisterCategory::kVector);

  ReadRegistersRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.id, second.id);
  EXPECT_EQ(initial.categories, second.categories);
}

TEST(Protocol, ReadRegistersReply) {
  ReadRegistersReply initial;

  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_lr, 1));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_pc, 2));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_sp, 4));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_cpsr, 8));

  // Sanity check
  ASSERT_EQ(*(uint8_t*)&(initial.registers[0].data[0]), 0x01u);
  ASSERT_EQ(*(uint16_t*)&(initial.registers[1].data[0]), 0x0102u);
  ASSERT_EQ(*(uint32_t*)&(initial.registers[2].data[0]), 0x01020304u);
  ASSERT_EQ(*(uint64_t*)&(initial.registers[3].data[0]), 0x0102030405060708u);

  ReadRegistersReply second;
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  ASSERT_EQ(second.registers.size(), 4u);

  EXPECT_EQ(second.registers[0].id, initial.registers[0].id);
  EXPECT_EQ(second.registers[0].data, initial.registers[0].data);
  EXPECT_EQ(second.registers[1].id, initial.registers[1].id);
  EXPECT_EQ(second.registers[1].data, initial.registers[1].data);
  EXPECT_EQ(second.registers[2].id, initial.registers[2].id);
  EXPECT_EQ(second.registers[2].data, initial.registers[2].data);
}

TEST(Protocol, WriteRegistersRequest) {
  WriteRegistersRequest initial;
  initial.id = {.process = 0x1234, .thread = 0x5678};
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x0, 1));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x1, 2));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x2, 4));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x3, 8));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x4, 16));

  WriteRegistersRequest second;
  ASSERT_TRUE(SerializeDeserializeRequest(initial, &second));

  EXPECT_EQ(initial.id, second.id);
  ASSERT_EQ(second.registers.size(), 5u);
  EXPECT_EQ(second.registers[0].id, initial.registers[0].id);
  EXPECT_EQ(second.registers[0].data, initial.registers[0].data);
  EXPECT_EQ(second.registers[1].id, initial.registers[1].id);
  EXPECT_EQ(second.registers[1].data, initial.registers[1].data);
  EXPECT_EQ(second.registers[2].id, initial.registers[2].id);
  EXPECT_EQ(second.registers[2].data, initial.registers[2].data);
  EXPECT_EQ(second.registers[3].id, initial.registers[3].id);
  EXPECT_EQ(second.registers[3].data, initial.registers[3].data);
}

TEST(Protocol, WriteRegistersReply) {
  WriteRegistersReply initial = {};
  initial.status = debug::Status("Not so good");
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x0, 1));
  initial.registers.push_back(CreateRegisterWithTestData(RegisterID::kARMv8_x1, 2));

  WriteRegistersReply second = {};
  ASSERT_TRUE(SerializeDeserializeReply(initial, &second));

  EXPECT_EQ(second.status, initial.status);
  EXPECT_EQ(second.registers, initial.registers);
}

// Notifications -----------------------------------------------------------------------------------

TEST(Protocol, NotifyThread) {
  NotifyThread initial;
  initial.record.id = {.process = 9887, .thread = 1234};
  initial.record.name = "Wolfgang";
  initial.record.state = ThreadRecord::State::kDying;
  initial.record.stack_amount = ThreadRecord::StackAmount::kNone;
  initial.timestamp = kTestTimestampDefault;

  MessageWriter writer;
  WriteNotifyThread(MsgHeader::Type::kNotifyThreadStarting, initial, &writer);

  MessageReader reader(writer.MessageComplete());
  NotifyThread second;
  ASSERT_TRUE(ReadNotifyThread(&reader, &second));

  EXPECT_EQ(initial.record.id, second.record.id);
  EXPECT_EQ(initial.record.name, second.record.name);
  EXPECT_EQ(initial.record.state, second.record.state);
  EXPECT_EQ(initial.record.stack_amount, second.record.stack_amount);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

TEST(Protocol, NotifyException) {
  NotifyException initial;
  initial.thread.id = {.process = 23, .thread = 23};
  initial.thread.name = "foo";
  initial.thread.stack_amount = ThreadRecord::StackAmount::kMinimal;
  initial.thread.frames.emplace_back(0x7647342634, 0x9861238251);
  initial.type = ExceptionType::kHardwareBreakpoint;
  initial.timestamp = kTestTimestampDefault;

  initial.exception.arch.x64.vector = 22;
  initial.exception.arch.x64.err_code = 5;
  initial.exception.arch.x64.cr2 = 12345;
  initial.exception.strategy = ExceptionStrategy::kSecondChance;

  initial.hit_breakpoints.emplace_back();
  initial.hit_breakpoints[0].id = 45;
  initial.hit_breakpoints[0].hit_count = 15;
  initial.hit_breakpoints[0].should_delete = true;

  initial.hit_breakpoints.emplace_back();
  initial.hit_breakpoints[1].id = 46;
  initial.hit_breakpoints[1].hit_count = 16;
  initial.hit_breakpoints[1].should_delete = false;

  initial.other_affected_threads.emplace_back();
  initial.other_affected_threads[0].id = {.process = 667788, .thread = 990011};

  initial.memory_blocks.resize(2);
  initial.memory_blocks[0].address = 876234;
  initial.memory_blocks[0].valid = true;
  initial.memory_blocks[0].size = 12;
  for (uint64_t i = 0; i < initial.memory_blocks[0].size; i++)
    initial.memory_blocks[0].data.push_back(static_cast<uint8_t>(i));

  initial.memory_blocks[1].address = 89362454;
  initial.memory_blocks[1].valid = false;
  initial.memory_blocks[1].size = 0;

  NotifyException second;
  ASSERT_TRUE(SerializeDeserializeNotification(initial, &second, &WriteNotifyException,
                                               &ReadNotifyException));

  EXPECT_EQ(initial.thread.id, second.thread.id);
  EXPECT_EQ(initial.thread.name, second.thread.name);
  EXPECT_EQ(initial.thread.stack_amount, second.thread.stack_amount);
  EXPECT_EQ(initial.thread.frames[0], second.thread.frames[0]);
  EXPECT_EQ(initial.type, second.type);
  EXPECT_EQ(initial.timestamp, second.timestamp);

  EXPECT_EQ(initial.exception.arch.x64.vector, second.exception.arch.x64.vector);
  EXPECT_EQ(initial.exception.arch.x64.err_code, second.exception.arch.x64.err_code);
  EXPECT_EQ(initial.exception.arch.x64.cr2, second.exception.arch.x64.cr2);
  EXPECT_EQ(initial.exception.strategy, second.exception.strategy);

  ASSERT_EQ(initial.hit_breakpoints.size(), second.hit_breakpoints.size());

  EXPECT_EQ(initial.hit_breakpoints[0].id, second.hit_breakpoints[0].id);
  EXPECT_EQ(initial.hit_breakpoints[0].hit_count, second.hit_breakpoints[0].hit_count);
  EXPECT_EQ(initial.hit_breakpoints[0].should_delete, second.hit_breakpoints[0].should_delete);

  EXPECT_EQ(initial.hit_breakpoints[1].id, second.hit_breakpoints[1].id);
  EXPECT_EQ(initial.hit_breakpoints[1].hit_count, second.hit_breakpoints[1].hit_count);
  EXPECT_EQ(initial.hit_breakpoints[1].should_delete, second.hit_breakpoints[1].should_delete);

  ASSERT_EQ(initial.other_affected_threads.size(), second.other_affected_threads.size());
  EXPECT_EQ(initial.other_affected_threads[0].id, second.other_affected_threads[0].id);

  ASSERT_EQ(initial.memory_blocks.size(), second.memory_blocks.size());

  EXPECT_EQ(initial.memory_blocks[0].address, second.memory_blocks[0].address);
  EXPECT_EQ(initial.memory_blocks[0].valid, second.memory_blocks[0].valid);
  EXPECT_EQ(initial.memory_blocks[0].size, second.memory_blocks[0].size);
  EXPECT_EQ(second.memory_blocks[0].size, second.memory_blocks[0].data.size());
  EXPECT_EQ(initial.memory_blocks[0].data, second.memory_blocks[0].data);

  EXPECT_EQ(initial.memory_blocks[1].address, second.memory_blocks[1].address);
  EXPECT_EQ(initial.memory_blocks[1].valid, second.memory_blocks[1].valid);
  EXPECT_EQ(initial.memory_blocks[1].size, second.memory_blocks[1].size);
  EXPECT_TRUE(second.memory_blocks[1].data.empty());
}

TEST(Protocol, NotifyModules) {
  constexpr uint64_t kProcessKoid = 23;

  NotifyModules initial;
  initial.process_koid = kProcessKoid;
  initial.modules.resize(2);
  initial.modules[0].name = "foo";
  initial.modules[0].base = 0x12345;
  initial.modules[1].name = "bar";
  initial.modules[1].base = 0x43567;
  initial.stopped_threads.push_back({.process = kProcessKoid, .thread = 34});
  initial.stopped_threads.push_back({.process = kProcessKoid, .thread = 96});
  initial.timestamp = kTestTimestampDefault;

  NotifyModules second;
  ASSERT_TRUE(
      SerializeDeserializeNotification(initial, &second, &WriteNotifyModules, &ReadNotifyModules));

  EXPECT_EQ(initial.process_koid, second.process_koid);
  ASSERT_EQ(initial.modules.size(), second.modules.size());
  EXPECT_EQ(initial.modules[0].name, second.modules[0].name);
  EXPECT_EQ(initial.modules[0].base, second.modules[0].base);
  EXPECT_EQ(initial.modules[1].name, second.modules[1].name);
  EXPECT_EQ(initial.modules[1].base, second.modules[1].base);
  EXPECT_EQ(initial.stopped_threads, second.stopped_threads);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

TEST(Protocol, NotifyProcessStarting) {
  NotifyProcessStarting initial;
  initial.type = NotifyProcessStarting::Type::kLimbo;
  initial.koid = 10;
  initial.component_id = 2;
  initial.name = "some_process";
  initial.timestamp = kTestTimestampDefault;

  NotifyProcessStarting second;
  ASSERT_TRUE(SerializeDeserializeNotification(initial, &second, &WriteNotifyProcessStarting,
                                               &ReadNotifyProcessStarting));

  EXPECT_EQ(second.type, initial.type);
  EXPECT_EQ(initial.koid, second.koid);
  EXPECT_EQ(initial.component_id, second.component_id);
  EXPECT_EQ(initial.name, second.name);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

TEST(Protocol, NotifyProcessExiting) {
  NotifyProcessExiting initial;
  initial.process_koid = 10;
  initial.return_code = 3;
  initial.timestamp = kTestTimestampDefault;

  NotifyProcessExiting second;
  ASSERT_TRUE(SerializeDeserializeNotification(initial, &second, &WriteNotifyProcessExiting,
                                               &ReadNotifyProcessExiting));

  EXPECT_EQ(initial.process_koid, second.process_koid);
  EXPECT_EQ(initial.return_code, second.return_code);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

TEST(Protocol, NotifyIO) {
  NotifyIO initial;
  initial.process_koid = 1234;
  initial.type = NotifyIO::Type::kStderr;
  initial.data = "Some data";
  initial.more_data_available = true;
  initial.timestamp = kTestTimestampDefault;

  NotifyIO second;
  ASSERT_TRUE(SerializeDeserializeNotification(initial, &second, &WriteNotifyIO, &ReadNotifyIO));
  EXPECT_EQ(initial.process_koid, second.process_koid);
  EXPECT_EQ(initial.type, second.type);
  EXPECT_EQ(initial.data, second.data);
  EXPECT_EQ(initial.more_data_available, second.more_data_available);
  EXPECT_EQ(initial.timestamp, second.timestamp);
}

}  // namespace debug_ipc
