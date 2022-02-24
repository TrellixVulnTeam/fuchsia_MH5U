// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/diagnostics.h>
#include <fidl/flat_ast.h>

#include <zxtest/zxtest.h>

#include "error_test.h"
#include "test_library.h"

namespace {

using fidl::SourceSpan;
using fidl::flat::Constant;
using fidl::flat::HandleRights;
using fidl::flat::HandleType;
using fidl::flat::Name;
using fidl::flat::NumericConstantValue;
using fidl::types::HandleSubtype;
using fidl::types::Nullability;
using fidl::types::RightsWrappedType;

TEST(FlatAstTests, GoodImplicitAssumptions) {
  // Preconditions to unit test cases: if these change, we need to rewrite the tests themselves.
  EXPECT_TRUE(HandleSubtype::kChannel < HandleSubtype::kEvent);
  EXPECT_TRUE(Nullability::kNullable < Nullability::kNonnullable);
}

TEST(FlatAstTests, GoodCompareHandles) {
  auto name_not_important = Name::CreateIntrinsic("ignore");
  auto rights1Constant = std::make_unique<Constant>(Constant::Kind::kLiteral, SourceSpan());
  rights1Constant->ResolveTo(std::make_unique<HandleRights>(1), nullptr);
  auto rights1Value = static_cast<const HandleRights*>(&rights1Constant->Value());
  auto rights2Constant = std::make_unique<Constant>(Constant::Kind::kLiteral, SourceSpan());
  rights2Constant->ResolveTo(std::make_unique<HandleRights>(2), nullptr);
  auto rights2Value = static_cast<const HandleRights*>(&rights2Constant->Value());
  fidl::flat::Resource* resource_decl_not_needed = nullptr;
  uint32_t channel_obj_type = 4;
  uint32_t event_obj_type = 5;
  HandleType nonnullable_channel_rights1(name_not_important, resource_decl_not_needed,
                                         channel_obj_type, rights1Value, Nullability::kNonnullable);
  HandleType nullable_channel_rights1(name_not_important, resource_decl_not_needed,
                                      channel_obj_type, rights1Value, Nullability::kNullable);
  HandleType nonnullable_event_rights1(name_not_important, resource_decl_not_needed, event_obj_type,
                                       rights1Value, Nullability::kNonnullable);
  HandleType nullable_event_rights1(name_not_important, resource_decl_not_needed, event_obj_type,
                                    rights1Value, Nullability::kNullable);
  HandleType nullable_event_rights2(name_not_important, resource_decl_not_needed, event_obj_type,
                                    rights2Value, Nullability::kNullable);

  // Comparison is nullability, then type.
  EXPECT_TRUE(nullable_channel_rights1 < nonnullable_channel_rights1);
  EXPECT_TRUE(nullable_event_rights1 < nonnullable_event_rights1);
  EXPECT_TRUE(nonnullable_channel_rights1 < nonnullable_event_rights1);
  EXPECT_TRUE(nullable_channel_rights1 < nullable_event_rights1);
  EXPECT_TRUE(nullable_event_rights1 < nullable_event_rights2);
}

TEST(FlatAstTests, BadCannotReferenceAnonymousName) {
  TestLibrary library(R"FIDL(
library example;

protocol Foo {
  SomeMethod(struct { some_param uint8; });
};

type Bar = struct {
  bad_member_type FooSomeMethodRequest;
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrAnonymousNameReference);
}

TEST(FlatAstTests, BadAnonymousNameConflict) {
  TestLibrary library(R"FIDL(
library example;

protocol Foo {
  SomeMethod(struct { some_param uint8; });
};

type FooSomeMethodRequest = struct {};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrNameCollision);
}

TEST(FlatAstTests, GoodSingleAnonymousNameUse) {
  TestLibrary library(R"FIDL(library example;

protocol Foo {
    SomeMethod() -> (struct {
        some_param uint8;
    }) error uint32;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(FlatAstTests, BadMultipleLibrariesSameName) {
  SharedAmongstLibraries shared;
  TestLibrary library1("one.fidl", "library example;", &shared);
  ASSERT_COMPILED(library1);
  TestLibrary library2("two.fidl", "library example;", &shared);
  ASSERT_ERRORED_DURING_COMPILE(library2, fidl::ErrMultipleLibrariesWithSameName);
}

}  // namespace
