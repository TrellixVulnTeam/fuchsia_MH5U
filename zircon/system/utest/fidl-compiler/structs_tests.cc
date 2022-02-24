// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "error_test.h"
#include "fidl/diagnostics.h"
#include "test_library.h"

namespace {

TEST(StructsTests, GoodPrimitiveDefaultValueLiteral) {
  TestLibrary library(R"FIDL(library example;

type MyStruct = struct {
    field int64 = 20;
};
)FIDL");
  ASSERT_COMPILED(library);
  auto type_decl = library.LookupStruct("MyStruct");
  ASSERT_NOT_NULL(type_decl);
  EXPECT_EQ(type_decl->members.size(), 1);
}

TEST(StructsTests, GoodPrimitiveDefaultValueConstReference) {
  TestLibrary library(R"FIDL(library example;

const A int32 = 20;

type MyStruct = struct {
    field int64 = A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadMissingDefaultValueReferenceTarget) {
  TestLibrary library(R"FIDL(
library example;

type MyStruct = struct {
    field int64 = A;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
}

TEST(StructsTests, GoodEnumDefaultValueEnumMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyEnum = strict enum : int32 {
    A = 5;
};

type MyStruct = struct {
    field MyEnum = MyEnum.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueEnumMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyEnum = strict enum : int32 {
    A = 5;
};

type MyStruct = struct {
    field int64 = MyEnum.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadDefaultValueEnumType) {
  TestLibrary library(R"FIDL(
library example;

type MyEnum = enum : int32 { A = 1; };
type OtherEnum = enum : int32 { A = 1; };

type MyStruct = struct {
    field MyEnum = OtherEnum.A;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrMismatchedNameTypeAssignment,
                                      fidl::ErrCouldNotResolveMemberDefault);
}

TEST(StructsTests, BadDefaultValuePrimitiveInEnum) {
  TestLibrary library(R"FIDL(
library example;

type MyEnum = enum : int32 { A = 1; };

type MyStruct = struct {
    field MyEnum = 1;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrTypeCannotBeConvertedToType,
                                      fidl::ErrCouldNotResolveMemberDefault);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "MyEnum");
}

TEST(StructsTests, GoodEnumDefaultValueBitsMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyBits = strict bits : uint32 {
    A = 0x00000001;
};

type MyStruct = struct {
    field MyBits = MyBits.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueBitsMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyBits = strict bits : uint32 {
    A = 0x00000001;
};

type MyStruct = struct {
    field int64 = MyBits.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadDefaultValueBitsType) {
  TestLibrary library(R"FIDL(
library example;

type MyBits = bits : uint32 { A = 0x00000001; };
type OtherBits = bits : uint32 { A = 0x00000001; };

type MyStruct = struct {
    field MyBits = OtherBits.A;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrMismatchedNameTypeAssignment,
                                      fidl::ErrCouldNotResolveMemberDefault);
}

TEST(StructsTests, BadDefaultValuePrimitiveInBits) {
  TestLibrary library(R"FIDL(
library example;

type MyBits = enum : int32 { A = 0x00000001; };

type MyStruct = struct {
    field MyBits = 1;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrTypeCannotBeConvertedToType,
                                      fidl::ErrCouldNotResolveMemberDefault);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "MyBits");
}

// The old-style of enum-referencing should no longer work.
TEST(StructsTests, BadLegacyEnumMemberReference) {
  TestLibrary library(R"FIDL(
library example;

type MyEnum = enum : int32 { A = 5; };

type MyStruct = struct {
    field MyEnum = A;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
}

TEST(StructsTests, BadDefaultValueNullableString) {
  TestLibrary library(R"FIDL(
library example;

type MyStruct = struct {
    field string:optional = "";
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidStructMemberType);
}

TEST(StructsTests, BadDuplicateMemberName) {
  TestLibrary library(R"FIDL(
library example;

type Duplicates = struct {
    s string;
    s uint8;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrDuplicateStructMemberName);
}

TEST(StructsTests, GoodMaxInlineSize) {
  TestLibrary library(R"FIDL(library example;

type MyStruct = struct {
    arr array<uint8, 65535>;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadInlineSizeExceeds64k) {
  TestLibrary library(R"FIDL(
library example;

type MyStruct = struct {
    arr array<uint8,65536>;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInlineSizeExceeds64k);
}

TEST(StructsTests, BadMutuallyRecursive) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  yang Yang;
};

type Yang = struct {
  yin Yin;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrIncludeCycle);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "struct Yang -> struct Yin -> struct Yang");
}

TEST(StructsTests, BadSelfRecursive) {
  TestLibrary library(R"FIDL(
library example;

type MySelf = struct {
  me MySelf;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrIncludeCycle);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "struct MySelf -> struct MySelf");
}

TEST(StructsTests, BadMutuallyRecursiveWithIncomingLeaf) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  yang Yang;
};

type Yang = struct {
  yin Yin;
};

type Leaf = struct {
  yin Yin;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrIncludeCycle);
  // Leaf sorts before either Yin or Yang, so the cycle finder in sort_step.cc
  // starts there, which leads it to yin before yang.
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "struct Yin -> struct Yang -> struct Yin");
}

TEST(StructsTests, BadMutuallyRecursiveWithOutogingLeaf) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  yang Yang;
};

type Yang = struct {
  yin Yin;
  leaf Leaf;
};

type Leaf = struct {
  x int32;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrIncludeCycle);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "struct Yang -> struct Yin -> struct Yang");
}

TEST(StructsTests, BadMutuallyRecursiveIntersectingLoops) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  intersection Intersection;
};

type Yang = struct {
  intersection Intersection;
};

type Intersection = struct {
  yin Yin;
  yang Yang;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrIncludeCycle);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(),
                "struct Intersection -> struct Yang -> struct Intersection");
}

TEST(StructsTests, BadBoxCannotBeNullable) {
  TestLibrary library(R"FIDL(
library example;

type BoxedStruct = struct {};

type Foo = struct {
  foo box<Foo>:optional;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrBoxCannotBeNullable);
}

TEST(StructsTests, BadBoxedTypeCannotBeNullable) {
  TestLibrary library(R"FIDL(
library example;

type BoxedStruct = struct {};

type Foo = struct {
  foo box<Foo:optional>;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrBoxedTypeCannotBeNullable);
}

TEST(StructsTests, BadTypeCannotBeBoxed) {
  for (const std::string& definition : {
           "type Foo = struct { union_member box<union { 1: data uint8; }>; };",
           "type Foo = struct { table_member box<table { 1: data uint8; }>; };",
           "type Foo = struct { enum_member box<enum { DATA = 1; }>; };",
           "type Foo = struct { bits_member box<bits { DATA = 1; }>; };",
           "type Foo = struct { array_member box<array<uint8, 1>>; };",
           "type Foo = struct { vector_member box<vector<uint8>>; };",
           "type Foo = struct { string_member box<string>; };",
           "type Foo = struct { prim_member box<int32>; };",
           "type Foo = struct { resource_member box<zx.handle>; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    auto library = WithLibraryZx(fidl_library);
    ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrCannotBeBoxed);
  }
}

TEST(StructsTests, BadDefaultValueReferencesInvalidConst) {
  TestLibrary library(R"FIDL(
library example;

type Foo = struct {
    flag bool = BAR;
};

const BAR bool = "not a bool";
)FIDL");
  ASSERT_FALSE(library.Compile());
  ASSERT_EQ(library.errors().size(), 3);
  EXPECT_ERR(library.errors()[0], fidl::ErrTypeCannotBeConvertedToType);
  EXPECT_ERR(library.errors()[1], fidl::ErrCannotResolveConstantValue);
  EXPECT_ERR(library.errors()[2], fidl::ErrCouldNotResolveMemberDefault);
}

}  // namespace
