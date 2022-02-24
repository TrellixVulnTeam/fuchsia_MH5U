// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/hwinfo/cpp/fidl.h>
#include <fuchsia/intl/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <fuchsia/metrics/cpp/fidl.h>
#include <fuchsia/metrics/test/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/update/channelcontrol/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fpromise/result.h>
#include <lib/inspect/contrib/cpp/archive_reader.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/cpp/testing/test_with_environment_fixture.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback/tests/zx_object_util.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/testing/fakes/cobalt.h"
#include "src/developer/forensics/testing/gmatchers.h"
#include "src/developer/forensics/utils/archive.h"
#include "src/developer/forensics/utils/cobalt/metrics.h"
#include "src/developer/forensics/utils/cobalt/metrics_registry.cb.h"
#include "src/lib/files/file.h"
#include "src/lib/fostr/fidl/fuchsia/feedback/formatting.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/uuid/uuid.h"
#include "src/ui/lib/escher/test/common/gtest_vulkan.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/schema.h"

namespace forensics::feedback {
namespace {

using fuchsia::feedback::Annotations;
using fuchsia::feedback::Attachment;
using fuchsia::feedback::ComponentDataRegisterSyncPtr;
using fuchsia::feedback::DataProviderSyncPtr;
using fuchsia::feedback::GetAnnotationsParameters;
using fuchsia::feedback::GetSnapshotParameters;
using fuchsia::feedback::ImageEncoding;
using fuchsia::feedback::Screenshot;
using fuchsia::feedback::Snapshot;
using fuchsia::hwinfo::BoardInfo;
using fuchsia::hwinfo::BoardPtr;
using fuchsia::hwinfo::ProductInfo;
using fuchsia::hwinfo::ProductPtr;
using fuchsia::intl::Profile;
using testing::Key;
using testing::UnorderedElementsAreArray;

class LogListener : public fuchsia::logger::LogListenerSafe {
 public:
  LogListener(std::shared_ptr<sys::ServiceDirectory> services) : binding_(this) {
    binding_.Bind(log_listener_.NewRequest());

    fuchsia::logger::LogPtr logger = services->Connect<fuchsia::logger::Log>();
    logger->ListenSafe(std::move(log_listener_), /*options=*/nullptr);
  }

  bool HasLogs() { return has_logs_; }

 private:
  // |fuchsia::logger::LogListenerSafe|
  void LogMany(::std::vector<fuchsia::logger::LogMessage> log, LogManyCallback done) {
    has_logs_ = true;
    done();
  }
  void Log(fuchsia::logger::LogMessage log, LogCallback done) {
    has_logs_ = true;
    done();
  }
  void Done() { FX_NOTIMPLEMENTED(); }

  ::fidl::Binding<fuchsia::logger::LogListenerSafe> binding_;
  fuchsia::logger::LogListenerSafePtr log_listener_;
  bool has_logs_ = false;
};

// Smoke-tests the real environment service for the fuchsia.feedback FIDL interfaces,
// connecting through FIDL.
class FeedbackIntegrationTest : public gtest::TestWithEnvironmentFixture {
 public:
  void SetUp() override {
    environment_services_ = sys::ServiceDirectory::CreateFromNamespace();
    environment_services_->Connect(crash_register_.NewRequest());
    environment_services_->Connect(crash_reporter_.NewRequest());
    fake_cobalt_ = std::make_unique<fakes::Cobalt>(environment_services_);
  }

  void TearDown() override {
    if (inspect_test_app_controller_) {
      inspect_test_app_controller_->Kill();
      bool is_inspect_test_app_terminated = false;
      inspect_test_app_controller_.events().OnTerminated =
          [&is_inspect_test_app_terminated](int64_t code, fuchsia::sys::TerminationReason reason) {
            FX_CHECK(reason == fuchsia::sys::TerminationReason::EXITED);
            is_inspect_test_app_terminated = true;
          };
      RunLoopUntil([&is_inspect_test_app_terminated] { return is_inspect_test_app_terminated; });
    }
  }

 protected:
  // Makes sure the component serving fuchsia.logger.Log is up and running as the DumpLogs() request
  // could time out on machines where the component is too slow to start.
  //
  // Syslog are generally handled by a single logger that implements two protocols:
  //   (1) fuchsia.logger.LogSink to write syslog messages
  //   (2) fuchsia.logger.Log to read syslog messages and kernel log messages.
  // Returned syslog messages are restricted to the ones that were written using its LogSink while
  // kernel log messages are the same for all loggers.
  //
  // In this integration test, we inject a "fresh copy" of archivist.cmx for fuchsia.logger.Log so
  // we can retrieve the syslog messages. But we do _not_ inject that same archivist.cmx for
  // fuchsia.logger.LogSink as it would swallow all the error and warning messages the other
  // injected services could produce and make debugging really hard. Therefore, the injected
  // archivist.cmx does not have any syslog messages and will only have the global kernel log
  // messages.
  //
  // When archivist.cmx spawns, it will start collecting asynchronously kernel log messages. But if
  // DumpLogs() is called "too soon", it will immediately return empty logs instead of waiting on
  // the kernel log collection (fxbug.dev/4665), resulting in a flaky test (fxbug.dev/8303). We thus
  // spawn archivist.cmx on advance and wait for it to have at least one message before running the
  // actual test.
  void WaitForLogger() {
    LogListener log_listener(environment_services_);
    RunLoopUntil([&log_listener] { return log_listener.HasLogs(); });
  }

  // Makes sure the component serving fuchsia.update.channelcontrol.ChannelControl is up and running
  // as the GetCurrent() request could time out on machines where the component is too slow to
  // start.
  void WaitForChannelProvider() {
    fuchsia::update::channelcontrol::ChannelControlSyncPtr channel_provider;
    environment_services_->Connect(channel_provider.NewRequest());
    std::string unused;
    ASSERT_EQ(channel_provider->GetCurrent(&unused), ZX_OK);
  }

  // Makes sure there is at least one component in the test environment that exposes some Inspect
  // data.
  //
  // This is useful as we are excluding system_objects paths from the Inspect discovery and the test
  // component itself only has a system_objects Inspect node.
  void WaitForInspect() {
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = "fuchsia-pkg://fuchsia.com/feedback-tests#meta/inspect_test_app.cmx";
    environment_ = CreateNewEnclosingEnvironment("inspect_test_app_environment", CreateServices());
    environment_->CreateComponent(std::move(launch_info),
                                  inspect_test_app_controller_.NewRequest());
    bool ready = false;
    inspect_test_app_controller_.events().OnDirectoryReady = [&ready] { ready = true; };
    RunLoopUntil([&ready] { return ready; });

    // Additionally wait for the component to appear in the observer's output.
    async::Executor executor(dispatcher());

    inspect::contrib::ArchiveReader reader(
        environment_services_->Connect<fuchsia::diagnostics::ArchiveAccessor>(),
        {"inspect_test_app_environment/inspect_test_app.cmx:root"});

    bool done = false;
    executor.schedule_task(
        reader.SnapshotInspectUntilPresent({"inspect_test_app.cmx"})
            .then([&](::fpromise::result<std::vector<inspect::contrib::DiagnosticsData>,
                                         std::string>& unused) { done = true; }));
    RunLoopUntil([&done] { return done; });
  }

  // Makes sure the component serving fuchsia.hwinfo.BoardInfo is up and running as the
  // GetInfo() request could time out on machines where the component is too slow to start.
  void WaitForBoardProvider() {
    fuchsia::hwinfo::BoardPtr board_provider;
    environment_services_->Connect(board_provider.NewRequest());

    bool ready = false;
    board_provider->GetInfo([&](BoardInfo board_info) { ready = true; });
    RunLoopUntil([&ready] { return ready; });
  }

  // Makes sure the component serving fuchsia.hwinfo.ProductInfo is up and running as the
  // GetInfo() request could time out on machines where the component is too slow to start.
  void WaitForProductProvider() {
    fuchsia::hwinfo::ProductPtr product_provider;
    environment_services_->Connect(product_provider.NewRequest());

    bool ready = false;
    product_provider->GetInfo([&](ProductInfo product_info) { ready = true; });
    RunLoopUntil([&ready] { return ready; });
  }

  // Makes sure the component serving fuchsia.intl.PropertyProvider is up and running as the
  // GetProfile() request could time out on machines where the component is too slow to start.
  void WaitForProfileProvider() {
    fuchsia::intl::PropertyProviderPtr property_provider;
    environment_services_->Connect(property_provider.NewRequest());

    bool ready = false;
    property_provider->GetProfile([&](Profile profile) { ready = true; });
    RunLoopUntil([&ready] { return ready; });
  }

  void FileCrashReport() {
    fuchsia::feedback::CrashReport report;
    report.set_program_name("crashing_program");

    fuchsia::feedback::CrashReporter_File_Result out_result;
    ASSERT_EQ(crash_reporter_->File(std::move(report), &out_result), ZX_OK);
    EXPECT_TRUE(out_result.is_response());
  }

  void RegisterProduct() {
    fuchsia::feedback::CrashReportingProduct product;
    product.set_name("some name");
    product.set_version("some version");
    product.set_channel("some channel");

    EXPECT_EQ(crash_register_->Upsert("some/component/URL", std::move(product)), ZX_OK);
  }

  void RegisterProductWithAck() {
    fuchsia::feedback::CrashReportingProduct product;
    product.set_name("some name");
    product.set_version("some version");
    product.set_channel("some channel");

    EXPECT_EQ(crash_register_->UpsertWithAck("some/component/URL", std::move(product)), ZX_OK);
  }

 protected:
  std::shared_ptr<sys::ServiceDirectory> environment_services_;

 private:
  std::unique_ptr<sys::testing::EnclosingEnvironment> environment_;
  fuchsia::feedback::CrashReportingProductRegisterSyncPtr crash_register_;
  fuchsia::feedback::CrashReporterSyncPtr crash_reporter_;

  fuchsia::sys::ComponentControllerPtr inspect_test_app_controller_;

 protected:
  std::unique_ptr<fakes::Cobalt> fake_cobalt_;
};

// Smoke-tests the actual service for fuchsia.feedback.CrashReportingProductRegister, connecting
// through FIDL.
TEST_F(FeedbackIntegrationTest, CrashRegister_SmokeTest) {
  RegisterProduct();
  RegisterProductWithAck();
}

// Smoke-tests the actual service for fuchsia.feedback.CrashReporter, connecting through FIDL.
TEST_F(FeedbackIntegrationTest, CrashReporter_SmokeTest) {
  FileCrashReport();

  fake_cobalt_->RegisterExpectedEvent(cobalt::CrashState::kFiled, 1);
  fake_cobalt_->RegisterExpectedEvent(cobalt::CrashState::kArchived, 1);

  EXPECT_TRUE(
      fake_cobalt_->MeetsExpectedEvents(fuchsia::metrics::test::LogMethod::LOG_OCCURRENCE, true));
}

TEST_F(FeedbackIntegrationTest, ComponentDataRegister_Upsert_SmokeTest) {
  ComponentDataRegisterSyncPtr data_register;
  environment_services_->Connect(data_register.NewRequest());

  ASSERT_EQ(data_register->Upsert({}), ZX_OK);
}

// We use VK_TEST instead of the regular TEST macro because Scenic needs Vulkan to operate properly
// and take a screenshot. Note that calls to Scenic hang indefinitely for headless devices so this
// test assumes the device has a display like the other Scenic tests, see fxbug.dev/24479.
VK_TEST_F(FeedbackIntegrationTest, DataProvider_GetScreenshot_SmokeTest) {
  DataProviderSyncPtr data_provider;
  environment_services_->Connect(data_provider.NewRequest());

  std::unique_ptr<Screenshot> out_screenshot;
  ASSERT_EQ(data_provider->GetScreenshot(ImageEncoding::PNG, &out_screenshot), ZX_OK);
  // We cannot expect a particular payload in the response because Scenic might return a screenshot
  // or not depending on which device the test runs.
}

constexpr char kInspectJsonSchema[] = R"({
  "type": "array",
  "items": {
    "type": "object",
    "properties": {
      "moniker": {
        "type": "string"
      },
      "payload": {
        "type": "object"
      }
    },
    "required": [
      "moniker",
      "payload"
    ],
    "additionalProperties": true
  },
  "uniqueItems": true
})";

TEST_F(FeedbackIntegrationTest, DataProvider_GetSnapshot_CheckKeys) {
  // We make sure the components serving the services GetSnapshot() connects to are up and running.

  WaitForLogger();
  WaitForChannelProvider();
  WaitForInspect();
  WaitForBoardProvider();
  WaitForProductProvider();
  WaitForProfileProvider();

  DataProviderSyncPtr data_provider;
  environment_services_->Connect(data_provider.NewRequest());

  Snapshot snapshot;
  ASSERT_EQ(data_provider->GetSnapshot(GetSnapshotParameters(), &snapshot), ZX_OK);

  // We cannot expect a particular value for each annotation or attachment because values might
  // depend on which device the test runs (e.g., board name) or what happened prior to running this
  // test (e.g., logs). But we should expect the keys to be present.
  ASSERT_TRUE(snapshot.has_annotations());
  EXPECT_THAT(snapshot.annotations(),
              testing::IsSupersetOf({
                  MatchesKey(feedback_data::kAnnotationBuildBoard),
                  MatchesKey(feedback_data::kAnnotationBuildIsDebug),
                  MatchesKey(feedback_data::kAnnotationBuildLatestCommitDate),
                  MatchesKey(feedback_data::kAnnotationBuildProduct),
                  MatchesKey(feedback_data::kAnnotationBuildVersion),
                  MatchesKey(feedback_data::kAnnotationDeviceBoardName),
                  MatchesKey(feedback_data::kAnnotationDeviceFeedbackId),
                  MatchesKey(feedback::kDeviceUptimeKey),
                  MatchesKey(feedback::kDeviceUtcTimeKey),
                  MatchesKey(feedback_data::kAnnotationHardwareBoardName),
                  MatchesKey(feedback_data::kAnnotationHardwareBoardRevision),
                  MatchesKey(feedback_data::kAnnotationHardwareProductLanguage),
                  MatchesKey(feedback_data::kAnnotationHardwareProductLocaleList),
                  MatchesKey(feedback_data::kAnnotationHardwareProductManufacturer),
                  MatchesKey(feedback_data::kAnnotationHardwareProductModel),
                  MatchesKey(feedback_data::kAnnotationHardwareProductName),
                  MatchesKey(feedback_data::kAnnotationHardwareProductRegulatoryDomain),
                  MatchesKey(feedback_data::kAnnotationHardwareProductSKU),
                  MatchesKey(feedback_data::kAnnotationSystemBootIdCurrent),
                  MatchesKey(feedback_data::kAnnotationSystemLastRebootReason),
                  MatchesKey(feedback_data::kAnnotationSystemTimezonePrimary),
                  MatchesKey(feedback_data::kAnnotationSystemUpdateChannelCurrent),
                  MatchesKey(feedback_data::kAnnotationSystemUpdateChannelTarget),
              }));

  ASSERT_TRUE(snapshot.has_archive());
  EXPECT_STREQ(snapshot.archive().key.c_str(), feedback_data::kSnapshotFilename);
  std::map<std::string, std::string> unpacked_attachments;
  ASSERT_TRUE(Unpack(snapshot.archive().value, &unpacked_attachments));
  ASSERT_THAT(unpacked_attachments, testing::UnorderedElementsAreArray({
                                        Key(feedback_data::kAttachmentAnnotations),
                                        Key(feedback_data::kAttachmentBuildSnapshot),
                                        Key(feedback_data::kAttachmentInspect),
                                        Key(feedback_data::kAttachmentLogKernel),
                                        Key(feedback_data::kAttachmentLogSystem),
                                        Key(feedback_data::kAttachmentMetadata),
                                    }));

  ASSERT_NE(unpacked_attachments.find(feedback_data::kAttachmentInspect),
            unpacked_attachments.end());
  const std::string inspect_json = unpacked_attachments[feedback_data::kAttachmentInspect];
  ASSERT_FALSE(inspect_json.empty());

  // JSON verification.
  // We check that the output is a valid JSON and that it matches the schema.
  rapidjson::Document json;
  ASSERT_FALSE(json.Parse(inspect_json.c_str()).HasParseError());
  rapidjson::Document schema_json;
  ASSERT_FALSE(schema_json.Parse(kInspectJsonSchema).HasParseError());
  rapidjson::SchemaDocument schema(schema_json);
  rapidjson::SchemaValidator validator(schema);
  EXPECT_TRUE(json.Accept(validator));

  // We then check that we get the expected Inspect data for the injected test app.
  bool has_entry_for_test_app = false;
  for (const auto& obj : json.GetArray()) {
    const std::string path = obj["moniker"].GetString();
    if (path.find("inspect_test_app.cmx") != std::string::npos) {
      has_entry_for_test_app = true;
      const auto contents = obj["payload"].GetObject();
      ASSERT_TRUE(contents.HasMember("root"));
      const auto root = contents["root"].GetObject();
      ASSERT_TRUE(root.HasMember("obj1"));
      ASSERT_TRUE(root.HasMember("obj2"));
      const auto obj1 = root["obj1"].GetObject();
      const auto obj2 = root["obj2"].GetObject();
      ASSERT_TRUE(obj1.HasMember("version"));
      ASSERT_TRUE(obj2.HasMember("version"));
      EXPECT_STREQ(obj1["version"].GetString(), "1.0");
      EXPECT_STREQ(obj2["version"].GetString(), "1.0");
      ASSERT_TRUE(obj1.HasMember("value"));
      ASSERT_TRUE(obj2.HasMember("value"));
      EXPECT_EQ(obj1["value"].GetUint(), 100u);
      EXPECT_EQ(obj2["value"].GetUint(), 200u);
    }
  }
  EXPECT_TRUE(has_entry_for_test_app);
}

TEST_F(FeedbackIntegrationTest, DataProvider_GetAnnotation_CheckKeys) {
  // We make sure the components serving the services GetAnnotations() connects to are up and
  // running.
  WaitForChannelProvider();
  WaitForBoardProvider();
  WaitForProductProvider();
  WaitForProfileProvider();

  DataProviderSyncPtr data_provider;
  environment_services_->Connect(data_provider.NewRequest());

  Annotations annotations;
  ASSERT_EQ(data_provider->GetAnnotations(GetAnnotationsParameters(), &annotations), ZX_OK);

  // We cannot expect a particular value for each annotation because values might depend on which
  // device the test runs (e.g., board name). But we should expect the keys to be present.
  ASSERT_TRUE(annotations.has_annotations());
  EXPECT_THAT(annotations.annotations(),
              testing::IsSupersetOf({
                  MatchesKey(feedback_data::kAnnotationBuildBoard),
                  MatchesKey(feedback_data::kAnnotationBuildIsDebug),
                  MatchesKey(feedback_data::kAnnotationBuildLatestCommitDate),
                  MatchesKey(feedback_data::kAnnotationBuildProduct),
                  MatchesKey(feedback_data::kAnnotationBuildVersion),
                  MatchesKey(feedback_data::kAnnotationDeviceBoardName),
                  MatchesKey(feedback_data::kAnnotationDeviceFeedbackId),
                  MatchesKey(feedback::kDeviceUptimeKey),
                  MatchesKey(feedback::kDeviceUtcTimeKey),
                  MatchesKey(feedback_data::kAnnotationHardwareBoardName),
                  MatchesKey(feedback_data::kAnnotationHardwareBoardRevision),
                  MatchesKey(feedback_data::kAnnotationHardwareProductLanguage),
                  MatchesKey(feedback_data::kAnnotationHardwareProductLocaleList),
                  MatchesKey(feedback_data::kAnnotationHardwareProductManufacturer),
                  MatchesKey(feedback_data::kAnnotationHardwareProductModel),
                  MatchesKey(feedback_data::kAnnotationHardwareProductName),
                  MatchesKey(feedback_data::kAnnotationHardwareProductRegulatoryDomain),
                  MatchesKey(feedback_data::kAnnotationHardwareProductSKU),
                  MatchesKey(feedback_data::kAnnotationSystemBootIdCurrent),
                  MatchesKey(feedback_data::kAnnotationSystemLastRebootReason),
                  MatchesKey(feedback_data::kAnnotationSystemUpdateChannelCurrent),
                  MatchesKey(feedback_data::kAnnotationSystemUpdateChannelTarget),
              }));
}

TEST_F(FeedbackIntegrationTest, DataProvider_GetSnapshot_CheckCobalt) {
  // We make sure the components serving the services GetSnapshot() connects to are up and running.
  WaitForLogger();
  WaitForChannelProvider();
  WaitForInspect();
  WaitForBoardProvider();
  WaitForProductProvider();
  WaitForProfileProvider();

  DataProviderSyncPtr data_provider;
  environment_services_->Connect(data_provider.NewRequest());

  Snapshot snapshot;
  ASSERT_EQ(data_provider->GetSnapshot(GetSnapshotParameters(), &snapshot), ZX_OK);

  ASSERT_FALSE(snapshot.IsEmpty());

  fake_cobalt_->RegisterExpectedEvent(cobalt::SnapshotGenerationFlow::kSuccess, 1);
  fake_cobalt_->RegisterExpectedEvent(cobalt::SnapshotVersion::kV_01, 1);

  EXPECT_TRUE(
      fake_cobalt_->MeetsExpectedEvents(fuchsia::metrics::test::LogMethod::LOG_INTEGER, false));
}

TEST_F(FeedbackIntegrationTest,
       DataProvider_GetSnapshot_NonPlatformAnnotationsFromComponentDataRegister) {
  // We make sure the components serving the services GetSnapshot() connects to are up and running.
  WaitForLogger();
  WaitForChannelProvider();
  WaitForInspect();
  WaitForBoardProvider();
  WaitForProductProvider();
  WaitForProfileProvider();

  DataProviderSyncPtr data_provider;
  environment_services_->Connect(data_provider.NewRequest());

  ComponentDataRegisterSyncPtr data_register;
  environment_services_->Connect(data_register.NewRequest());

  fuchsia::feedback::ComponentData extra_data;
  extra_data.set_namespace_("namespace");
  extra_data.set_annotations({
      {"k", "v"},
  });
  ASSERT_EQ(data_register->Upsert(std::move(extra_data)), ZX_OK);

  Snapshot snapshot;
  ASSERT_EQ(data_provider->GetSnapshot(GetSnapshotParameters(), &snapshot), ZX_OK);

  ASSERT_TRUE(snapshot.has_annotations());
  EXPECT_THAT(snapshot.annotations(), testing::Contains(MatchesAnnotation("namespace.k", "v")));
}

}  // namespace
}  // namespace forensics::feedback
