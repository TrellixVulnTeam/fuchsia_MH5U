// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/resolve_array.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/common/test_with_loop.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/developer/debug/zxdb/expr/mock_eval_context.h"
#include "src/developer/debug/zxdb/expr/pretty_type.h"
#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/type_test_support.h"

namespace zxdb {

namespace {

class ResolveArrayTest : public TestWithLoop {};

// A PrettyType implementation that provides arracy access. This array access returns the index * 2
// as the array value.
class TestPrettyArray : public PrettyType {
 public:
  TestPrettyArray(){}

  void Format(FormatNode* node, const FormatOptions& options,
              const fxl::RefPtr<EvalContext>& context, fit::deferred_callback cb) override {}
  EvalArrayFunction GetArrayAccess() const override {
    return [](const fxl::RefPtr<EvalContext>&, const ExprValue& object_value, int64_t index,
              EvalCallback cb) { cb(ExprValue(index * 2)); };
  }
};

}  // namespace

// Tests the static resolution case when the source is a vector register. The "source" of array
// elements in this case is tricky.
TEST_F(ResolveArrayTest, ResolveVectorRegister) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  // Array holds 4 uint32_t.
  constexpr uint32_t kTypeSize = 4;
  auto elt_type = fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeUnsigned, kTypeSize, "uint32_t");
  auto array_type = fxl::MakeRefCounted<ArrayType>(elt_type, 4);

  std::vector<uint8_t> array_bytes = {0, 0, 0, 0,   // array[0] = 0
                                      1, 0, 0, 0,   // array[1] = 1
                                      2, 0, 0, 0,   // array[2] = 2
                                      3, 0, 0, 0};  // array[3] = 3
  constexpr debug::RegisterID register_id = debug::RegisterID::kX64_xmm3;
  ExprValue value(array_type, array_bytes, ExprValueSource(register_id));

  // Ask for all 4 values.
  bool called = false;
  ErrOrValueVector result((std::vector<ExprValue>()));
  ResolveArray(eval_context, value, 0, 4, [&called, &result](ErrOrValueVector cb_result) {
    called = true;
    result = std::move(cb_result);
    debug::MessageLoop::Current()->QuitNow();
  });
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);

  EXPECT_FALSE(result.has_error());
  ASSERT_EQ(4u, result.value().size());

  // Each element should be 32 bits wide and shifted 32 bits more than the previous.
  EXPECT_EQ(0u, result.value()[0].GetAs<uint32_t>());
  EXPECT_EQ(ExprValueSource(register_id, 32, 0), result.value()[0].source());

  EXPECT_EQ(1u, result.value()[1].GetAs<uint32_t>());
  EXPECT_EQ(ExprValueSource(register_id, 32, 32), result.value()[1].source());

  EXPECT_EQ(2u, result.value()[2].GetAs<uint32_t>());
  EXPECT_EQ(ExprValueSource(register_id, 32, 64), result.value()[2].source());

  EXPECT_EQ(3u, result.value()[3].GetAs<uint32_t>());
  EXPECT_EQ(ExprValueSource(register_id, 32, 96), result.value()[3].source());
}

// Resolves an array element with a pointer as the base.
TEST_F(ResolveArrayTest, ResolvePointer) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  // Request 3 elements from 1-4.
  constexpr uint64_t kBaseAddress = 0x100000;
  constexpr uint32_t kBeginIndex = 1;
  constexpr uint32_t kEndIndex = 4;

  // Array holds 3 uint16_t.
  constexpr uint32_t kTypeSize = 2;
  auto elt_type = fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeUnsigned, kTypeSize, "uint16_t");
  auto ptr_type = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, elt_type);

  // Create memory with two values 0x3344, 0x5566. Note that these are offset one value from the
  // beginning of the array so the requested address of the kBeginIndex'th element matches this
  // address.
  constexpr uint64_t kBeginAddress = kBaseAddress + kBeginIndex * kTypeSize;
  eval_context->data_provider()->AddMemory(kBeginAddress, {0x44, 0x33, 0x66, 0x55});

  // Data in the value is the pointer to the beginning of the array.
  ExprValue value(ptr_type, {0, 0, 0x10, 0, 0, 0, 0, 0});

  bool called = false;
  ErrOrValueVector result((std::vector<ExprValue>()));
  ResolveArray(eval_context, value, kBeginIndex, kEndIndex,
               [&called, &result](ErrOrValueVector cb_result) {
                 called = true;
                 result = std::move(cb_result);
                 debug::MessageLoop::Current()->QuitNow();
               });

  // Should be called async.
  EXPECT_FALSE(called);
  loop().Run();
  EXPECT_TRUE(called);

  // Should have returned two values (the overlap of the array and the requested range).
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(2u, result.value().size());

  EXPECT_EQ(elt_type.get(), result.value()[0].type());
  EXPECT_EQ(0x3344, result.value()[0].GetAs<uint16_t>());
  EXPECT_EQ(kBaseAddress + kTypeSize, result.value()[0].source().address());

  EXPECT_EQ(elt_type.get(), result.value()[1].type());
  EXPECT_EQ(0x5566, result.value()[1].GetAs<uint16_t>());
  EXPECT_EQ(kBaseAddress + kTypeSize * 2, result.value()[1].source().address());

  // Test the one-element variant.
  called = false;
  ErrOrValue single_result((ExprValue()));
  ResolveArrayItem(eval_context, value, kBeginIndex, [&called, &single_result](ErrOrValue result) {
    called = true;
    single_result = std::move(result);
    debug::MessageLoop::Current()->QuitNow();
  });

  // Should be called async.
  EXPECT_FALSE(called);
  loop().Run();
  EXPECT_TRUE(called);

  ASSERT_TRUE(single_result.ok());
  EXPECT_EQ(elt_type.get(), single_result.value().type());
  EXPECT_EQ(0x3344, single_result.value().GetAs<uint16_t>());
  EXPECT_EQ(kBaseAddress + kTypeSize, single_result.value().source().address());
}

TEST_F(ResolveArrayTest, Invalid) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  // Resolving an array on an empty ExprValue.
  bool called = false;
  ResolveArrayItem(eval_context, ExprValue(), 1, [&called](ErrOrValue result) {
    called = true;
    EXPECT_TRUE(result.has_error());
    EXPECT_EQ("Can't resolve an array access on type '<Unknown>'.", result.err().msg());
  });
  EXPECT_TRUE(called);

  // Resolving an array on an integer type.
  called = false;
  ResolveArrayItem(eval_context, ExprValue(56), 1, [&called](ErrOrValue result) {
    called = true;
    EXPECT_TRUE(result.has_error());
    EXPECT_EQ("Can't resolve an array access on type 'int32_t'.", result.err().msg());
  });
  EXPECT_TRUE(called);
}

// Tests a PrettyType's implementation of [].
TEST_F(ResolveArrayTest, PrettyArray) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  const char kMyTypeName[] = "MyType";

  // Set up pretty array mock for "MyType".
  IdentifierGlob mytype_glob;
  ASSERT_FALSE(mytype_glob.Init(kMyTypeName).has_error());
  eval_context->pretty_type_manager().Add(ExprLanguage::kC, mytype_glob,
                                          std::make_unique<TestPrettyArray>());

  auto my_type = MakeCollectionType(DwarfTag::kStructureType, kMyTypeName, {});
  ExprValue my_value(my_type, {});

  constexpr uint64_t kIndex = 55;

  // Test the one-element variant.
  bool called = false;
  ErrOrValue result((ExprValue()));
  ResolveArrayItem(eval_context, my_value, kIndex, [&called, &result](ErrOrValue value) {
    called = true;
    result = std::move(value);
    debug::MessageLoop::Current()->QuitNow();
  });

  // The PrettyType executes synchronously so it should complete synchronouly.
  EXPECT_TRUE(called);
  ASSERT_TRUE(result.ok());

  // Result should be twice the input.
  EXPECT_EQ(kIndex * 2, result.value().GetAs<uint64_t>());
}

TEST_F(ResolveArrayTest, CoercePointerToArrayOfSize) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  constexpr uint64_t kBaseAddress = 0x100000;

  // Array holds 3 uint16_t.
  constexpr uint32_t kTypeSize = 2;
  auto elt_type = fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeUnsigned, kTypeSize, "uint16_t");
  auto ptr_type = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, elt_type);

  // The full array in memory including 3 items. The values are 0x1122, 0x3344, 0x5566
  std::vector<uint8_t> array_bytes = {0x22, 0x11, 0x44, 0x33, 0x66, 0x55};
  eval_context->data_provider()->AddMemory(kBaseAddress, array_bytes);

  ExprValue ptr_value(kBaseAddress, ptr_type);

  // This callback is used for all examples below.
  bool called = false;
  ErrOrValue result((ExprValue()));
  auto callback = [&called, &result](ErrOrValue value) {
    called = true;
    result = std::move(value);
    debug::MessageLoop::Current()->QuitNow();
  };

  // Request an array of size 2. It should fetch the memory.
  CoerceArraySize(eval_context, ptr_value, 2, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(kTypeSize * 2, result.value().data().size());
  std::vector<uint8_t> two_value_expected(array_bytes.begin(), array_bytes.begin() + kTypeSize * 2);
  EXPECT_EQ(two_value_expected, result.value().data().bytes());
  EXPECT_EQ("uint16_t[2]", result.value().type()->GetFullName());
  EXPECT_EQ(kBaseAddress, result.value().source().address());

  // Size 0 should also work.
  called = false;
  CoerceArraySize(eval_context, ptr_value, 0, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value().data().empty());
  EXPECT_EQ("uint16_t[0]", result.value().type()->GetFullName());
  EXPECT_EQ(kBaseAddress, result.value().source().address());
}

TEST_F(ResolveArrayTest, CoerceStaticArraySize) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  constexpr uint64_t kBaseAddress = 0x100000;
  constexpr uint32_t kOriginalArraySize = 2;

  // Array holds 3 uint16_t.
  constexpr uint32_t kTypeSize = 2;
  auto elt_type = fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeUnsigned, kTypeSize, "uint16_t");
  auto array_type = fxl::MakeRefCounted<ArrayType>(elt_type, kOriginalArraySize);

  // The full array in memory including 3 items. The values are 0x1122, 0x3344, 0x5566
  std::vector<uint8_t> array_bytes = {0x22, 0x11, 0x44, 0x33, 0x66, 0x55};
  eval_context->data_provider()->AddMemory(kBaseAddress, array_bytes);
  ExprValue value(array_type, array_bytes, ExprValueSource(kBaseAddress));

  // Source array with 2 elts and a temporary source (not referencing the memory added).
  std::vector<uint8_t> two_elt_bytes(array_bytes.begin(),
                                     array_bytes.begin() + (kOriginalArraySize * kTypeSize));
  ExprValue temp_value(array_type, two_elt_bytes);

  // This callback is used for all examples below.
  bool called = false;
  ErrOrValue result((ExprValue()));
  auto callback = [&called, &result](ErrOrValue value) {
    called = true;
    result = std::move(value);
    debug::MessageLoop::Current()->QuitNow();
  };

  // Shrinking the temporary value should succeed.
  CoerceArraySize(eval_context, temp_value, 1, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(2u, result.value().data().size());
  EXPECT_EQ(0x1122, result.value().GetAs<uint16_t>());
  EXPECT_EQ("uint16_t[1]", result.value().type()->GetFullName());

  // Expanding to the same size should work.
  called = false;
  CoerceArraySize(eval_context, temp_value, kOriginalArraySize, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(temp_value, result.value());
  EXPECT_EQ("uint16_t[2]", result.value().type()->GetFullName());

  // Expanding the temp value should fail (it references no memory).
  called = false;
  CoerceArraySize(eval_context, temp_value, kOriginalArraySize + 1, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ("Can not expand array that is not in memory.", result.err().msg());

  // Make an identical value that references the source memory.
  ExprValue mem_value(array_type, two_elt_bytes, ExprValueSource(kBaseAddress));

  // Expanding to the same size should work.
  called = false;
  CoerceArraySize(eval_context, mem_value, kOriginalArraySize + 1, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(array_bytes, result.value().data().bytes());
  EXPECT_EQ("uint16_t[3]", result.value().type()->GetFullName());
  EXPECT_EQ(kBaseAddress, result.value().source().address());

  // Expanding beyond the valid memory should fail.
  called = false;
  CoerceArraySize(eval_context, mem_value, kOriginalArraySize + 2, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ("Array memory not valid.", result.err().msg());

  // Shrinking to empty should also work and it should preserve the source.
  called = false;
  CoerceArraySize(eval_context, mem_value, 0, callback);
  loop().RunUntilNoTasks();
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value().data().empty());
  EXPECT_EQ("uint16_t[0]", result.value().type()->GetFullName());
  EXPECT_EQ(kBaseAddress, result.value().source().address());
}

}  // namespace zxdb
