// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/flat_ast.h>
#include <fidl/lexer.h>
#include <fidl/parser.h>
#include <fidl/source_file.h>

#include <zxtest/zxtest.h>

#include "error_test.h"
#include "test_library.h"

namespace {

TEST(StrictnessTests, BadDuplicateModifier) {
  TestLibrary library(R"FIDL(
library example;

type One = strict union { 1: b bool; };
type Two = strict strict union { 1: b bool; };          // line 5
type Three = strict strict strict union { 1: b bool; }; // line 6
  )FIDL");
  ASSERT_FALSE(library.Compile());

  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 3);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[0]->span.position().line, 5);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "strict");
  ASSERT_ERR(errors[1], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[1]->span.position().line, 6);
  ASSERT_SUBSTR(errors[1]->msg.c_str(), "strict");
  ASSERT_ERR(errors[2], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[2]->span.position().line, 6);
  ASSERT_SUBSTR(errors[2]->msg.c_str(), "strict");
}

TEST(StrictnessTests, BadConflictingModifiers) {
  TestLibrary library(R"FIDL(
library example;

type SF = strict flexible union { 1: b bool; }; // line 4
type FS = flexible strict union { 1: b bool; }; // line 5
  )FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrConflictingModifier,
                                      fidl::ErrConflictingModifier);
  EXPECT_EQ(library.errors()[0]->span.position().line, 4);
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "strict");
  ASSERT_SUBSTR(library.errors()[0]->msg.c_str(), "flexible");
  EXPECT_EQ(library.errors()[1]->span.position().line, 5);
  ASSERT_SUBSTR(library.errors()[1]->msg.c_str(), "strict");
  ASSERT_SUBSTR(library.errors()[1]->msg.c_str(), "flexible");
}

TEST(StrictnessTests, GoodBitsStrictness) {
  TestLibrary library(
      R"FIDL(library example;

type DefaultStrictFoo = strict bits {
    BAR = 0x1;
};

type StrictFoo = strict bits {
    BAR = 0x1;
};

type FlexibleFoo = flexible bits {
    BAR = 0x1;
};
)FIDL");
  ASSERT_COMPILED(library);
  EXPECT_EQ(library.LookupBits("FlexibleFoo")->strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(library.LookupBits("StrictFoo")->strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(library.LookupBits("DefaultStrictFoo")->strictness, fidl::types::Strictness::kStrict);
}

TEST(StrictnessTests, GoodEnumStrictness) {
  TestLibrary library(
      R"FIDL(library example;

type DefaultStrictFoo = strict enum {
    BAR = 1;
};

type StrictFoo = strict enum {
    BAR = 1;
};

type FlexibleFoo = flexible enum {
    BAR = 1;
};
)FIDL");
  ASSERT_COMPILED(library);
  EXPECT_EQ(library.LookupEnum("FlexibleFoo")->strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(library.LookupEnum("StrictFoo")->strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(library.LookupEnum("DefaultStrictFoo")->strictness, fidl::types::Strictness::kStrict);
}

TEST(StrictnessTests, GoodFlexibleEnum) {
  TestLibrary library(R"FIDL(library example;

type Foo = flexible enum {
    BAR = 1;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StrictnessTests, GoodFlexibleBitsRedundant) {
  TestLibrary library(R"FIDL(library example;

type Foo = flexible bits {
    BAR = 0x1;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StrictnessTests, BadStrictnessStruct) {
  TestLibrary library(R"FIDL(
library example;

type Foo = strict struct {
    i int32;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrCannotSpecifyModifier);
}

TEST(StrictnessTests, BadStrictnessTable) {
  TestLibrary library("table", R"FIDL(
library example;

type StrictFoo = strict table {};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrCannotSpecifyModifier);
}

TEST(StrictnessTests, GoodUnionStrictness) {
  TestLibrary library(R"FIDL(library example;

type Foo = strict union {
    1: i int32;
};

type FlexibleFoo = flexible union {
    1: i int32;
};

type StrictFoo = strict union {
    1: i int32;
};
)FIDL");
  ASSERT_COMPILED(library);
  EXPECT_EQ(library.LookupUnion("Foo")->strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(library.LookupUnion("FlexibleFoo")->strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(library.LookupUnion("StrictFoo")->strictness, fidl::types::Strictness::kStrict);
}

TEST(StrictnessTests, GoodStrictUnionRedundant) {
  TestLibrary library(R"FIDL(library example;

type Foo = strict union {
    1: i int32;
};
)FIDL");
  ASSERT_COMPILED(library);
  ASSERT_EQ(library.LookupUnion("Foo")->strictness, fidl::types::Strictness::kStrict);
}

}  // namespace
