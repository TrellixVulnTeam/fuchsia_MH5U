// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/modified_type.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/base_type.h"

namespace zxdb {

namespace {

fxl::RefPtr<BaseType> MakeBaseType(const char* name, int base_type, uint32_t byte_size) {
  fxl::RefPtr<BaseType> result = fxl::MakeRefCounted<BaseType>();
  result->set_base_type(base_type);
  result->set_byte_size(byte_size);
  result->set_assigned_name(name);
  return result;
}

}  // namespace

TEST(ModifiedType, Strip) {
  constexpr uint32_t kIntSize = 4u;
  auto int_type = MakeBaseType("int", BaseType::kBaseTypeSigned, kIntSize);

  // Construct an insane modified type.
  auto volatile_int = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kVolatileType, int_type);
  auto atomic_volatile_int = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kAtomicType, volatile_int);
  auto const_atomic_volatile_int =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, atomic_volatile_int);

  // This puts the "const" at the right which is a little weird (following the pointer rule) but
  // this is still OK.
  EXPECT_EQ("_Atomic volatile int const", const_atomic_volatile_int->GetFullName());

  // Stripping (both types) should remove all qualifiers we just added.
  auto stripped_cv = const_atomic_volatile_int->StripCV();
  EXPECT_EQ(stripped_cv, int_type.get());
  auto stripped_cvt = const_atomic_volatile_int->StripCVT();
  EXPECT_EQ(stripped_cvt, int_type.get());

  // Construct a typedef of the insane type.
  auto insane = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kTypedef, const_atomic_volatile_int);
  insane->set_assigned_name("Insane");
  EXPECT_EQ("Insane", insane->GetFullName());

  auto const_insane = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, insane);

  // StripCV() should only strip the const, not the typedef.
  stripped_cv = const_insane->StripCV();
  EXPECT_EQ(stripped_cv, insane.get());

  // StripCVT() should get rid of everything.
  stripped_cvt = const_insane->StripCVT();
  EXPECT_EQ(stripped_cvt, int_type.get());
}

TEST(ModifiedType, GetFullName) {
  // int
  constexpr uint32_t kIntSize = 4u;
  auto int_type = MakeBaseType("int", BaseType::kBaseTypeSigned, kIntSize);
  EXPECT_EQ("int", int_type->GetFullName());
  EXPECT_EQ(kIntSize, int_type->byte_size());

  // int*
  constexpr uint32_t kPtrSize = 8u;
  auto int_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, int_type);
  EXPECT_EQ("int*", int_ptr->GetFullName());
  EXPECT_EQ(kPtrSize, int_ptr->byte_size());

  // const int
  auto const_int = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, int_type);
  EXPECT_EQ("const int", const_int->GetFullName());
  EXPECT_EQ(kIntSize, const_int->byte_size());

  // const int*
  auto const_int_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, const_int);
  EXPECT_EQ("const int*", const_int_ptr->GetFullName());
  EXPECT_EQ(kPtrSize, const_int_ptr->byte_size());

  // const int* const
  auto const_int_const_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, const_int_ptr);
  EXPECT_EQ("const int* const", const_int_const_ptr->GetFullName());
  EXPECT_EQ(kPtrSize, const_int_const_ptr->byte_size());

  // const int* restrict
  auto const_int_ptr_restrict =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kRestrictType, const_int_ptr);
  EXPECT_EQ("const int* restrict", const_int_ptr_restrict->GetFullName());
  EXPECT_EQ(kPtrSize, const_int_ptr_restrict->byte_size());

  // const int* const&
  auto const_int_const_ptr_ref =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kReferenceType, const_int_const_ptr);
  EXPECT_EQ("const int* const&", const_int_const_ptr_ref->GetFullName());
  EXPECT_EQ(kPtrSize, const_int_const_ptr_ref->byte_size());

  // volatile
  auto volatile_int = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kVolatileType, int_type);
  EXPECT_EQ("volatile int", volatile_int->GetFullName());
  EXPECT_EQ(kIntSize, volatile_int->byte_size());

  // volatile int&&
  auto volatile_int_rvalue_ref =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kRvalueReferenceType, volatile_int);
  EXPECT_EQ("volatile int&&", volatile_int_rvalue_ref->GetFullName());
  EXPECT_EQ(kPtrSize, volatile_int_rvalue_ref->byte_size());

  // typedef const int* Foo
  auto typedef_etc = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kTypedef, const_int_ptr);
  typedef_etc->set_assigned_name("Foo");
  EXPECT_EQ("Foo", typedef_etc->GetFullName());
  EXPECT_EQ(kPtrSize, typedef_etc->byte_size());

  // typedef void VoidType;
  auto typedef_void = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kTypedef, LazySymbol());
  typedef_void->set_assigned_name("VoidType");
  EXPECT_EQ("VoidType", typedef_void->GetFullName());

  // void* (There are two ways to encode: pointer to nothing, and pointer to "none" base type).
  auto void_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, LazySymbol());
  EXPECT_EQ("void*", void_ptr->GetFullName());
  auto void_ptr2 =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, fxl::MakeRefCounted<BaseType>());
  EXPECT_EQ("void*", void_ptr2->GetFullName());

  // const void (same two ways to encode as void*).
  auto const_void = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, LazySymbol());
  EXPECT_EQ("const void", const_void->GetFullName());
  auto const_void2 =
      fxl::MakeRefCounted<ModifiedType>(DwarfTag::kConstType, fxl::MakeRefCounted<BaseType>());
  EXPECT_EQ("const void", const_void2->GetFullName());

  // const void* (same two ways to encode as void*).
  auto const_void_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, const_void);
  EXPECT_EQ("const void*", const_void_ptr->GetFullName());
  auto const_void_ptr2 = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, const_void2);
  EXPECT_EQ("const void*", const_void_ptr2->GetFullName());

  // Atomic int.
  auto atomic_int_type = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kAtomicType, int_type);
  EXPECT_EQ("_Atomic int", atomic_int_type->GetFullName());
  EXPECT_EQ(kIntSize, atomic_int_type->byte_size());
}

}  // namespace zxdb
