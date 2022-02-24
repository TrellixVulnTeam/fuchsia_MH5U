// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fpromise/single_threaded_executor.h>
#include <lib/inspect/cpp/hierarchy.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/inspect/cpp/reader.h>
#include <lib/inspect/cpp/vmo/block.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <zxtest/zxtest.h>

using inspect::Hierarchy;
using inspect::Inspector;
using inspect::MissingValueReason;
using inspect::Node;
using inspect::Snapshot;
using inspect::StringArrayValue;
using inspect::internal::ArrayBlockPayload;
using inspect::internal::Block;
using inspect::internal::BlockType;
using inspect::internal::ExtentBlockFields;
using inspect::internal::GetState;
using inspect::internal::HeaderBlockFields;
using inspect::internal::kMagicNumber;
using inspect::internal::kMinOrderSize;
using inspect::internal::LinkBlockDisposition;
using inspect::internal::NameBlockFields;
using inspect::internal::PropertyBlockPayload;
using inspect::internal::State;
using inspect::internal::StringReferenceBlockFields;
using inspect::internal::StringReferenceBlockPayload;
using inspect::internal::ValueBlockFields;

namespace {

TEST(Reader, GetByPath) {
  Inspector inspector;
  ASSERT_TRUE(bool(inspector));
  auto child = inspector.GetRoot().CreateChild("test");
  auto child2 = child.CreateChild("test2");

  auto result = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();

  EXPECT_NOT_NULL(hierarchy.GetByPath({"test"}));
  EXPECT_NOT_NULL(hierarchy.GetByPath({"test", "test2"}));
  EXPECT_NULL(hierarchy.GetByPath({"test", "test2", "test3"}));
}

void MakeStringReference(uint64_t index, const std::string_view data, uint64_t next_extent,
                         size_t total_size, std::vector<uint8_t>* buf) {
  auto* string_ref = reinterpret_cast<Block*>(buf->data() + kMinOrderSize * index);
  string_ref->header = StringReferenceBlockFields::Order::Make(0) |
                       StringReferenceBlockFields::Type::Make(BlockType::kStringReference) |
                       StringReferenceBlockFields::NextExtentIndex::Make(next_extent) |
                       StringReferenceBlockFields::ReferenceCount::Make(0);

  string_ref->payload.u64 = StringReferenceBlockPayload::TotalLength::Make(total_size);
  memcpy(string_ref->payload.data + StringReferenceBlockPayload::TotalLength::SizeInBytes(),
         data.data(), std::size(data));
}

void MakeHeader(std::vector<uint8_t>* buf) {
  auto* header = reinterpret_cast<Block*>(buf->data());
  header->header = HeaderBlockFields::Order::Make(0) |
                   HeaderBlockFields::Type::Make(BlockType::kHeader) |
                   HeaderBlockFields::Version::Make(0);
  memcpy(&header->header_data[4], kMagicNumber, 4);
  header->payload.u64 = 0;
}

TEST(Reader, InterpretInlineStringReferences) {
  std::vector<uint8_t> buf;
  buf.resize(128);

  MakeHeader(&buf);
  MakeStringReference(1, "a", 0, 1, &buf);

  Block* value = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 2);
  value->header = ValueBlockFields::Order::Make(0) |
                  ValueBlockFields::Type::Make(BlockType::kNodeValue) |
                  ValueBlockFields::ParentIndex::Make(0) | ValueBlockFields::NameIndex::Make(1);

  Block* value2 = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 3);
  value2->header = ValueBlockFields::Order::Make(0) |
                   ValueBlockFields::Type::Make(BlockType::kIntValue) |
                   ValueBlockFields::ParentIndex::Make(0) | ValueBlockFields::NameIndex::Make(1);
  value2->payload.i64 = 5;

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
  EXPECT_EQ("root", result.value().node().name());
  EXPECT_EQ(1u, result.value().children().size());
  EXPECT_EQ(1u, result.value().node().properties().size());
  EXPECT_EQ("a", result.value().children().at(0).node().name());
  EXPECT_EQ("a", result.value().node().properties().at(0).name());
}

TEST(Reader, InterpretStringArrays) {
  std::vector<uint8_t> buf(128);
  MakeHeader(&buf);
  auto name = std::make_pair("n", uint32_t(1));
  auto zero = std::make_pair("0", uint32_t(2));
  auto one = std::make_pair("1", uint32_t(3));
  auto two = std::make_pair("2", uint32_t(4));

  MakeStringReference(name.second, name.first, 0, 1, &buf);
  MakeStringReference(zero.second, zero.first, 0, 1, &buf);
  MakeStringReference(one.second, one.first, 0, 1, &buf);
  MakeStringReference(two.second, two.first, 0, 1, &buf);

  Block* string_array = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 5);
  string_array->header =
      ValueBlockFields::Type::Make(BlockType::kArrayValue) | ValueBlockFields::Order::Make(1) |
      ValueBlockFields::NameIndex::Make(name.second) | ValueBlockFields::ParentIndex::Make(0);
  string_array->payload.u64 =
      ArrayBlockPayload::EntryType::Make(BlockType::kStringReference) |
      ArrayBlockPayload::Flags::Make(inspect::internal::ArrayBlockFormat::kDefault) |
      ArrayBlockPayload::Count::Make(4);

  uint32_t indexes[] = {zero.second, one.second, two.second};
  memcpy(string_array->payload_ptr() + 8, indexes, 3 * sizeof(uint32_t));

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
  auto& root_node = result.value().node();

  EXPECT_EQ(1u, root_node.properties().size());

  auto& array_prop = root_node.properties()[0];
  std::vector<std::string> expected_data = {zero.first, one.first, two.first, ""};
  auto& actual_data = array_prop.Get<StringArrayValue>().value();
  EXPECT_EQ(name.first, array_prop.name());
  ASSERT_EQ(expected_data.size(), actual_data.size());

  for (size_t i = 0; i < expected_data.size(); ++i) {
    EXPECT_EQ(expected_data[i], actual_data[i]);
  }
}

TEST(Reader, InterpretStringReferences) {
  std::vector<uint8_t> buf;
  buf.resize(128);

  MakeHeader(&buf);

  // Manually create a node.
  MakeStringReference(1, "abcd", 2, 12, &buf);
  auto* next_extent = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 2);
  next_extent->header = ExtentBlockFields::Order::Make(0) |
                        ExtentBlockFields::Type::Make(BlockType::kExtent) |
                        ExtentBlockFields::NextExtentIndex::Make(0);
  memcpy(next_extent->payload.data, "efghijkl", 8);

  Block* value = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 3);
  value->header = ValueBlockFields::Order::Make(0) |
                  ValueBlockFields::Type::Make(BlockType::kNodeValue) |
                  ValueBlockFields::ParentIndex::Make(0) | ValueBlockFields::NameIndex::Make(1);

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
  EXPECT_EQ(result.value().node().name(), "root");
  EXPECT_EQ(1u, result.value().children().size());
  EXPECT_EQ("abcdefghijkl", result.value().children().at(0).node().name());
}

TEST(Reader, VisitHierarchy) {
  Inspector inspector;
  ASSERT_TRUE(bool(inspector));

  // root:
  //   test:
  //     test2
  //   test3
  auto child = inspector.GetRoot().CreateChild("test");
  auto child2 = child.CreateChild("test2");
  auto child3 = inspector.GetRoot().CreateChild("test3");

  auto result = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();
  hierarchy.Sort();

  std::vector<std::vector<std::string>> paths;
  hierarchy.Visit([&](const std::vector<std::string>& path, Hierarchy* current) {
    paths.push_back(path);
    EXPECT_NE(nullptr, current);
    return true;
  });

  std::vector<std::vector<std::string>> expected;
  expected.emplace_back(std::vector<std::string>{"root"});
  expected.emplace_back(std::vector<std::string>{"root", "test"});
  expected.emplace_back(std::vector<std::string>{"root", "test", "test2"});
  expected.emplace_back(std::vector<std::string>{"root", "test3"});
  EXPECT_EQ(expected, paths);

  paths.clear();
  hierarchy.Visit([&](const std::vector<std::string>& path, Hierarchy* current) {
    paths.push_back(path);
    EXPECT_NE(nullptr, current);
    return false;
  });
  EXPECT_EQ(1u, paths.size());
}

TEST(Reader, VisitHierarchyWithTombstones) {
  Inspector inspector;
  ASSERT_TRUE(bool(inspector));

  // root:
  //   test:
  //     test2
  auto child = inspector.GetRoot().CreateChild("test");
  auto child2 = child.CreateChild("test2");
  auto child3 = child2.CreateChild("test3");
  auto _prop = child2.CreateString("val", "test");
  // Delete node
  child2 = inspect::Node();

  auto result = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();
  hierarchy.Sort();

  std::vector<std::vector<std::string>> paths;
  hierarchy.Visit([&](const std::vector<std::string>& path, Hierarchy* current) {
    paths.push_back(path);
    EXPECT_NE(nullptr, current);
    return true;
  });

  std::vector<std::vector<std::string>> expected;
  expected.emplace_back(std::vector<std::string>{"root"});
  expected.emplace_back(std::vector<std::string>{"root", "test"});
  EXPECT_EQ(expected, paths);
}

TEST(Reader, BucketComparison) {
  inspect::UintArrayValue::HistogramBucket a(0, 2, 6);
  inspect::UintArrayValue::HistogramBucket b(0, 2, 6);
  inspect::UintArrayValue::HistogramBucket c(1, 2, 6);
  inspect::UintArrayValue::HistogramBucket d(0, 3, 6);
  inspect::UintArrayValue::HistogramBucket e(0, 2, 7);

  EXPECT_TRUE(a == b);
  EXPECT_TRUE(a != c);
  EXPECT_TRUE(b != c);
  EXPECT_TRUE(a != d);
  EXPECT_TRUE(a != e);
}

TEST(Reader, InvalidNameParsing) {
  std::vector<uint8_t> buf;
  buf.resize(4096);

  Block* header = reinterpret_cast<Block*>(buf.data());
  header->header = HeaderBlockFields::Order::Make(0) |
                   HeaderBlockFields::Type::Make(BlockType::kHeader) |
                   HeaderBlockFields::Version::Make(0);
  memcpy(&header->header_data[4], kMagicNumber, 4);
  header->payload.u64 = 0;

  // Manually create a value with an invalid name field.
  Block* value = reinterpret_cast<Block*>(buf.data() + kMinOrderSize);
  value->header = ValueBlockFields::Order::Make(0) |
                  ValueBlockFields::Type::Make(BlockType::kNodeValue) |
                  ValueBlockFields::NameIndex::Make(2000);

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
}

TEST(Reader, LargeExtentsWithCycle) {
  std::vector<uint8_t> buf;
  buf.resize(4096);

  Block* header = reinterpret_cast<Block*>(buf.data());
  header->header = HeaderBlockFields::Order::Make(0) |
                   HeaderBlockFields::Type::Make(BlockType::kHeader) |
                   HeaderBlockFields::Version::Make(0);
  memcpy(&header->header_data[4], kMagicNumber, 4);
  header->payload.u64 = 0;

  // Manually create a property.
  Block* value = reinterpret_cast<Block*>(buf.data() + kMinOrderSize);
  value->header = ValueBlockFields::Order::Make(0) |
                  ValueBlockFields::Type::Make(BlockType::kBufferValue) |
                  ValueBlockFields::NameIndex::Make(2);
  value->payload.u64 = PropertyBlockPayload::TotalLength::Make(0xFFFFFFFF) |
                       PropertyBlockPayload::ExtentIndex::Make(3);

  Block* name = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 2);
  name->header = NameBlockFields::Order::Make(0) | NameBlockFields::Type::Make(BlockType::kName) |
                 NameBlockFields::Length::Make(1);
  memcpy(name->payload.data, "a", 2);

  Block* extent = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 3);
  extent->header = ExtentBlockFields::Order::Make(0) |
                   ExtentBlockFields::Type::Make(BlockType::kExtent) |
                   ExtentBlockFields::NextExtentIndex::Make(3);

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
  EXPECT_EQ(1u, result.value().node().properties().size());
}

TEST(Reader, NameDoesNotFit) {
  std::vector<uint8_t> buf;
  buf.resize(4096);

  Block* header = reinterpret_cast<Block*>(buf.data());
  header->header = HeaderBlockFields::Order::Make(0) |
                   HeaderBlockFields::Type::Make(BlockType::kHeader) |
                   HeaderBlockFields::Version::Make(0);
  memcpy(&header->header_data[4], kMagicNumber, 4);
  header->payload.u64 = 0;

  // Manually create a node.
  Block* value = reinterpret_cast<Block*>(buf.data() + kMinOrderSize);
  value->header = ValueBlockFields::Order::Make(0) |
                  ValueBlockFields::Type::Make(BlockType::kNodeValue) |
                  ValueBlockFields::NameIndex::Make(2);

  Block* name = reinterpret_cast<Block*>(buf.data() + kMinOrderSize * 2);
  name->header = NameBlockFields::Order::Make(0) | NameBlockFields::Type::Make(BlockType::kName) |
                 NameBlockFields::Length::Make(10);
  memcpy(name->payload.data, "a", 2);

  auto result = inspect::ReadFromBuffer(std::move(buf));
  EXPECT_TRUE(result.is_ok());
  EXPECT_EQ(0u, result.value().children().size());
}

fpromise::result<Hierarchy> ReadHierarchyFromInspector(const Inspector& inspector) {
  fpromise::result<Hierarchy> result;
  fpromise::single_threaded_executor exec;
  exec.schedule_task(inspect::ReadFromInspector(inspector).then(
      [&](fpromise::result<Hierarchy>& res) { result = std::move(res); }));
  exec.run();

  return result;
}

TEST(Reader, MissingNamedChild) {
  Inspector inspector;
  auto state = GetState(&inspector);

  auto link =
      state->CreateLink("link", 0, "link-0", inspect::internal::LinkBlockDisposition::kChild);

  auto result = ReadHierarchyFromInspector(inspector);

  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();
  ASSERT_EQ(1, hierarchy.missing_values().size());
  EXPECT_EQ(MissingValueReason::kLinkNotFound, hierarchy.missing_values()[0].reason);
  EXPECT_EQ("link", hierarchy.missing_values()[0].name);
}

TEST(Reader, LinkedChildren) {
  Inspector inspector;
  auto state = GetState(&inspector);

  auto link0 = state->CreateLazyNode("link", 0, []() {
    inspect::Inspector inspect;
    inspect.GetRoot().CreateInt("val", 1, &inspect);
    return fpromise::make_ok_promise(inspect);
  });

  auto link1 = state->CreateLazyNode("other", 0, []() {
    inspect::Inspector inspect;
    inspect.GetRoot().CreateInt("val", 2, &inspect);
    return fpromise::make_ok_promise(inspect);
  });

  auto result = ReadHierarchyFromInspector(inspector);

  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();
  ASSERT_EQ(2, hierarchy.children().size());
  bool found_link = false, found_other = false;
  for (const auto& c : hierarchy.children()) {
    if (c.node().name() == "link") {
      ASSERT_EQ(1, c.node().properties().size());
      found_link = true;
      EXPECT_EQ("val", c.node().properties()[0].name());
      EXPECT_EQ(1, c.node().properties()[0].Get<inspect::IntPropertyValue>().value());
    } else if (c.node().name() == "other") {
      ASSERT_EQ(1, c.node().properties().size());
      found_other = true;
      EXPECT_EQ("val", c.node().properties()[0].name());
      EXPECT_EQ(2, c.node().properties()[0].Get<inspect::IntPropertyValue>().value());
    }
  }

  EXPECT_TRUE(found_link);
  EXPECT_TRUE(found_other);
}

TEST(Reader, LinkedInline) {
  Inspector inspector;
  auto state = GetState(&inspector);

  auto link = state->CreateLazyValues("link", 0, []() {
    inspect::Inspector inspector;
    inspector.GetRoot().CreateChild("child", &inspector);
    inspector.GetRoot().CreateInt("a", 10, &inspector);
    return fpromise::make_ok_promise(inspector);
  });

  auto result = ReadHierarchyFromInspector(inspector);
  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();

  ASSERT_EQ(1, hierarchy.children().size());
  EXPECT_EQ("child", hierarchy.children()[0].node().name());
  ASSERT_EQ(1, hierarchy.node().properties().size());
  EXPECT_EQ("a", hierarchy.node().properties()[0].name());
  EXPECT_EQ(10, hierarchy.node().properties()[0].Get<inspect::IntPropertyValue>().value());
}

TEST(Reader, LinkedInlineChain) {
  Inspector inspector;
  auto state = GetState(&inspector);

  auto link = state->CreateLazyValues("link", 0, []() {
    inspect::Inspector inspector;
    inspector.GetRoot().CreateInt("a", 10, &inspector);
    inspector.GetRoot().CreateLazyValues(
        "link",
        []() {
          inspect::Inspector inspector;
          inspector.GetRoot().CreateInt("b", 11, &inspector);
          inspector.GetRoot().CreateLazyValues(
              "link",
              []() {
                inspect::Inspector inspector;
                inspector.GetRoot().CreateInt("c", 12, &inspector);
                return fpromise::make_ok_promise(inspector);
              },
              &inspector);
          return fpromise::make_ok_promise(inspector);
        },
        &inspector);
    return fpromise::make_ok_promise(inspector);
  });

  auto result = ReadHierarchyFromInspector(inspector);
  ASSERT_TRUE(result.is_ok());
  auto hierarchy = result.take_value();
  hierarchy.Sort();

  ASSERT_EQ(0, hierarchy.children().size());
  ASSERT_EQ(3, hierarchy.node().properties().size());
  EXPECT_EQ("a", hierarchy.node().properties()[0].name());
  EXPECT_EQ("b", hierarchy.node().properties()[1].name());
  EXPECT_EQ("c", hierarchy.node().properties()[2].name());
  EXPECT_EQ(10, hierarchy.node().properties()[0].Get<inspect::IntPropertyValue>().value());
  EXPECT_EQ(11, hierarchy.node().properties()[1].Get<inspect::IntPropertyValue>().value());
  EXPECT_EQ(12, hierarchy.node().properties()[2].Get<inspect::IntPropertyValue>().value());
}

}  // namespace
