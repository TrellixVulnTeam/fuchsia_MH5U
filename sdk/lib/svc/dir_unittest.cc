// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/svc/dir.h"

#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/channel.h>

#include <thread>

#include "lib/gtest/real_loop_fixture.h"

namespace svc {
namespace {

static void connect(void* context, const char* service_name, zx_handle_t service_request) {
  EXPECT_EQ(std::string("foobar"), service_name);
  zx::channel binding(service_request);
  zx_signals_t observed;
  EXPECT_EQ(ZX_OK, binding.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), &observed));
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL,
            binding.read(ZX_CHANNEL_READ_MAY_DISCARD, nullptr, nullptr, 0, 0, nullptr, nullptr));
  EXPECT_EQ(ZX_OK, binding.write(0, "ok", 2, 0, 0));
}

using ServiceTest = ::gtest::RealLoopFixture;

TEST_F(ServiceTest, Control) {
  zx::channel dir, dir_request;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &dir, &dir_request));

  std::thread child([this, dir_request = std::move(dir_request)]() mutable {
    svc_dir_t* dir = nullptr;
    EXPECT_EQ(ZX_OK, svc_dir_create(dispatcher(), dir_request.release(), &dir));
    EXPECT_EQ(ZX_OK, svc_dir_add_service(dir, "svc", "foobar", nullptr, connect));
    EXPECT_EQ(ZX_OK, svc_dir_add_service(dir, "svc", "baz", nullptr, nullptr));
    EXPECT_EQ(ZX_ERR_ALREADY_EXISTS, svc_dir_add_service(dir, "svc", "baz", nullptr, nullptr));
    EXPECT_EQ(ZX_OK, svc_dir_remove_service(dir, "svc", "baz"));
    EXPECT_EQ(ZX_OK, svc_dir_add_service(dir, "another", "qux", nullptr, nullptr));

    RunLoop();

    svc_dir_destroy(dir);
  });

  // Verify that we can connect to a foobar service and get a response.
  zx::channel svc, request;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "svc/foobar", request.release());
  EXPECT_EQ(ZX_OK, svc.write(0, "hello", 5, 0, 0));
  zx_signals_t observed;
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), &observed));
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL,
            svc.read(ZX_CHANNEL_READ_MAY_DISCARD, nullptr, nullptr, 0, 0, nullptr, nullptr));

  // Verify that connection to a removed service fails.
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "svc/baz", request.release());
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &observed));

  // Shutdown the service thread.
  QuitLoop();
  child.join();

  // Verify that connection fails after svc_dir_destroy().
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "svc/foobar", request.release());
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &observed));
}

TEST_F(ServiceTest, PublishLegacyService) {
  zx::channel dir, dir_request;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &dir, &dir_request));

  std::thread child([this, dir_request = std::move(dir_request)]() mutable {
    svc_dir_t* dir = nullptr;
    EXPECT_EQ(ZX_OK, svc_dir_create(dispatcher(), dir_request.release(), &dir));
    EXPECT_EQ(ZX_OK, svc_dir_add_service(dir, nullptr, "foobar", nullptr, connect));
    EXPECT_EQ(ZX_OK, svc_dir_add_service(dir, nullptr, "baz", nullptr, connect));
    EXPECT_EQ(ZX_OK, svc_dir_remove_service(dir, nullptr, "baz"));

    RunLoop();

    svc_dir_destroy(dir);
  });

  // Verify that we can connect to a foobar service and get a response.
  zx::channel svc, request;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "foobar", request.release());
  EXPECT_EQ(ZX_OK, svc.write(0, "hello", 5, 0, 0));
  zx_signals_t observed;
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), &observed));
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL,
            svc.read(ZX_CHANNEL_READ_MAY_DISCARD, nullptr, nullptr, 0, 0, nullptr, nullptr));

  // Verify that connection to a removed service fails.
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "baz", request.release());
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &observed));

  // Shutdown the service thread.
  QuitLoop();
  child.join();

  // Verify that connection fails after svc_dir_destroy().
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "foobar", request.release());
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &observed));
}

TEST_F(ServiceTest, ConnectsByPath) {
  zx::channel dir, dir_request;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &dir, &dir_request));

  std::thread child([this, dir_request = std::move(dir_request)]() mutable {
    svc_dir_t* dir = nullptr;
    ASSERT_EQ(ZX_OK, svc_dir_create(dispatcher(), dir_request.release(), &dir));
    ASSERT_EQ(ZX_OK, svc_dir_add_service_by_path(dir, "svc/fuchsia.logger.LogSink/default",
                                                 "foobar", nullptr, connect));

    RunLoop();

    ASSERT_EQ(svc_dir_destroy(dir), ZX_OK);
  });

  // Verify that we can connect to svc/fuchsia.logger.LogSink/default/foobar
  // and get a response.
  zx::channel svc, request;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &svc, &request));
  fdio_service_connect_at(dir.get(), "svc/fuchsia.logger.LogSink/default/foobar",
                          request.release());
  EXPECT_EQ(ZX_OK, svc.write(0, "hello", 5, 0, 0));
  zx_signals_t observed;
  EXPECT_EQ(ZX_OK, svc.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), &observed));
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL,
            svc.read(ZX_CHANNEL_READ_MAY_DISCARD, nullptr, nullptr, 0, 0, nullptr, nullptr));

  // Shutdown the service thread.
  QuitLoop();
  child.join();
}

TEST_F(ServiceTest, RejectsMalformedPaths) {
  zx::channel _directory, dir_request;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &_directory, &dir_request));

  svc_dir_t* dir = nullptr;
  ASSERT_EQ(ZX_OK, svc_dir_create(dispatcher(), dir_request.release(), &dir));

  // The following paths should all fail.
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "/", "foobar", nullptr, connect), ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "/svc", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "/svc//foo", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "svc/", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, ".", "foobar", nullptr, connect), ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "..", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "...", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(svc_dir_add_service_by_path(dir, "svc/..", "foobar", nullptr, connect),
            ZX_ERR_INVALID_ARGS);

  // Cleanup resources.
  ASSERT_EQ(svc_dir_destroy(dir), ZX_OK);
}

}  // namespace
}  // namespace svc
