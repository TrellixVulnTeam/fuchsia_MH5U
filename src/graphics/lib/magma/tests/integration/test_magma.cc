// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include <array>
#include <thread>
#include <unordered_set>

#if defined(__Fuchsia__)
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmar.h>

#include <filesystem>

#include "fidl/fuchsia.gpu.magma/cpp/wire.h"
#include "fuchsia/sysmem/cpp/fidl.h"
#include "magma_sysmem.h"
#include "platform_logger.h"
#include "platform_trace_provider.h"
#endif

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "helper/magma_map_cpu.h"
#include "magma.h"
#include "magma_common_defs.h"
#include "magma_intel_gen_defs.h"
#include "src/graphics/drivers/msd-arm-mali/include/magma_arm_mali_types.h"
#include "src/graphics/drivers/msd-arm-mali/include/magma_vendor_queries.h"

extern "C" {
#include "test_magma.h"
}

namespace {

inline uint64_t page_size() { return sysconf(_SC_PAGESIZE); }

inline constexpr int64_t ms_to_ns(int64_t ms) { return ms * 1000000ull; }

static inline uint32_t to_uint32(uint64_t val) {
  assert(val <= std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(val);
}

static uint64_t clock_gettime_monotonic_raw() {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

  return 1000000000ull * ts.tv_sec + ts.tv_nsec;
}

}  // namespace

#if defined(__Fuchsia__)
class FakePerfCountAccessServer
    : public fidl::WireServer<fuchsia_gpu_magma::PerformanceCounterAccess> {
  void GetPerformanceCountToken(GetPerformanceCountTokenRequestView request,
                                GetPerformanceCountTokenCompleter::Sync& completer) override {
    zx::event event;
    zx::event::create(0, &event);
    completer.Reply(std::move(event));
  }
};
#endif

class TestConnection {
 public:
  static constexpr const char* kDevicePathFuchsia = "/dev/class/gpu";
  static constexpr const char* kDeviceNameLinux = "/dev/dri/renderD128";
  static constexpr const char* kDeviceNameVirt = "/dev/magma0";

#if defined(VIRTMAGMA)
  static std::string device_name() { return kDeviceNameVirt; }
#elif defined(__linux__)
  static std::string device_name() { return kDeviceNameLinux; }
#elif defined(__Fuchsia__)
  static std::string device_name() {
    std::string name;
    magma_device_t device;
    if (OpenFuchsiaDevice(&name, &device)) {
      magma_device_release(device);
      return name;
    }
    return "";
  }

  static bool OpenFuchsiaDevice(std::string* device_name_out, magma_device_t* device_out) {
    std::string device_name;
    magma_device_t device = 0;

    for (auto& p : std::filesystem::directory_iterator(kDevicePathFuchsia)) {
      EXPECT_FALSE(device) << " More than one GPU device found, specify --vendor-id";
      if (device) {
        magma_device_release(device);
        return false;
      }

      zx::channel server_end, client_end;
      zx::channel::create(0, &server_end, &client_end);

      zx_status_t zx_status = fdio_service_connect(p.path().c_str(), server_end.release());
      EXPECT_EQ(ZX_OK, zx_status);
      if (zx_status != ZX_OK)
        return false;

      magma_status_t status = magma_device_import(client_end.release(), &device);
      EXPECT_EQ(MAGMA_STATUS_OK, status);
      if (status != MAGMA_STATUS_OK)
        return false;

      device_name = p.path();

      if (gVendorId) {
        uint64_t vendor_id;
        status = magma_query2(device, MAGMA_QUERY_VENDOR_ID, &vendor_id);
        EXPECT_EQ(MAGMA_STATUS_OK, status);
        if (status != MAGMA_STATUS_OK)
          return false;

        if (vendor_id == gVendorId) {
          break;
        } else {
          magma_device_release(device);
          device = 0;
        }
      }
    }

    if (!device)
      return false;

    *device_name_out = device_name;
    *device_out = device;
    return true;
  }

#else
#error Unimplemented
#endif

  static bool is_virtmagma() { return device_name() == kDeviceNameVirt; }

  TestConnection() {
#if defined(__Fuchsia__)
    std::string device;
    EXPECT_TRUE(OpenFuchsiaDevice(&device, &device_));

#elif defined(__linux__)
    std::string device = device_name();
    EXPECT_FALSE(device.empty()) << " No GPU device";
    if (device.empty())
      return;

    int fd = open(device.c_str(), O_RDWR);
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      EXPECT_EQ(MAGMA_STATUS_OK, magma_device_import(fd, &device_));
    }
#else
#error Unimplemented
#endif
    if (device_) {
      magma_create_connection2(device_, &connection_);
    }
  }

  ~TestConnection() {
    if (connection_)
      magma_release_connection(connection_);
    if (device_)
      magma_device_release(device_);
    if (fd_ >= 0)
      close(fd_);
  }

  int fd() { return fd_; }

  magma_connection_t connection() { return connection_; }

  void Connection() { ASSERT_TRUE(connection_); }

  void Context() {
    ASSERT_TRUE(connection_);

    uint32_t context_id[2];
    EXPECT_EQ(MAGMA_STATUS_OK, magma_create_context(connection_, &context_id[0]));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    EXPECT_EQ(MAGMA_STATUS_OK, magma_create_context(connection_, &context_id[1]));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id[0]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id[1]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    // Already released
    magma_release_context(connection_, context_id[1]);
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_error(connection_));
  }

  void NotificationChannelHandle() {
    ASSERT_TRUE(connection_);

    uint32_t handle = magma_get_notification_channel_handle(connection_);
    EXPECT_NE(0u, handle);

    uint32_t handle2 = magma_get_notification_channel_handle(connection_);
    EXPECT_EQ(handle, handle2);
  }

  void ReadNotificationChannel() {
    ASSERT_TRUE(connection_);

    std::array<unsigned char, 1024> buffer;
    uint64_t buffer_size = ~0;
    magma_bool_t more_data = true;
    magma_status_t status = magma_read_notification_channel2(
        connection_, buffer.data(), buffer.size(), &buffer_size, &more_data);
    EXPECT_EQ(MAGMA_STATUS_OK, status);
    EXPECT_EQ(0u, buffer_size);
    EXPECT_EQ(false, more_data);
  }

  void Buffer() {
    ASSERT_TRUE(connection_);

    uint64_t size = page_size();
    uint64_t actual_size;
    magma_buffer_t buffer = 0;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &actual_size, &buffer));
    EXPECT_GE(size, actual_size);
    EXPECT_NE(buffer, 0u);

    magma_release_buffer(connection_, buffer);
  }

  void BufferMap() {
    ASSERT_TRUE(connection_);

    uint64_t size = page_size();
    uint64_t actual_size;
    magma_buffer_t buffer = 0;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &actual_size, &buffer));
    EXPECT_NE(buffer, 0u);

    constexpr uint64_t kGpuAddress = 0x1000;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_map_buffer_gpu(connection_, buffer, 0, size / page_size(),
                                                    kGpuAddress, MAGMA_GPU_MAP_FLAG_READ));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    {
      uint64_t vendor_id;
      ASSERT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_VENDOR_ID, &vendor_id));
      // Unmap not implemented on Intel
      if (vendor_id != 0x8086) {
        magma_unmap_buffer_gpu(connection_, buffer, kGpuAddress);
        EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));
      }
    }

    // Invalid page offset, remote error
    constexpr uint64_t kInvalidPageOffset = 1024;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_map_buffer_gpu(connection_, buffer, kInvalidPageOffset, 0,
                                                    size / page_size(), MAGMA_GPU_MAP_FLAG_READ));
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_error(connection_));

    magma_release_buffer(connection_, buffer);
  }

  void BufferExport(uint32_t* handle_out, uint64_t* id_out) {
    ASSERT_TRUE(connection_);

    uint64_t size = page_size();
    magma_buffer_t buffer;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &size, &buffer));

    *id_out = magma_get_buffer_id(buffer);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_export(connection_, buffer, handle_out));

    magma_release_buffer(connection_, buffer);
  }

  void BufferImportInvalid() {
    ASSERT_TRUE(connection_);

    constexpr uint32_t kInvalidHandle = 0xabcd1234;
    magma_buffer_t buffer;
#if defined(__Fuchsia__)
    constexpr magma_status_t kExpectedStatus = MAGMA_STATUS_INVALID_ARGS;
#elif defined(__linux__)
    constexpr magma_status_t kExpectedStatus = MAGMA_STATUS_INTERNAL_ERROR;
#endif
    ASSERT_EQ(kExpectedStatus, magma_import(connection_, kInvalidHandle, &buffer));
  }

  void BufferImport(uint32_t handle, uint64_t id) {
    ASSERT_TRUE(connection_);

    magma_buffer_t buffer;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_import(connection_, handle, &buffer));
    EXPECT_EQ(magma_get_buffer_id(buffer), id);
    magma_release_buffer(connection_, buffer);
  }

  static void BufferImportExport(TestConnection* test1, TestConnection* test2) {
    if (is_virtmagma())
      GTEST_SKIP();  // TODO(fxbug.dev/13278)

    uint32_t handle;
    uint64_t id;
    test1->BufferExport(&handle, &id);
    test2->BufferImport(handle, id);
  }

  static magma_status_t wait_all(std::vector<magma_poll_item_t>& items, int64_t timeout_ns) {
    int64_t remaining_ns = timeout_ns;

    for (size_t i = 0; i < items.size(); i++) {
      if (remaining_ns < 0)
        remaining_ns = 0;

      auto start = std::chrono::steady_clock::now();

      magma_status_t status = magma_poll(&items[i], 1, remaining_ns);
      if (status != MAGMA_STATUS_OK)
        return status;

      remaining_ns -= std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    }
    return MAGMA_STATUS_OK;
  }

  void Semaphore(uint32_t count) {
    ASSERT_TRUE(connection_);

    std::vector<magma_poll_item_t> items(count);

    for (uint32_t i = 0; i < count; i++) {
      items[i] = {.type = MAGMA_POLL_TYPE_SEMAPHORE, .condition = MAGMA_POLL_CONDITION_SIGNALED};
      ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &items[i].semaphore));
      EXPECT_NE(0u, magma_get_semaphore_id(items[i].semaphore));
    }

    magma_signal_semaphore(items[0].semaphore);

    constexpr uint32_t kTimeoutMs = 100;
    constexpr uint64_t kNsPerMs = 1000000;

    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(count == 1 ? MAGMA_STATUS_OK : MAGMA_STATUS_TIMED_OUT,
              wait_all(items, kNsPerMs * kTimeoutMs));
    if (count > 1) {
      // Subtract to allow for rounding errors in magma_wait_semaphores time calculations
      EXPECT_LE(kTimeoutMs - count, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    }

    for (uint32_t i = 1; i < items.size(); i++) {
      magma_signal_semaphore(items[i].semaphore);
    }

    EXPECT_EQ(MAGMA_STATUS_OK, wait_all(items, 0));

    for (uint32_t i = 0; i < items.size(); i++) {
      magma_reset_semaphore(items[i].semaphore);
    }

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, wait_all(items, 0));

    // Wait for one
    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), to_uint32(items.size()), kNsPerMs * kTimeoutMs));

    // Subtract to allow for rounding errors in magma_wait_semaphores time calculations
    EXPECT_LE(kTimeoutMs - count, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());

    magma_signal_semaphore(items.back().semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), to_uint32(items.size()), 0));

    magma_reset_semaphore(items.back().semaphore);

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), to_uint32(items.size()), 0));

    for (auto& item : items) {
      magma_release_semaphore(connection_, item.semaphore);
    }
  }

  void PollWithNotificationChannel(uint32_t semaphore_count) {
    ASSERT_TRUE(connection_);

    std::vector<magma_poll_item_t> items;

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_semaphore_t semaphore;
      ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore));

      items.push_back({.semaphore = semaphore,
                       .type = MAGMA_POLL_TYPE_SEMAPHORE,
                       .condition = MAGMA_POLL_CONDITION_SIGNALED});
    }

    items.push_back({
        .handle = magma_get_notification_channel_handle(connection_),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    constexpr int64_t kTimeoutNs = ms_to_ns(100);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), to_uint32(items.size()), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    if (semaphore_count == 0)
      return;

    magma_signal_semaphore(items[0].semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), to_uint32(items.size()), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, 0u);

    magma_reset_semaphore(items[0].semaphore);

    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), to_uint32(items.size()), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_signal_semaphore(items[i].semaphore);
    }

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), to_uint32(items.size()), 0));

    for (uint32_t i = 0; i < items.size(); i++) {
      if (i < items.size() - 1) {
        EXPECT_EQ(items[i].result, items[i].condition);
      } else {
        // Notification channel
        EXPECT_EQ(items[i].result, 0u);
      }
    }

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_release_semaphore(connection_, items[i].semaphore);
    }
  }

  void PollWithTestChannel() {
#ifdef __Fuchsia__
    ASSERT_TRUE(connection_);

    zx::channel local, remote;
    ASSERT_EQ(ZX_OK, zx::channel::create(0 /* flags */, &local, &remote));

    magma_semaphore_t semaphore;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore));

    std::vector<magma_poll_item_t> items;
    items.push_back({.semaphore = semaphore,
                     .type = MAGMA_POLL_TYPE_SEMAPHORE,
                     .condition = MAGMA_POLL_CONDITION_SIGNALED});
    items.push_back({
        .handle = local.get(),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    constexpr int64_t kTimeoutNs = ms_to_ns(100);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), static_cast<uint32_t>(items.size()), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    magma_signal_semaphore(semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), static_cast<uint32_t>(items.size()), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, 0u);

    magma_reset_semaphore(semaphore);

    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), static_cast<uint32_t>(items.size()), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    uint32_t dummy;
    EXPECT_EQ(ZX_OK, remote.write(0 /* flags */, &dummy, sizeof(dummy), nullptr /* handles */,
                                  0 /* num_handles*/));

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), static_cast<uint32_t>(items.size()), 0));
    EXPECT_EQ(items[0].result, 0u);
    EXPECT_EQ(items[1].result, items[1].condition);

    magma_signal_semaphore(semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), static_cast<uint32_t>(items.size()), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, items[1].condition);

    magma_release_semaphore(connection_, semaphore);
#else
    GTEST_SKIP();
#endif
  }

  void PollChannelClosed() {
#ifdef __Fuchsia__
    ASSERT_TRUE(connection_);

    zx::channel local, remote;
    ASSERT_EQ(ZX_OK, zx::channel::create(0 /* flags */, &local, &remote));

    std::vector<magma_poll_item_t> items;
    items.push_back({
        .handle = local.get(),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_poll(items.data(), static_cast<uint32_t>(items.size()), 0));

    remote.reset();
    EXPECT_EQ(MAGMA_STATUS_CONNECTION_LOST,
              magma_poll(items.data(), static_cast<uint32_t>(items.size()), 0));
#else
    GTEST_SKIP();
#endif
  }

  void SemaphoreExport(magma_handle_t* handle_out, uint64_t* id_out) {
    ASSERT_TRUE(connection_);

    magma_semaphore_t semaphore;
    ASSERT_EQ(magma_create_semaphore(connection_, &semaphore), MAGMA_STATUS_OK);
    *id_out = magma_get_semaphore_id(semaphore);
    EXPECT_EQ(magma_export_semaphore(connection_, semaphore, handle_out), MAGMA_STATUS_OK);
    magma_release_semaphore(connection_, semaphore);
  }

  void SemaphoreImport(magma_handle_t handle, uint64_t expected_id) {
    ASSERT_TRUE(connection_);

    magma_semaphore_t semaphore;
    ASSERT_EQ(magma_import_semaphore(connection_, handle, &semaphore), MAGMA_STATUS_OK);
    EXPECT_EQ(magma_get_semaphore_id(semaphore), expected_id);
    magma_release_semaphore(connection_, semaphore);
  }

  static void SemaphoreImportExport(TestConnection* test1, TestConnection* test2) {
    magma_handle_t handle;
    uint64_t id;
    test1->SemaphoreExport(&handle, &id);
    test2->SemaphoreImport(handle, id);
  }

  void ImmediateCommands() {
    if (TestConnection::is_virtmagma())
      GTEST_SKIP();

    ASSERT_TRUE(connection_);

    uint32_t context_id;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_create_context(connection_, &context_id));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_inline_command_buffer inline_command_buffer{};
    EXPECT_EQ(MAGMA_STATUS_OK, magma_execute_immediate_commands2(connection_, context_id, 0,
                                                                 &inline_command_buffer));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));
  }

  void Sysmem(bool use_format_modifier) {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    magma_sysmem_connection_t connection;
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK,
              fdio_service_connect("/svc/fuchsia.sysmem.Allocator", server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_sysmem_connection_import(local_endpoint.release(), &connection));

    magma_buffer_collection_t collection;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_collection_import(connection, ZX_HANDLE_INVALID, &collection));

    magma_buffer_format_constraints_t buffer_constraints{};

    buffer_constraints.count = 1;
    buffer_constraints.usage = 0;
    buffer_constraints.secure_permitted = false;
    buffer_constraints.secure_required = false;
    buffer_constraints.cpu_domain_supported = true;
    magma_sysmem_buffer_constraints_t constraints;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_constraints_create(connection, &buffer_constraints, &constraints));

    magma_buffer_format_additional_constraints_t additional{
        .min_buffer_count_for_camping = 1,
        .min_buffer_count_for_dedicated_slack = 1,
        .min_buffer_count_for_shared_slack = 1};
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_constraints_add_additional(connection, constraints, &additional));

    // Create a set of basic 512x512 RGBA image constraints.
    magma_image_format_constraints_t image_constraints{};
    image_constraints.image_format = MAGMA_FORMAT_R8G8B8A8;
    image_constraints.has_format_modifier = use_format_modifier;
    image_constraints.format_modifier = use_format_modifier ? MAGMA_FORMAT_MODIFIER_LINEAR : 0;
    image_constraints.width = 512;
    image_constraints.height = 512;
    image_constraints.layers = 1;
    image_constraints.bytes_per_row_divisor = 1;
    image_constraints.min_bytes_per_row = 0;

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_constraints_set_format(connection, constraints, 0, &image_constraints));

    uint32_t color_space_in = MAGMA_COLORSPACE_SRGB;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_constraints_set_colorspaces(connection, constraints, 0,
                                                                        1, &color_space_in));

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_collection_set_constraints(connection, collection, constraints));

    // Buffer should be allocated now.
    magma_buffer_format_description_t description;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_sysmem_get_description_from_collection(connection, collection, &description));

    uint32_t expected_buffer_count = additional.min_buffer_count_for_camping +
                                     additional.min_buffer_count_for_dedicated_slack +
                                     additional.min_buffer_count_for_shared_slack;
    uint32_t buffer_count;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_count(description, &buffer_count));
    EXPECT_EQ(expected_buffer_count, buffer_count);
    magma_bool_t is_secure;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_is_secure(description, &is_secure));
    EXPECT_FALSE(is_secure);

    uint32_t format;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_format(description, &format));
    EXPECT_EQ(MAGMA_FORMAT_R8G8B8A8, format);
    uint32_t color_space = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_color_space(description, &color_space));
    EXPECT_EQ(MAGMA_COLORSPACE_SRGB, color_space);

    magma_bool_t has_format_modifier;
    uint64_t format_modifier;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_format_modifier(description, &has_format_modifier,
                                                                &format_modifier));
    if (has_format_modifier) {
      EXPECT_EQ(MAGMA_FORMAT_MODIFIER_LINEAR, format_modifier);
    }

    magma_image_plane_t planes[4];
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 512u, 512u, planes));
    EXPECT_EQ(512 * 4u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 512, 512, planes));
    EXPECT_EQ(512 * 4u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);

    magma_buffer_format_description_release(description);

    magma_handle_t handle;
    uint32_t offset;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_sysmem_get_buffer_handle_from_collection(
                                   connection, collection, 0, &handle, &offset));
    EXPECT_EQ(ZX_OK, zx_handle_close(handle));

    magma_buffer_collection_release(connection, collection);
    magma_buffer_constraints_release(connection, constraints);
    magma_sysmem_connection_release(connection);
#endif
  }

  void TracingInit() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.tracing.provider.Registry",
                                          server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_initialize_tracing(local_endpoint.release()));

    if (magma::PlatformTraceProvider::Get())
      EXPECT_TRUE(magma::PlatformTraceProvider::Get()->IsInitialized());
#endif
  }

  void LoggingInit() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK,
              fdio_service_connect("/svc/fuchsia.logger.LogSink", server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_initialize_logging(local_endpoint.release()));
    EXPECT_TRUE(magma::PlatformLogger::IsInitialized());
    MAGMA_LOG(INFO, "LoggingInit test complete");
#endif
  }

  void GetDeviceIdImported() {
    ASSERT_TRUE(device_);

    uint64_t device_id = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_DEVICE_ID, &device_id));
    EXPECT_NE(0u, device_id);
  }

  void GetVendorIdImported() {
    ASSERT_TRUE(device_);

    uint64_t vendor_id = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_VENDOR_ID, &vendor_id));
    EXPECT_NE(0u, vendor_id);
  }

  void QueryReturnsBufferImported(bool leaky = false, bool check_clock = false) {
    ASSERT_TRUE(device_);
    ASSERT_TRUE(connection_);

    constexpr uint32_t kVendorIdIntel = 0x8086;
    constexpr uint32_t kVendorIdArm = 0x13B5;

    uint64_t query_id = 0;
    uint64_t vendor_id;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_VENDOR_ID, &vendor_id));
    switch (vendor_id) {
      case kVendorIdIntel:
        query_id = kMagmaIntelGenQueryTimestamp;
        break;
      case kVendorIdArm:
        query_id = kMsdArmVendorQueryDeviceTimestamp;
        break;
      default:
        GTEST_SKIP();
    }

    uint64_t before_ns = clock_gettime_monotonic_raw();

    uint32_t buffer_handle = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_query_returns_buffer2(device_, query_id, &buffer_handle));

    uint64_t after_ns = clock_gettime_monotonic_raw();

    ASSERT_NE(0u, buffer_handle);

    struct magma_intel_gen_timestamp_query intel_timestamp_query;
    struct magma_arm_mali_device_timestamp_return arm_timestamp_return;

#if defined(__Fuchsia__)
    zx_vaddr_t zx_vaddr;
    {
      zx::vmo vmo(buffer_handle);

      ASSERT_EQ(ZX_OK, zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                                  0,  // vmar_offset,
                                                  vmo, 0 /*offset*/, page_size(), &zx_vaddr));
    }

    memcpy(&intel_timestamp_query, reinterpret_cast<void*>(zx_vaddr),
           sizeof(intel_timestamp_query));
    memcpy(&arm_timestamp_return, reinterpret_cast<void*>(zx_vaddr), sizeof(arm_timestamp_return));

    if (!leaky) {
      EXPECT_EQ(ZX_OK, zx::vmar::root_self()->unmap(zx_vaddr, page_size()));
    }

#elif defined(__linux__)
    void* addr;
    {
      int fd = buffer_handle;

      addr = mmap(nullptr, page_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 /*offset*/);
      ASSERT_NE(MAP_FAILED, addr);

      close(fd);
    }

    memcpy(&intel_timestamp_query, addr, sizeof(intel_timestamp_query));
    memcpy(&arm_timestamp_return, addr, sizeof(arm_timestamp_return));

    if (!leaky) {
      munmap(addr, page_size());
    }
#endif

    if (!check_clock)
      return;

    // Check that clock_gettime is synchronized between client and driver.
    // Required for clients using VK_EXT_calibrated_timestamps.
    if (vendor_id == kVendorIdIntel) {
      EXPECT_LT(before_ns, intel_timestamp_query.monotonic_raw_timestamp[0]);
      EXPECT_LT(intel_timestamp_query.monotonic_raw_timestamp[0],
                intel_timestamp_query.monotonic_raw_timestamp[1]);
      EXPECT_LT(intel_timestamp_query.monotonic_raw_timestamp[1], after_ns);
    } else if (vendor_id == kVendorIdArm) {
      EXPECT_LT(before_ns, arm_timestamp_return.monotonic_raw_timestamp_before);
      EXPECT_LT(arm_timestamp_return.monotonic_raw_timestamp_before,
                arm_timestamp_return.monotonic_raw_timestamp_after);
      EXPECT_LT(arm_timestamp_return.monotonic_raw_timestamp_after, after_ns);
    }
  }

  void QueryTestRestartSupported() {
    ASSERT_TRUE(device_);

    uint64_t is_supported = 0;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_query2(device_, MAGMA_QUERY_IS_TEST_RESTART_SUPPORTED, &is_supported));
    // We don't care about the value of |is_supported|, just that the query returns ok.
  }

#if defined(__Fuchsia__)
  void CheckAccessWithInvalidToken(magma_status_t expected_result) {
    FakePerfCountAccessServer server;
    async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
    ASSERT_EQ(ZX_OK, loop.StartThread("server-loop"));

    zx::channel server_endpoint, client_endpoint;
    ASSERT_EQ(ZX_OK, zx::channel::create(0, &server_endpoint, &client_endpoint));
    fidl::BindServer(loop.dispatcher(), std::move(server_endpoint), &server);

    EXPECT_EQ(expected_result, magma_connection_enable_performance_counter_access(
                                   connection_, client_endpoint.release()));
  }
#endif

  void EnablePerformanceCounters() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    CheckAccessWithInvalidToken(MAGMA_STATUS_ACCESS_DENIED);

    bool success = false;
    for (auto& p : std::filesystem::directory_iterator("/dev/class/gpu-performance-counters")) {
      zx::channel server_end, client_end;
      zx::channel::create(0, &server_end, &client_end);

      zx_status_t zx_status = fdio_service_connect(p.path().c_str(), server_end.release());
      EXPECT_EQ(ZX_OK, zx_status);
      magma_status_t status =
          magma_connection_enable_performance_counter_access(connection_, client_end.release());
      EXPECT_TRUE(status == MAGMA_STATUS_OK || status == MAGMA_STATUS_ACCESS_DENIED);
      if (status == MAGMA_STATUS_OK) {
        success = true;
      }
    }
    EXPECT_TRUE(success);
    // Access should remain enabled even though an invalid token is used.
    CheckAccessWithInvalidToken(MAGMA_STATUS_OK);
#endif
  }

  void DisabledPerformanceCounters() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    uint64_t counter = 5;
    magma_semaphore_t semaphore;
    ASSERT_EQ(magma_create_semaphore(connection_, &semaphore), MAGMA_STATUS_OK);
    uint64_t size = page_size();
    magma_buffer_t buffer;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &size, &buffer));

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_connection_enable_performance_counters(connection_, &counter, 1));
    EXPECT_EQ(MAGMA_STATUS_ACCESS_DENIED, magma_get_error(connection_));

    magma_perf_count_pool_t pool;
    magma_handle_t handle;
    EXPECT_EQ(MAGMA_STATUS_CONNECTION_LOST,
              magma_connection_create_performance_counter_buffer_pool(connection_, &pool, &handle));

    magma_release_buffer(connection_, buffer);
    magma_release_semaphore(connection_, semaphore);
#endif
  }

 private:
  int fd_ = -1;
  magma_device_t device_ = 0;
  magma_connection_t connection_ = 0;
};

class TestConnectionWithContext : public TestConnection {
 public:
  TestConnectionWithContext() {
    if (connection()) {
      EXPECT_EQ(MAGMA_STATUS_OK, magma_create_context(connection(), &context_id_));
    }
  }

  ~TestConnectionWithContext() {
    if (connection()) {
      magma_release_context(connection(), context_id_);
    }
  }

  uint32_t context_id() { return context_id_; }

  void ExecuteCommandBufferWithResources2(uint32_t resource_count) {
    ASSERT_TRUE(connection());

    magma_command_buffer command_buffer = {.resource_count = resource_count};
    magma_exec_resource resources[resource_count];

    memset(resources, 0, sizeof(magma_exec_resource) * resource_count);
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_execute_command_buffer_with_resources2(connection(), context_id(),
                                                           &command_buffer, resources, nullptr));

    // Command buffer is mostly zeros, so we expect an error here
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_error(connection()));
  }

  void ExecuteCommandBufferNoResources2() {
    ASSERT_TRUE(connection());

    magma_command_buffer command_buffer = {.resource_count = 0};
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_execute_command_buffer_with_resources2(
                  connection(), context_id(), &command_buffer, nullptr /* resources */, nullptr));

    // Empty command buffers may or may not be valid.
    magma_status_t status = magma_get_error(connection());
    EXPECT_TRUE(status == MAGMA_STATUS_OK || status == MAGMA_STATUS_INVALID_ARGS ||
                status == MAGMA_STATUS_UNIMPLEMENTED)
        << "status: " << status;
  }

  void ExecuteCommand(uint32_t resource_count) {
    ASSERT_TRUE(connection());

    magma_exec_command_buffer command_buffer = {.resource_index = 0, .start_offset = 0};

    magma_exec_resource resources[resource_count];
    memset(resources, 0, sizeof(magma_exec_resource) * resource_count);

    magma_command_descriptor descriptor = {.resource_count = resource_count,
                                           .command_buffer_count = 1,
                                           .resources = resources,
                                           .command_buffers = &command_buffer};

    EXPECT_EQ(MAGMA_STATUS_OK, magma_execute_command(connection(), context_id(), &descriptor));

    // Command buffer is mostly zeros, so we expect an error here
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_error(connection()));
  }

  void ExecuteCommandNoResources() {
    ASSERT_TRUE(connection());

    magma_command_descriptor descriptor = {.resource_count = 0, .command_buffer_count = 0};

    EXPECT_EQ(MAGMA_STATUS_OK, magma_execute_command(connection(), context_id(), &descriptor));

    // Empty command buffers may or may not be valid.
    magma_status_t status = magma_get_error(connection());
    EXPECT_TRUE(status == MAGMA_STATUS_OK || status == MAGMA_STATUS_INVALID_ARGS ||
                status == MAGMA_STATUS_UNIMPLEMENTED)
        << "status: " << status;
  }

  void ExecuteCommandTwoCommandBuffers() {
    ASSERT_TRUE(connection());

    std::array<magma_exec_resource, 2> resources{};
    std::array<magma_exec_command_buffer, 2> command_buffers = {
        magma_exec_command_buffer{.resource_index = 0, .start_offset = 0},
        magma_exec_command_buffer{.resource_index = 1, .start_offset = 0}};

    magma_command_descriptor descriptor = {.resource_count = resources.size(),
                                           .command_buffer_count = command_buffers.size(),
                                           .resources = resources.data(),
                                           .command_buffers = command_buffers.data()};

    EXPECT_EQ(MAGMA_STATUS_OK, magma_execute_command(connection(), context_id(), &descriptor));

    EXPECT_EQ(magma_get_error(connection()), MAGMA_STATUS_UNIMPLEMENTED);
  }

 private:
  uint32_t context_id_;
};

// NOTE: LoggingInit is first so other tests may use logging.
TEST(Magma, LoggingInit) {
  TestConnection test;
  test.LoggingInit();
}

TEST(Magma, DeviceId) {
  TestConnection test;
  test.GetDeviceIdImported();
}

TEST(Magma, VendorId) {
  TestConnection test;
  test.GetVendorIdImported();
}

TEST(Magma, QueryReturnsBuffer) {
  TestConnection test;
  test.QueryReturnsBufferImported();
}

// Test for cleanup of leaked mapping
TEST(Magma, QueryReturnsBufferLeaky) {
  constexpr bool kLeaky = true;
  TestConnection test;
  test.QueryReturnsBufferImported(kLeaky);
}

TEST(Magma, QueryReturnsBufferCalibratedTimestamps) {
  constexpr bool kLeaky = false;
  constexpr bool kCheckClock = true;
  TestConnection test;
  test.QueryReturnsBufferImported(kLeaky, kCheckClock);
}

TEST(Magma, QueryTestRestartSupported) {
  TestConnection test;
  test.QueryTestRestartSupported();
}

TEST(Magma, TracingInit) {
  TestConnection test;
  test.TracingInit();
}

TEST(Magma, Buffer) {
  TestConnection test;
  test.Buffer();
}

TEST(Magma, Connection) {
  TestConnection test;
  test.Connection();
}

TEST(Magma, Context) {
  TestConnection test;
  test.Context();
}

TEST(Magma, NotificationChannelHandle) {
  TestConnection test;
  test.NotificationChannelHandle();
}

TEST(Magma, ReadNotificationChannel) {
  TestConnection test;
  test.ReadNotificationChannel();
}

TEST(Magma, BufferMap) {
  TestConnection test;
  test.BufferMap();
}

TEST(Magma, BufferImportInvalid) { TestConnection().BufferImportInvalid(); }

TEST(Magma, BufferImportExport) {
  TestConnection test1;
  TestConnection test2;
  TestConnection::BufferImportExport(&test1, &test2);
}

TEST(Magma, Semaphore) {
  TestConnection test;
  test.Semaphore(1);
  test.Semaphore(2);
  test.Semaphore(3);
}

TEST(Magma, SemaphoreImportExport) {
  TestConnection test1;
  TestConnection test2;
  TestConnection::SemaphoreImportExport(&test1, &test2);
}

TEST(Magma, ImmediateCommands) { TestConnection().ImmediateCommands(); }

class MagmaPoll : public testing::TestWithParam<uint32_t> {};

TEST_P(MagmaPoll, PollWithNotificationChannel) {
  uint32_t semaphore_count = GetParam();
  TestConnection().PollWithNotificationChannel(semaphore_count);
}

INSTANTIATE_TEST_SUITE_P(MagmaPoll, MagmaPoll, ::testing::Values(0, 1, 2, 3));

TEST(Magma, PollWithTestChannel) { TestConnection().PollWithTestChannel(); }

TEST(Magma, PollChannelClosed) { TestConnection().PollChannelClosed(); }

TEST(Magma, Sysmem) {
  TestConnection test;
  test.Sysmem(false);
}

TEST(Magma, SysmemLinearFormatModifier) {
  TestConnection test;
  test.Sysmem(true);
}

TEST(Magma, FromC) { EXPECT_TRUE(test_magma_from_c(TestConnection::device_name().c_str())); }

TEST(Magma, ExecuteCommandBufferWithResources2) {
  TestConnectionWithContext().ExecuteCommandBufferWithResources2(5);
}

TEST(Magma, ExecuteCommandBufferNoResources2) {
  TestConnectionWithContext().ExecuteCommandBufferNoResources2();
}

TEST(Magma, ExecuteCommand) { TestConnectionWithContext().ExecuteCommand(5); }

TEST(Magma, ExecuteCommandNoResources) { TestConnectionWithContext().ExecuteCommandNoResources(); }

TEST(Magma, ExecuteCommandTwoCommandBuffers) {
  TestConnectionWithContext().ExecuteCommandTwoCommandBuffers();
}

TEST(Magma, FlowControl) {
  if (TestConnection::is_virtmagma())
    GTEST_SKIP();

  // Each call to Buffer is 2 messages.
  // Without flow control, this will trigger a policy exception (too many channel messages)
  // or an OOM.
  constexpr uint32_t kIterations = 10000 / 2;

  TestConnection test_connection;
  for (uint32_t i = 0; i < kIterations; i++) {
    test_connection.Buffer();
  }
}

TEST(Magma, EnablePerformanceCounters) { TestConnection().EnablePerformanceCounters(); }

TEST(Magma, DisabledPerformanceCounters) { TestConnection().DisabledPerformanceCounters(); }

TEST(Magma, CommitBuffer) {
#if !defined(__Fuchsia__)
  // magma_buffer_get_info is only implemented on Fuchsia.
  GTEST_SKIP();
#endif
  TestConnection connection;
  magma_buffer_t buffer;
  uint64_t size_out;
  uint64_t buffer_size = page_size() * 10;
  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_create_buffer(connection.connection(), buffer_size, &size_out, &buffer));
  magma_buffer_info_t info;
  EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_get_info(connection.connection(), buffer, &info));
  EXPECT_EQ(info.size, buffer_size);
  EXPECT_EQ(0u, info.committed_byte_count);

  EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_COMMIT, 0,
                                  page_size() + 1));
  EXPECT_EQ(MAGMA_STATUS_MEMORY_ERROR,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_COMMIT,
                                  page_size(), buffer_size));
  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_COMMIT,
                                  page_size(), page_size()));
  EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_get_info(connection.connection(), buffer, &info));
  EXPECT_EQ(page_size(), info.committed_byte_count);

  EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_DECOMMIT,
                                  0, page_size() + 1));
  EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_DECOMMIT,
                                  page_size(), buffer_size));
  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_DECOMMIT,
                                  2 * page_size(), page_size()));
  EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_get_info(connection.connection(), buffer, &info));
  EXPECT_EQ(page_size(), info.committed_byte_count);

  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_buffer_range_op(connection.connection(), buffer, MAGMA_BUFFER_RANGE_OP_DECOMMIT,
                                  page_size(), page_size()));
  EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_get_info(connection.connection(), buffer, &info));
  EXPECT_EQ(0u, info.committed_byte_count);

  magma_release_buffer(connection.connection(), buffer);
}

TEST(MagmaAbi, MapWithBufferHandle2) {
  TestConnection connection;

  magma_buffer_t buffer;
  uint64_t actual_size;
  constexpr uint64_t kBufferSizeInPages = 10;
  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_create_buffer(connection.connection(), kBufferSizeInPages * page_size(),
                                &actual_size, &buffer));

  magma_handle_t handle;
  ASSERT_EQ(MAGMA_STATUS_OK, magma_get_buffer_handle2(buffer, &handle));

  void* full_range_ptr;
  ASSERT_TRUE(magma::MapCpuHelper(buffer, 0 /*offset*/, actual_size, &full_range_ptr));

  // Some arbitrary constants
  constexpr uint32_t kPattern[] = {
      0x12345678,
      0x89abcdef,
      0xfedcba98,
      0x87654321,
  };

  reinterpret_cast<uint32_t*>(full_range_ptr)[0] = kPattern[0];
  reinterpret_cast<uint32_t*>(full_range_ptr)[1] = kPattern[1];
  reinterpret_cast<uint32_t*>(full_range_ptr)[actual_size / sizeof(uint32_t) - 2] = kPattern[2];
  reinterpret_cast<uint32_t*>(full_range_ptr)[actual_size / sizeof(uint32_t) - 1] = kPattern[3];

  EXPECT_TRUE(magma::UnmapCpuHelper(full_range_ptr, actual_size));

  void* first_page_ptr;
  EXPECT_TRUE(magma::MapCpuHelper(buffer, 0 /*offset*/, page_size(), &first_page_ptr));

  void* last_page_ptr;
  EXPECT_TRUE(magma::MapCpuHelper(buffer, (kBufferSizeInPages - 1) * page_size() /*offset*/,
                                  page_size(), &last_page_ptr));

  // Check that written values match.
  EXPECT_EQ(reinterpret_cast<uint32_t*>(first_page_ptr)[0], kPattern[0]);
  EXPECT_EQ(reinterpret_cast<uint32_t*>(first_page_ptr)[1], kPattern[1]);

  EXPECT_EQ(reinterpret_cast<uint32_t*>(last_page_ptr)[page_size() / sizeof(uint32_t) - 2],
            kPattern[2]);
  EXPECT_EQ(reinterpret_cast<uint32_t*>(last_page_ptr)[page_size() / sizeof(uint32_t) - 1],
            kPattern[3]);

  EXPECT_TRUE(magma::UnmapCpuHelper(last_page_ptr, page_size()));
  EXPECT_TRUE(magma::UnmapCpuHelper(first_page_ptr, page_size()));

  magma_release_buffer(connection.connection(), buffer);
}

TEST(MagmaAbi, MaxBufferHandle2) {
  TestConnection connection;

  magma_buffer_t buffer;
  uint64_t actual_size;
  constexpr uint64_t kBufferSizeInPages = 1;
  ASSERT_EQ(MAGMA_STATUS_OK,
            magma_create_buffer(connection.connection(), kBufferSizeInPages * page_size(),
                                &actual_size, &buffer));

  std::unordered_set<magma_handle_t> handles;

  // This may fail on Linux if the open file limit is too small.
  constexpr size_t kMaxBufferHandles = 10000;

  for (size_t i = 0; i < kMaxBufferHandles; i++) {
    magma_handle_t handle;

    magma_status_t status = magma_get_buffer_handle2(buffer, &handle);
    if (status != MAGMA_STATUS_OK) {
      EXPECT_EQ(status, MAGMA_STATUS_OK) << "magma_get_buffer_handle2 failed count: " << i;
      break;
    }
    handles.insert(handle);
  }

  EXPECT_EQ(handles.size(), kMaxBufferHandles);

  for (auto& handle : handles) {
#if defined(__Fuchsia__)
    zx_handle_close(handle);
#elif defined(__linux__)
    close(handle);
#endif
  }

  magma_release_buffer(connection.connection(), buffer);
}

TEST(Magma, MaxBufferMappings) {
  TestConnection connection;

  magma_buffer_t buffer;
  uint64_t actual_size;
  constexpr uint64_t kBufferSizeInPages = 1;
  ASSERT_EQ(MAGMA_STATUS_OK,
            magma_create_buffer(connection.connection(), kBufferSizeInPages * page_size(),
                                &actual_size, &buffer));

  std::unordered_set<void*> maps;

  // The helper closes the buffer handle, so the Linux open file limit shouldn't matter.
  constexpr size_t kMaxBufferMaps = 10000;

  for (size_t i = 0; i < kMaxBufferMaps; i++) {
    void* ptr;
    if (!magma::MapCpuHelper(buffer, 0 /*offset*/, actual_size, &ptr)) {
      EXPECT_TRUE(false) << "MapCpuHelper failed count: " << i;
      break;
    }
    maps.insert(ptr);
  }

  EXPECT_EQ(maps.size(), kMaxBufferMaps);

  for (void* ptr : maps) {
    EXPECT_TRUE(magma::UnmapCpuHelper(ptr, actual_size));
  }

  magma_release_buffer(connection.connection(), buffer);
}

TEST(Magma, Flush) {
  TestConnection connection;
  EXPECT_EQ(MAGMA_STATUS_OK, magma_flush(connection.connection()));
}
