// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/metadata.h"

#include <lib/fpromise/result.h>
#include <lib/syslog/cpp/macros.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback_data/annotations/types.h"
#include "src/developer/forensics/feedback_data/attachments/types.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/feedback_data/metadata_schema.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/developer/forensics/utils/errors.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/timekeeper/test_clock.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/schema.h"

#define ANNOTATIONS_JSON_STATE_IS(json, state)                           \
  {                                                                      \
    ASSERT_TRUE(json.HasMember("files"));                                \
    auto& files = json["files"];                                         \
    ASSERT_TRUE(files.HasMember("annotations.json"));                    \
    ASSERT_TRUE(files["annotations.json"].HasMember("state"));           \
    EXPECT_STREQ(files["annotations.json"]["state"].GetString(), state); \
  }

#define HAS_PRESENT_ANNOTATION(json, name)                                                         \
  {                                                                                                \
    ASSERT_TRUE(json.HasMember("files"));                                                          \
    auto& files = json["files"];                                                                   \
    ASSERT_TRUE(files.HasMember("annotations.json"));                                              \
    ASSERT_TRUE(files["annotations.json"].HasMember("present annotations"));                       \
    {                                                                                              \
      bool has_annotation = false;                                                                 \
      for (const auto& annotation : files["annotations.json"]["present annotations"].GetArray()) { \
        if (strcmp(annotation.GetString(), name) == 0) {                                           \
          has_annotation = true;                                                                   \
          break;                                                                                   \
        }                                                                                          \
      }                                                                                            \
      EXPECT_TRUE(has_annotation&& name);                                                          \
    }                                                                                              \
  }

#define HAS_MISSING_ANNOTATION(json, name, error)                                            \
  {                                                                                          \
    ASSERT_TRUE(json.HasMember("files"));                                                    \
    auto& files = json["files"];                                                             \
    ASSERT_TRUE(files.HasMember("annotations.json"));                                        \
    ASSERT_TRUE(files["annotations.json"].HasMember("missing annotations"));                 \
    ASSERT_TRUE(files["annotations.json"]["missing annotations"].HasMember(name));           \
    EXPECT_STREQ(files["annotations.json"]["missing annotations"][name].GetString(), error); \
  }

#define HAS_COMPLETE_ATTACHMENT(json, name)                     \
  {                                                             \
    ASSERT_TRUE(json.HasMember("files"));                       \
    auto& files = json["files"];                                \
    ASSERT_TRUE(files.HasMember(name));                         \
    ASSERT_TRUE(files[name].HasMember("state"));                \
    EXPECT_STREQ(files[name]["state"].GetString(), "complete"); \
  }

#define HAS_PARTIAL_ATTACHMENT(json, name, error)              \
  {                                                            \
    ASSERT_TRUE(json.HasMember("files"));                      \
    auto& files = json["files"];                               \
    ASSERT_TRUE(files.HasMember(name));                        \
    ASSERT_TRUE(files[name].HasMember("state"));               \
    EXPECT_STREQ(files[name]["state"].GetString(), "partial"); \
    ASSERT_TRUE(files[name].HasMember("error"));               \
    EXPECT_STREQ(files[name]["error"].GetString(), error);     \
  }

#define HAS_MISSING_ATTACHMENT(json, name, error)              \
  {                                                            \
    ASSERT_TRUE(json.HasMember("files"));                      \
    auto& files = json["files"];                               \
    ASSERT_TRUE(files.HasMember(name));                        \
    ASSERT_TRUE(files[name].HasMember("state"));               \
    EXPECT_STREQ(files[name]["state"].GetString(), "missing"); \
    ASSERT_TRUE(files[name].HasMember("error"));               \
    EXPECT_STREQ(files[name]["error"].GetString(), error);     \
  }

#define UTC_MONOTONIC_DIFFERENCE_IS(json, name, utc_monotonic_difference) \
  {                                                                       \
    ASSERT_TRUE(json.HasMember("files"));                                 \
    auto& files = json["files"];                                          \
    ASSERT_TRUE(files.HasMember(name));                                   \
    ASSERT_TRUE(files[name].HasMember("utc_monotonic_difference_nanos")); \
    ASSERT_TRUE(files[name]["utc_monotonic_difference_nanos"].IsInt64()); \
    EXPECT_EQ(files[name]["utc_monotonic_difference_nanos"].GetInt64(),   \
              utc_monotonic_difference.get());                            \
  }

namespace forensics {
namespace feedback_data {
namespace {

constexpr zx::duration kPreviousBootUtcMonotonicDifference = zx::sec(100);
constexpr const char* kSnapshotUuid = "snapshot_uuid";

class MetadataTest : public UnitTestFixture {
 protected:
  void SetUp() override {
    FX_CHECK(files::WriteFile(files::JoinPath("/cache", kUtcMonotonicDifferenceFile),
                              std::to_string(kPreviousBootUtcMonotonicDifference.get())));
  }

  void TearDown() override {
    files::DeletePath(files::JoinPath("/tmp", kUtcMonotonicDifferenceFile), /*recursive=*/false);
    files::DeletePath(files::JoinPath("/cache", kUtcMonotonicDifferenceFile), /*recursive=*/false);
  }

  void SetUpMetadata(const AnnotationKeys& annotation_allowlist,
                     const AttachmentKeys& attachment_allowlist) {
    metadata_ = std::make_unique<Metadata>(dispatcher(), &clock_, /*is_first_instance=*/true,
                                           annotation_allowlist, attachment_allowlist);
  }

  // Get the integrity metadata for the provided annotations and attachments, check that it adheres
  // to the schema, and turn it into a json document
  rapidjson::Document MakeJsonReport(const ::fpromise::result<Annotations>& annotations,
                                     const ::fpromise::result<Attachments>& attachments,
                                     const bool missing_non_platform_annotations = false) {
    FX_CHECK(metadata_);
    const auto metadata_str = metadata_->MakeMetadata(annotations, attachments, kSnapshotUuid,
                                                      missing_non_platform_annotations);

    rapidjson::Document json;
    FX_CHECK(!json.Parse(metadata_str.c_str()).HasParseError());

    rapidjson::Document schema_json;
    FX_CHECK(!schema_json.Parse(kMetadataSchema).HasParseError());
    rapidjson::SchemaDocument schema(schema_json);
    rapidjson::SchemaValidator validator(schema);
    FX_CHECK(json.Accept(validator));

    // Convert to std::string to use its '==' operator.
    FX_CHECK(json["snapshot_version"].GetString() == std::string(SnapshotVersion::kString));
    FX_CHECK(json["metadata_version"].GetString() == std::string(Metadata::kVersion));
    FX_CHECK(json["snapshot_uuid"].GetString() == std::string(kSnapshotUuid));

    return json;
  }

  timekeeper::TestClock clock_;
  std::unique_ptr<Metadata> metadata_;
};

TEST_F(MetadataTest, Check_AddsMissingAnnotationsOnNoAnnotations) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  SetUpMetadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(::fpromise::error(), ::fpromise::error());
  HAS_MISSING_ANNOTATION(metadata_json, "annotation 1", "feedback logic error");
}

TEST_F(MetadataTest, Check_AddsMissingAnnotationsOnEmptyAnnotations) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  SetUpMetadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(::fpromise::ok<Annotations>({}), ::fpromise::error());
  HAS_MISSING_ANNOTATION(metadata_json, "annotation 1", "feedback logic error");
}

TEST_F(MetadataTest, Check_AddsMissingAttachmentsOnNoAttachments) {
  const AttachmentKeys attachment_allowlist = {
      "attachment 1",
  };

  SetUpMetadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(::fpromise::error(), ::fpromise::error());
  HAS_MISSING_ATTACHMENT(metadata_json, "attachment 1", "feedback logic error");
}

TEST_F(MetadataTest, Check_AddsMissingAttachmentsOnEmptyAttachments) {
  const AttachmentKeys attachment_allowlist = {
      "attachment 1",
  };

  SetUpMetadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(::fpromise::error(), ::fpromise::ok<Attachments>({}));
  HAS_MISSING_ATTACHMENT(metadata_json, "attachment 1", "feedback logic error");
}

TEST_F(MetadataTest, Check_FormatAnnotationsProperly) {
  const AnnotationKeys annotation_allowlist = {
      "present annotation 1",
      "present annotation 2",
      "missing annotation 1",
      "missing annotation 2",
  };

  const Annotations annotations = {
      {"present annotation 1", ""},
      {"present annotation 2", ""},
      {"missing annotation 1", Error::kConnectionError},
      {"missing annotation 2", Error::kFileWriteFailure},
  };

  SetUpMetadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(::fpromise::ok(std::move(annotations)), ::fpromise::error());

  ANNOTATIONS_JSON_STATE_IS(metadata_json, "partial");

  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 1");
  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 2");

  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 1", "FIDL connection error");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 2", "file write failure");
}

TEST_F(MetadataTest, Check_FormatAttachmentsProperly) {
  const AttachmentKeys attachment_allowlist = {
      "complete attachment 1", "complete attachment 2", "partial attachment 1",
      "partial attachment 2",  "missing attachment 1",  "missing attachment 2",
  };

  const Attachments attachments = {
      {"complete attachment 1", AttachmentValue("")},
      {"complete attachment 2", AttachmentValue("")},
      {"partial attachment 1", AttachmentValue("", Error::kTimeout)},
      {"partial attachment 2", AttachmentValue("", Error::kAsyncTaskPostFailure)},
      {"missing attachment 1", AttachmentValue(Error::kBadValue)},
      {"missing attachment 2", AttachmentValue(Error::kFileReadFailure)},
  };

  SetUpMetadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json =
      MakeJsonReport(::fpromise::error(), ::fpromise::ok<Attachments>(std::move(attachments)));

  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 1");
  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 2");

  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 1", "data collection timeout");
  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 2", "async post task failure");

  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 1", "bad data returned");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 2", "file read failure");
}

TEST_F(MetadataTest, Check_NonPlatformAnnotationsComplete) {
  const Annotations annotations = {
      {"non-platform annotation", ""},
  };

  SetUpMetadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(::fpromise::ok(std::move(annotations)), ::fpromise::error());

  HAS_PRESENT_ANNOTATION(metadata_json, "non-platform annotations");
}

TEST_F(MetadataTest, Check_NonPlatformAnnotationsPartial) {
  const Annotations annotations = {
      {"non-platform annotation", ""},
  };

  SetUpMetadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(::fpromise::ok(std::move(annotations)), ::fpromise::error(),
                     /*missing_non_platform_annotations=*/true);

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST_F(MetadataTest, Check_NonPlatformAnnotationsMissing) {
  SetUpMetadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(::fpromise::error(), ::fpromise::error(),
                                            /*missing_non_platform_annotations=*/true);

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST_F(MetadataTest, Check_SmokeTest) {
  const AnnotationKeys annotation_allowlist = {
      "present annotation 1", "present annotation 2", "missing annotation 1",
      "missing annotation 2", "missing annotation 3",
  };

  const Annotations annotations = {
      {"present annotation 1", ""},
      {"present annotation 2", ""},
      {"missing annotation 1", Error::kConnectionError},
      {"missing annotation 2", Error::kFileWriteFailure},
      {"non-platform annotation 1", ""},
  };

  const AttachmentKeys attachment_allowlist = {
      "complete attachment 1", "complete attachment 2", "partial attachment 1",
      "partial attachment 2",  "missing attachment 1",  "missing attachment 2",
      "missing attachment 3",
  };
  const Attachments attachments = {
      {"complete attachment 1", AttachmentValue("")},
      {"complete attachment 2", AttachmentValue("")},
      {"partial attachment 1", AttachmentValue("", Error::kTimeout)},
      {"partial attachment 2", AttachmentValue("", Error::kAsyncTaskPostFailure)},
      {"missing attachment 1", AttachmentValue(Error::kBadValue)},
      {"missing attachment 2", AttachmentValue(Error::kFileReadFailure)},
  };

  SetUpMetadata(annotation_allowlist, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(::fpromise::ok<Annotations>(std::move(annotations)),
                                            ::fpromise::ok<Attachments>(std::move(attachments)),
                                            /*missing_non_platform_annotations=*/true);

  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 1");
  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 2");

  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 1", "data collection timeout");
  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 2", "async post task failure");

  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 1", "bad data returned");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 2", "file read failure");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 3", "feedback logic error");

  ANNOTATIONS_JSON_STATE_IS(metadata_json, "partial");

  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 1");
  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 2");

  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 1", "FIDL connection error");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 2", "file write failure");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 3", "feedback logic error");

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST_F(MetadataTest, Check_EmptySnapshot) {
  SetUpMetadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  auto metadata_str =
      metadata_->MakeMetadata(::fpromise::error(), ::fpromise::error(), kSnapshotUuid,
                              /*missing_non_platform_annotations=*/false);

  rapidjson::Document json;
  ASSERT_TRUE(!json.Parse(metadata_str.c_str()).HasParseError());

  rapidjson::Document schema_json;
  ASSERT_TRUE(!schema_json.Parse(kMetadataSchema).HasParseError());
  rapidjson::SchemaDocument schema(schema_json);
  rapidjson::SchemaValidator validator(schema);
  ASSERT_TRUE(json.Accept(validator));

  // Convert to std::string to use its '==' operator.
  EXPECT_STREQ(json["snapshot_version"].GetString(), SnapshotVersion::kString);
  EXPECT_STREQ(json["metadata_version"].GetString(), Metadata::kVersion);
  EXPECT_STREQ(json["snapshot_uuid"].GetString(), kSnapshotUuid);

  EXPECT_TRUE(json.HasMember("files"));
  EXPECT_TRUE(json["files"].IsObject());
  EXPECT_TRUE(json["files"].GetObject().ObjectEmpty());
}

TEST_F(MetadataTest, Check_UtcMonotonicDifference) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  const AttachmentKeys attachment_allowlist = {
      kAttachmentInspect,
      kAttachmentLogKernel,
      kAttachmentLogSystem,
      kPreviousLogsFilePath,
  };

  const Annotations annotations = {
      {"annotation 1", "annotation"},
  };

  const Attachments attachments = {
      {kAttachmentInspect, AttachmentValue("")},
      {kAttachmentLogKernel, AttachmentValue("")},
      {kAttachmentLogSystem, AttachmentValue("")},
      {kAttachmentLogSystemPrevious, AttachmentValue("")},
  };

  SetUpMetadata(annotation_allowlist, attachment_allowlist);
  RunLoopUntilIdle();

  zx::time monotonic;
  timekeeper::time_utc utc;

  clock_.Set(zx::time(0));

  const zx::duration utc_monotonic_difference(utc.get() - monotonic.get());

  monotonic = clock_.Now();
  ASSERT_EQ(clock_.UtcNow(&utc), ZX_OK);

  const auto metadata_json = MakeJsonReport(::fpromise::ok<Annotations>(std::move(annotations)),
                                            ::fpromise::ok<Attachments>(std::move(attachments)));

  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentInspect, utc_monotonic_difference);
  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentLogKernel, utc_monotonic_difference);
  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentLogSystem, utc_monotonic_difference);
  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentLogSystemPrevious,
                              kPreviousBootUtcMonotonicDifference);

  ASSERT_TRUE(metadata_json["files"].HasMember(kAttachmentAnnotations));
  ASSERT_FALSE(
      metadata_json["files"][kAttachmentAnnotations].HasMember("utc_monotonic_difference"));
}

TEST_F(MetadataTest, Check_NoUtcMontonicDifferenceAvailable) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  const AttachmentKeys attachment_allowlist = {
      "attachment 1",
  };

  const Annotations annotations = {
      {"annotation 1", ""},
  };

  const Attachments attachments = {
      {"attachment 1", AttachmentValue("")},
  };

  SetUpMetadata(annotation_allowlist, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(::fpromise::ok<Annotations>(std::move(annotations)),
                                            ::fpromise::ok<Attachments>(std::move(attachments)));

  ASSERT_TRUE(metadata_json["files"].HasMember(kAttachmentAnnotations));
  ASSERT_FALSE(
      metadata_json["files"][kAttachmentAnnotations].HasMember("utc_monotonic_difference"));

  ASSERT_TRUE(metadata_json["files"].HasMember("attachment 1"));
  ASSERT_FALSE(metadata_json["files"]["attachment 1"].HasMember("utc_monotonic_difference"));
}

TEST_F(MetadataTest, Check_NoUtcMonotonicDifferenceMissingFile) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  const AttachmentKeys attachment_allowlist = {
      kAttachmentInspect,
      kAttachmentLogKernel,
      kAttachmentLogSystem,
      kPreviousLogsFilePath,
  };

  const Annotations annotations = {
      {"annotation 1", "annotation"},
  };

  const Attachments attachments = {
      {kAttachmentInspect, AttachmentValue("")},
      {kAttachmentLogKernel, AttachmentValue("")},
      {kAttachmentLogSystem, AttachmentValue("")},
      {kAttachmentLogSystemPrevious, AttachmentValue(Error::kCustom)},
  };

  SetUpMetadata(annotation_allowlist, attachment_allowlist);
  RunLoopUntilIdle();

  zx::time monotonic;
  timekeeper::time_utc utc;

  clock_.Set(zx::time(0));

  const zx::duration utc_monotonic_difference(utc.get() - monotonic.get());

  monotonic = clock_.Now();
  ASSERT_EQ(clock_.UtcNow(&utc), ZX_OK);

  const auto metadata_json = MakeJsonReport(::fpromise::ok<Annotations>(std::move(annotations)),
                                            ::fpromise::ok<Attachments>(std::move(attachments)));

  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentInspect, utc_monotonic_difference);
  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentLogKernel, utc_monotonic_difference);
  UTC_MONOTONIC_DIFFERENCE_IS(metadata_json, kAttachmentLogSystem, utc_monotonic_difference);

  ASSERT_TRUE(metadata_json["files"].HasMember(kAttachmentLogSystemPrevious));
  ASSERT_FALSE(
      metadata_json["files"][kAttachmentLogSystemPrevious].HasMember("utc_monotonic_difference"));

  ASSERT_TRUE(metadata_json["files"].HasMember(kAttachmentAnnotations));
  ASSERT_FALSE(
      metadata_json["files"][kAttachmentAnnotations].HasMember("utc_monotonic_difference"));
}

struct TestParam {
  std::string test_name;
  AnnotationKeys annotation_allowlist;
  Annotations annotations;
  bool missing_non_platform_annotations;
  std::string state;
};

class AnnotationsJsonStateTest : public MetadataTest,
                                 public testing::WithParamInterface<TestParam> {};

INSTANTIATE_TEST_SUITE_P(WithVariousAnnotations, AnnotationsJsonStateTest,
                         ::testing::ValuesIn(
                             std::vector<TestParam>(
                                 {
                                     TestParam{
                                         .test_name = "CompletePlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", ""},
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "complete",

                                     },
                                     TestParam{
                                         .test_name = "CompletePlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", ""},
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "CompletePlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", ""},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", ""},
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", ""},
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", ""},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"non-platform", ""},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations = {},
                                         .missing_non_platform_annotations = true,
                                         .state = "missing",

                                     },
                                 })),
                         [](const testing::TestParamInfo<TestParam>& info) {
                           return info.param.test_name;
                         });
TEST_P(AnnotationsJsonStateTest, Succeed) {
  const auto param = GetParam();
  SetUpMetadata(param.annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(::fpromise::ok(param.annotations), ::fpromise::error(),
                                            param.missing_non_platform_annotations);
  ANNOTATIONS_JSON_STATE_IS(metadata_json, param.state.c_str());
}

}  // namespace
}  // namespace feedback_data
}  // namespace forensics
