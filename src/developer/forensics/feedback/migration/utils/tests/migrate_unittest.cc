// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/migration/utils/migrate.h"

#include <lib/async/cpp/executor.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "fbl/unique_fd.h"
#include "src/developer/forensics/feedback/migration/utils/tests/directory_migrator_stubs.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"

namespace forensics::feedback {
namespace {

using LastRebootDirectoryMigrator =
    DirectoryMigratorStub<fuchsia::feedback::internal::LastRebootDirectoryMigrator>;

using LastRebootDirectoryMigratorClosesConnection =
    DirectoryMigratorStubClosesConnection<fuchsia::feedback::internal::LastRebootDirectoryMigrator>;

using CrashReportsDirectoryMigrator =
    DirectoryMigratorStub<fuchsia::feedback::internal::CrashReportsDirectoryMigrator>;

using CrashReportsDirectoryMigratorClosesConnection = DirectoryMigratorStubClosesConnection<
    fuchsia::feedback::internal::CrashReportsDirectoryMigrator>;

using FeedbackDataDirectoryMigrator =
    DirectoryMigratorStub<fuchsia::feedback::internal::FeedbackDataDirectoryMigrator>;

using FeedbackDataDirectoryMigratorClosesConnection = DirectoryMigratorStubClosesConnection<
    fuchsia::feedback::internal::FeedbackDataDirectoryMigrator>;

class MigrateTest : public UnitTestFixture {
 public:
  MigrateTest() : executor_(dispatcher()) {
    FX_CHECK(temp_dir_.NewTempDir(&to_data_path_));
    to_data_fd_ = fbl::unique_fd(open(to_data_path_.c_str(), O_DIRECTORY | O_RDWR, 0777));
    FX_CHECK(to_data_fd_.is_valid());

    FX_CHECK(temp_dir_.NewTempDir(&to_cache_path_));
    to_cache_fd_ = fbl::unique_fd(open(to_cache_path_.c_str(), O_DIRECTORY | O_RDWR, 0777));
    FX_CHECK(to_cache_fd_.is_valid());
  }

  ::fpromise::result<void, Error> MigrateLastReboot(
      const zx::duration timeout = zx::duration::infinite()) {
    std::optional<::fpromise::result<void, Error>> result;

    executor_.schedule_task(
        MigrateLastRebootData(dispatcher(), services(), to_data_fd_, to_cache_fd_, timeout)
            .then([&](::fpromise::result<void, Error>& r) { result = std::move(r); }));
    RunLoopUntilIdle();

    FX_CHECK(result);

    return *result;
  }

  ::fpromise::result<void, Error> MigrateCrashReports(
      const zx::duration timeout = zx::duration::infinite()) {
    std::optional<::fpromise::result<void, Error>> result;

    executor_.schedule_task(
        MigrateCrashReportsData(dispatcher(), services(), to_data_fd_, to_cache_fd_, timeout)
            .then([&](::fpromise::result<void, Error>& r) { result = std::move(r); }));
    RunLoopUntilIdle();

    FX_CHECK(result);

    return *result;
  }

  ::fpromise::result<void, Error> MigrateFeedbackData(
      const zx::duration timeout = zx::duration::infinite()) {
    std::optional<::fpromise::result<void, Error>> result;

    executor_.schedule_task(
        MigrateFeedbackDataData(dispatcher(), services(), to_data_fd_, to_cache_fd_, timeout)
            .then([&](::fpromise::result<void, Error>& r) { result = std::move(r); }));
    RunLoopUntilIdle();

    FX_CHECK(result);

    return *result;
  }

  std::string DataRoot() const { return to_data_path_; }
  std::string CacheRoot() const { return to_cache_path_; }

 private:
  async::Executor executor_;
  files::ScopedTempDir temp_dir_;

  std::string to_data_path_;
  fbl::unique_fd to_data_fd_;

  std::string to_cache_path_;
  fbl::unique_fd to_cache_fd_;
};

TEST_F(MigrateTest, MigrateLastRebootData) {
  files::ScopedTempDir data_dir;
  files::ScopedTempDir cache_dir;

  LastRebootDirectoryMigrator last_reboot_server(data_dir.path(), cache_dir.path());
  InjectServiceProvider(&last_reboot_server);

  EXPECT_TRUE(files::WriteFile(files::JoinPath(data_dir.path(), "data.txt"), "data"));
  EXPECT_TRUE(files::WriteFile(files::JoinPath(cache_dir.path(), "cache.txt"), "cache"));

  EXPECT_TRUE(MigrateLastReboot().is_ok());

  // The original files should be deleted.
  EXPECT_FALSE(files::IsFile(files::JoinPath(data_dir.path(), "data.txt")));
  EXPECT_FALSE(files::IsFile(files::JoinPath(cache_dir.path(), "cache.txt")));

  // The new files should have the content of the original files.
  EXPECT_TRUE(files::IsFile(files::JoinPath(DataRoot(), "data.txt")));
  EXPECT_TRUE(files::IsFile(files::JoinPath(CacheRoot(), "cache.txt")));

  std::string content;

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(DataRoot(), "data.txt"), &content));
  EXPECT_EQ(content, "data");

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(CacheRoot(), "cache.txt"), &content));
  EXPECT_EQ(content, "cache");
}

TEST_F(MigrateTest, MigrateLastRebootDataConnectionErrors) {
  LastRebootDirectoryMigratorClosesConnection last_reboot_server;
  InjectServiceProvider(&last_reboot_server);

  EXPECT_FALSE(MigrateLastReboot().is_ok());
}

TEST_F(MigrateTest, MigrateCrashReportsData) {
  files::ScopedTempDir data_dir;
  files::ScopedTempDir cache_dir;

  CrashReportsDirectoryMigrator last_reboot_server(data_dir.path(), cache_dir.path());
  InjectServiceProvider(&last_reboot_server);

  EXPECT_TRUE(files::WriteFile(files::JoinPath(data_dir.path(), "data.txt"), "data"));
  EXPECT_TRUE(files::WriteFile(files::JoinPath(cache_dir.path(), "cache.txt"), "cache"));

  EXPECT_TRUE(MigrateCrashReports().is_ok());

  // The original files should be deleted.
  EXPECT_FALSE(files::IsFile(files::JoinPath(data_dir.path(), "data.txt")));
  EXPECT_FALSE(files::IsFile(files::JoinPath(cache_dir.path(), "cache.txt")));

  // The new files should have the content of the original files.
  EXPECT_TRUE(files::IsFile(files::JoinPath(DataRoot(), "data.txt")));
  EXPECT_TRUE(files::IsFile(files::JoinPath(CacheRoot(), "cache.txt")));

  std::string content;

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(DataRoot(), "data.txt"), &content));
  EXPECT_EQ(content, "data");

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(CacheRoot(), "cache.txt"), &content));
  EXPECT_EQ(content, "cache");
}

TEST_F(MigrateTest, MigrateCrashReportsDataConnectionErrors) {
  CrashReportsDirectoryMigratorClosesConnection last_reboot_server;
  InjectServiceProvider(&last_reboot_server);

  EXPECT_FALSE(MigrateCrashReports().is_ok());
}

TEST_F(MigrateTest, MigrateFeedbackDataData) {
  files::ScopedTempDir data_dir;
  files::ScopedTempDir cache_dir;

  FeedbackDataDirectoryMigrator feedback_data_server(data_dir.path(), cache_dir.path());
  InjectServiceProvider(&feedback_data_server);

  EXPECT_TRUE(files::WriteFile(files::JoinPath(data_dir.path(), "data.txt"), "data"));
  EXPECT_TRUE(files::WriteFile(files::JoinPath(cache_dir.path(), "cache.txt"), "cache"));

  EXPECT_TRUE(MigrateFeedbackData().is_ok());

  // The original files should be deleted.
  EXPECT_FALSE(files::IsFile(files::JoinPath(data_dir.path(), "data.txt")));
  EXPECT_FALSE(files::IsFile(files::JoinPath(cache_dir.path(), "cache.txt")));

  // The new files should have the content of the original files.
  EXPECT_TRUE(files::IsFile(files::JoinPath(DataRoot(), "data.txt")));
  EXPECT_TRUE(files::IsFile(files::JoinPath(CacheRoot(), "cache.txt")));

  std::string content;

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(DataRoot(), "data.txt"), &content));
  EXPECT_EQ(content, "data");

  ASSERT_TRUE(files::ReadFileToString(files::JoinPath(CacheRoot(), "cache.txt"), &content));
  EXPECT_EQ(content, "cache");
}

TEST_F(MigrateTest, MigrateFeedbackDataDataConnectionErrors) {
  FeedbackDataDirectoryMigratorClosesConnection feedback_data_server;
  InjectServiceProvider(&feedback_data_server);

  EXPECT_FALSE(MigrateFeedbackData().is_ok());
}

}  // namespace
}  // namespace forensics::feedback
