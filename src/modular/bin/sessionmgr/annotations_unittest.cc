// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/annotations.h"

#include <fuchsia/modular/cpp/fidl.h>

#include <memory>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/modular/bin/sessionmgr/testing/annotations_matchers.h"

namespace modular::annotations {
namespace {

using Annotation = fuchsia::modular::Annotation;
using AnnotationValue = fuchsia::modular::AnnotationValue;

using ::testing::ByRef;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

Annotation MakeAnnotation(std::string key, std::string value) {
  AnnotationValue annotation_value;
  annotation_value.set_text(std::move(value));
  return Annotation{
      .key = std::move(key),
      .value = std::make_unique<fuchsia::modular::AnnotationValue>(std::move(annotation_value))};
}

// Merging two empty vectors of annotations produces an empty vector.
TEST(AnnotationsTest, MergeEmpty) {
  std::vector<Annotation> a{};
  std::vector<Annotation> b{};
  EXPECT_THAT(Merge(std::move(a), std::move(b)), IsEmpty());
}

// Merging an empty vectors of annotations into a non-empty one produces the latter, unchanged.
TEST(AnnotationsTest, MergeEmptyIntoNonEmpty) {
  auto annotation = MakeAnnotation("foo", "bar");

  std::vector<Annotation> a{};
  a.push_back(fidl::Clone(annotation));
  std::vector<Annotation> b{};

  EXPECT_THAT(Merge(std::move(a), std::move(b)), ElementsAre(AnnotationEq(ByRef(annotation))));
}

// Merging an annotation with the same key, with a non-null value, overwrites the value.
TEST(AnnotationsTest, MergeOverwrite) {
  auto annotation_1 = MakeAnnotation("foo", "bar");
  auto annotation_2 = MakeAnnotation("foo", "quux");

  std::vector<Annotation> a{};
  a.push_back(fidl::Clone(annotation_1));
  std::vector<Annotation> b{};
  b.push_back(fidl::Clone(annotation_2));

  EXPECT_THAT(Merge(std::move(a), std::move(b)), ElementsAre(AnnotationEq(ByRef(annotation_2))));
}

// Merging an annotation with the same key, with a null value, removes the annotation.
TEST(AnnotationsTest, MergeNullValueDeletesExisting) {
  auto annotation_1 = MakeAnnotation("foo", "bar");
  auto annotation_2 = Annotation{.key = "foo"};

  std::vector<Annotation> a{};
  a.push_back(fidl::Clone(annotation_1));
  std::vector<Annotation> b{};
  b.push_back(fidl::Clone(annotation_2));

  EXPECT_THAT(Merge(std::move(a), std::move(b)), IsEmpty());
}

// Merging disjoint sets of annotations produces a union.
TEST(AnnotationsTest, MergeDisjoint) {
  auto annotation_1 = MakeAnnotation("foo", "bar");
  auto annotation_2 = MakeAnnotation("hello", "world");

  std::vector<Annotation> a{};
  a.push_back(fidl::Clone(annotation_1));
  std::vector<Annotation> b{};
  a.push_back(fidl::Clone(annotation_2));

  EXPECT_THAT(
      Merge(std::move(a), std::move(b)),
      UnorderedElementsAre(AnnotationEq(ByRef(annotation_1)), AnnotationEq(ByRef(annotation_2))));
}

// TODO(fxbug.dev/37645): Return the proper properties instead of text strings.
TEST(AnnotationsTest, TextToInspect) {
  auto annotation_text = MakeAnnotation("string_key", "string_text");
  EXPECT_THAT(ToInspect(*annotation_text.value.get()), "string_text");
}

// TODO(fxbug.dev/37645): Return the proper properties instead of text strings.
TEST(AnnotationsTest, BufferToInspect) {
  fuchsia::mem::Buffer buffer{};
  std::string buffer_str = "x";
  ASSERT_TRUE(fsl::VmoFromString(buffer_str, &buffer));
  AnnotationValue annotation_value_buffer;
  annotation_value_buffer.set_buffer(std::move(buffer));
  auto annotation_buffer = Annotation{.key = std::move("buffer_key"),
                                      .value = std::make_unique<fuchsia::modular::AnnotationValue>(
                                          std::move(annotation_value_buffer))};

  EXPECT_THAT(ToInspect(*annotation_buffer.value.get()), "buffer");
}

// TODO(fxbug.dev/37645): Return the proper properties instead of text strings.
TEST(AnnotationsTest, BytesToInspect) {
  AnnotationValue annotation_value_bytes;
  annotation_value_bytes.set_bytes({0x01, 0x02, 0x03, 0x04});
  auto annotation_bytes = Annotation{.key = std::move("bytes_key"),
                                     .value = std::make_unique<fuchsia::modular::AnnotationValue>(
                                         std::move(annotation_value_bytes))};

  EXPECT_THAT(ToInspect(*annotation_bytes.value.get()), "bytes");
}

TEST(AnnotationsTest, ToElementAnnotationKeyGlobal) {
  auto annotation_key = "test_annotation_key";

  auto expected_annotation = fuchsia::element::AnnotationKey{
      .namespace_ = element::annotations::kGlobalNamespace, .value = annotation_key};

  EXPECT_TRUE(fidl::Equals(expected_annotation, ToElementAnnotationKey(annotation_key)));
}

TEST(AnnotationsTest, ToElementAnnotationKeySeparated) {
  auto annotation_key = "test_namespace|test_value";

  auto expected_annotation =
      fuchsia::element::AnnotationKey{.namespace_ = "test_namespace", .value = "test_value"};

  EXPECT_TRUE(fidl::Equals(expected_annotation, ToElementAnnotationKey(annotation_key)));
}

TEST(AnnotationsTest, ToElementAnnotationKeySeparatedEscaped) {
  auto annotation_key = "test\\|namespace|test\\|value";

  auto expected_annotation =
      fuchsia::element::AnnotationKey{.namespace_ = "test\\|namespace", .value = "test\\|value"};

  EXPECT_TRUE(fidl::Equals(expected_annotation, ToElementAnnotationKey(annotation_key)));
}

TEST(AnnotationsTest, ToElementAnnotation) {
  constexpr char kTestAnnotationKey[] = "test_key";
  constexpr char kTestAnnotationValue[] = "test_value";

  Annotation modular_annotation = MakeAnnotation(kTestAnnotationKey, kTestAnnotationValue);

  auto expected_annotation = fuchsia::element::Annotation{
      .key = ToElementAnnotationKey(kTestAnnotationKey),
      .value = fuchsia::element::AnnotationValue::WithText(kTestAnnotationValue)};

  auto actual_annotation = ToElementAnnotation(modular_annotation);

  EXPECT_THAT(expected_annotation, element::annotations::AnnotationEq(ByRef(actual_annotation)));
}

TEST(AnnotationsTest, ToElementAnnotations) {
  constexpr char kTestAnnotationKey1[] = "test_key_1";
  constexpr char kTestAnnotationKey2[] = "test_key_2";
  constexpr char kTestAnnotationValue1[] = "test_value_1";
  constexpr char kTestAnnotationValue2[] = "test_value_2";

  std::vector<Annotation> modular_annotations;
  modular_annotations.push_back(MakeAnnotation(kTestAnnotationKey1, kTestAnnotationValue1));
  modular_annotations.push_back(MakeAnnotation(kTestAnnotationKey2, kTestAnnotationValue2));

  std::vector<fuchsia::element::Annotation> element_annotations;
  element_annotations.push_back(fuchsia::element::Annotation{
      .key = ToElementAnnotationKey(kTestAnnotationKey1),
      .value = fuchsia::element::AnnotationValue::WithText(kTestAnnotationValue1)});
  element_annotations.push_back(fuchsia::element::Annotation{
      .key = ToElementAnnotationKey(kTestAnnotationKey2),
      .value = fuchsia::element::AnnotationValue::WithText(kTestAnnotationValue2)});

  auto actual_annotations = ToElementAnnotations(modular_annotations);

  EXPECT_THAT(
      actual_annotations,
      UnorderedElementsAre(element::annotations::AnnotationEq(ByRef(element_annotations[0])),
                           element::annotations::AnnotationEq(ByRef(element_annotations[1]))));
}

TEST(AnnotationsTest, ToElementAnnotationBuffer) {
  static constexpr auto kTestAnnotationKey = "annotation_key";
  static constexpr auto kTestAnnotationValue = "annotation_value";

  fuchsia::mem::Buffer buffer{};
  ASSERT_TRUE(fsl::VmoFromString(kTestAnnotationValue, &buffer));

  auto modular_annotation = Annotation{.key = kTestAnnotationKey,
                                       .value = std::make_unique<fuchsia::modular::AnnotationValue>(
                                           AnnotationValue::WithBuffer(std::move(buffer)))};

  // Set the buffer again because it was moved into |annotation.value|.
  ASSERT_TRUE(fsl::VmoFromString(kTestAnnotationValue, &buffer));

  auto expected_annotation = fuchsia::element::Annotation{
      .key = ToElementAnnotationKey(kTestAnnotationKey),
      .value = fuchsia::element::AnnotationValue::WithBuffer(std::move(buffer))};

  EXPECT_THAT(ToElementAnnotation(modular_annotation),
              element::annotations::AnnotationEq(ByRef(expected_annotation)));
}

}  // namespace
}  // namespace modular::annotations

namespace element::annotations {
namespace {

using Annotation = fuchsia::element::Annotation;
using AnnotationKey = fuchsia::element::AnnotationKey;
using AnnotationValue = fuchsia::element::AnnotationValue;

using ::testing::ByRef;
using ::testing::UnorderedElementsAre;

TEST(ElementAnnotationsTest, ToModularAnnotationKey) {
  auto annotation_key = AnnotationKey{.namespace_ = "test_namespace", .value = "test_value"};

  EXPECT_EQ("test_namespace|test_value", ToModularAnnotationKey(annotation_key));
}

TEST(ElementAnnotationsTest, ToModularAnnotationKeyGlobal) {
  auto annotation_key = AnnotationKey{.namespace_ = kGlobalNamespace, .value = "test_value"};

  EXPECT_EQ("test_value", ToModularAnnotationKey(annotation_key));
}

TEST(ElementAnnotationsTest, ToModularAnnotationKeyEscaped) {
  auto annotation_key = AnnotationKey{.namespace_ = "test|namespace", .value = "test|value"};

  EXPECT_EQ("test\\|namespace|test\\|value", ToModularAnnotationKey(annotation_key));
}

TEST(ElementAnnotationsTest, ToModularAnnotationText) {
  static constexpr auto kTestAnnotationKey = "annotation_key";
  static constexpr auto kTestAnnotationValue = "annotation_value";

  auto annotation_key = modular::annotations::ToElementAnnotationKey(kTestAnnotationKey);
  auto annotation = Annotation{.key = fidl::Clone(annotation_key),
                               .value = AnnotationValue::WithText(kTestAnnotationValue)};

  auto modular_annotation = fuchsia::modular::Annotation{
      .key = ToModularAnnotationKey(annotation_key),
      .value = std::make_unique<fuchsia::modular::AnnotationValue>(
          fuchsia::modular::AnnotationValue::WithText(kTestAnnotationValue))};

  EXPECT_THAT(ToModularAnnotation(annotation),
              modular::annotations::AnnotationEq(ByRef(modular_annotation)));
}

TEST(ElementAnnotationsTest, ToModularAnnotationBuffer) {
  static constexpr auto kTestAnnotationKey = "annotation_key";
  static constexpr auto kTestAnnotationValue = "annotation_value";

  fuchsia::mem::Buffer buffer{};
  ASSERT_TRUE(fsl::VmoFromString(kTestAnnotationValue, &buffer));

  auto annotation_key = modular::annotations::ToElementAnnotationKey(kTestAnnotationKey);
  auto annotation = Annotation{.key = fidl::Clone(annotation_key),
                               .value = AnnotationValue::WithBuffer(std::move(buffer))};

  // Set the buffer again because it was moved into |annotation.value|.
  ASSERT_TRUE(fsl::VmoFromString(kTestAnnotationValue, &buffer));

  auto modular_annotation = fuchsia::modular::Annotation{
      .key = ToModularAnnotationKey(annotation_key),
      .value = std::make_unique<fuchsia::modular::AnnotationValue>(
          fuchsia::modular::AnnotationValue::WithBuffer(std::move(buffer)))};

  EXPECT_THAT(ToModularAnnotation(annotation),
              modular::annotations::AnnotationEq(ByRef(modular_annotation)));
}

TEST(ElementAnnotationsTest, ToModularAnnotationsEmpty) {
  auto modular_annotations = ToModularAnnotations({});
  EXPECT_TRUE(modular_annotations.empty());
}

TEST(ElementAnnotationsTest, ToModularAnnotations) {
  static constexpr auto kTestTextAnnotationKey = "text_annotation_key";
  static constexpr auto kTestTextAnnotationValue = "text_annotation_value";
  static constexpr auto kTestBufferAnnotationKey = "buffer_annotation_key";
  static constexpr auto kTestBufferAnnotationValue = "buffer_annotation_value";

  fuchsia::mem::Buffer buffer{};
  ASSERT_TRUE(fsl::VmoFromString(kTestBufferAnnotationValue, &buffer));

  auto text_annotation_key = modular::annotations::ToElementAnnotationKey(kTestTextAnnotationKey);
  auto text_annotation = Annotation{.key = fidl::Clone(text_annotation_key),
                                    .value = AnnotationValue::WithText(kTestTextAnnotationValue)};

  auto buffer_annotation_key =
      modular::annotations::ToElementAnnotationKey(kTestBufferAnnotationKey);
  auto buffer_annotation = Annotation{.key = fidl::Clone(buffer_annotation_key),
                                      .value = AnnotationValue::WithBuffer(std::move(buffer))};

  std::vector<fuchsia::element::Annotation> annotations;
  annotations.push_back(std::move(text_annotation));
  annotations.push_back(std::move(buffer_annotation));

  auto modular_annotations = ToModularAnnotations(annotations);

  auto expected_text_annotation = fuchsia::modular::Annotation{
      .key = ToModularAnnotationKey(text_annotation_key),
      .value = std::make_unique<fuchsia::modular::AnnotationValue>(
          fuchsia::modular::AnnotationValue::WithText(kTestTextAnnotationValue))};

  // Set the buffer again because it was moved into |buffer_annotation.value|.
  ASSERT_TRUE(fsl::VmoFromString(kTestBufferAnnotationValue, &buffer));

  auto expected_buffer_annotation = fuchsia::modular::Annotation{
      .key = ToModularAnnotationKey(buffer_annotation_key),
      .value = std::make_unique<fuchsia::modular::AnnotationValue>(
          fuchsia::modular::AnnotationValue::WithBuffer(std::move(buffer)))};

  EXPECT_THAT(
      modular_annotations,
      UnorderedElementsAre(modular::annotations::AnnotationEq(ByRef(expected_text_annotation)),
                           modular::annotations::AnnotationEq(ByRef(expected_buffer_annotation))));
}

TEST(ElementAnnotationsTest, IsValidKey) {
  auto key = AnnotationKey{.namespace_ = "test_namespace", .value = "test_value"};
  EXPECT_TRUE(IsValidKey(key));
}

TEST(ElementAnnotationsTest, IsValidKeyEmptyNamespace) {
  auto key = AnnotationKey{.namespace_ = "", .value = "test_value"};
  EXPECT_FALSE(IsValidKey(key));
}

}  // namespace
}  // namespace element::annotations
