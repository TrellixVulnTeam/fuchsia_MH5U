// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/expr.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/expr/mock_eval_context.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/type_test_support.h"

namespace zxdb {

TEST(Expr, ValueToAddressAndSize) {
  auto eval_context = fxl::MakeRefCounted<MockEvalContext>();

  // Ints are OK but have no size.
  uint64_t address = 0;
  std::optional<uint32_t> size;
  ASSERT_TRUE(ValueToAddressAndSize(eval_context, ExprValue(23), &address, &size).ok());
  EXPECT_EQ(23u, address);
  EXPECT_EQ(std::nullopt, size);

  // Structure.
  auto uint64_type = MakeUint64Type();
  auto collection =
      MakeCollectionType(DwarfTag::kStructureType, "Foo", {{"a", uint64_type}, {"b", uint64_type}});
  std::vector<uint8_t> collection_data;
  collection_data.resize(collection->byte_size());

  // Currently evaluating a structure is expected to fail.
  // TODO(bug 44074) support non-pointer values and take their address implicitly.
  address = 0;
  size = std::nullopt;
  Err err = ValueToAddressAndSize(
      eval_context, ExprValue(collection, collection_data, ExprValueSource(0x12345678)), &address,
      &size);
  ASSERT_TRUE(err.has_error());
  EXPECT_EQ("Can't convert 'Foo' to an address.", err.msg());
  EXPECT_EQ(0u, address);
  EXPECT_EQ(std::nullopt, size);

  // Pointer to a collection.
  auto collection_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, collection);
  std::vector<uint8_t> ptr_data{8, 7, 6, 5, 4, 3, 2, 1};

  address = 0;
  size = std::nullopt;
  err = ValueToAddressAndSize(eval_context, ExprValue(collection_ptr, ptr_data), &address, &size);
  ASSERT_TRUE(err.ok());
  EXPECT_EQ(0x0102030405060708u, address);
  ASSERT_TRUE(size);
  EXPECT_EQ(collection->byte_size(), *size);
}

}  // namespace zxdb
