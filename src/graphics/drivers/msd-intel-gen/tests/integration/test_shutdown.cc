// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.gpu.magma/cpp/wire.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>
#include <magma_intel_gen_defs.h>

#include <thread>

#include <gtest/gtest.h>

#include "helper/inflight_list.h"
#include "helper/magma_map_cpu.h"
#include "helper/test_device_helper.h"
#include "magma.h"
#include "magma_util/macros.h"

namespace {

class TestConnection : public magma::TestDeviceBase {
 public:
  TestConnection() : magma::TestDeviceBase(MAGMA_VENDOR_ID_INTEL) {
    magma_create_connection2(device(), &connection_);

    magma_status_t status =
        magma_query2(device(), kMagmaIntelGenQueryExtraPageCount, &extra_page_count_);
    if (status != MAGMA_STATUS_OK) {
      DLOG("Failed to query kMagmaIntelGenQueryExtraPageCount: %d", status);
      extra_page_count_ = 0;
    }
  }

  ~TestConnection() {
    if (connection_)
      magma_release_connection(connection_);
  }

  static constexpr int64_t kOneSecondInNs = 1000000000;

  magma_status_t Test() {
    DASSERT(connection_);

    uint32_t context_id;
    magma::Status status = magma_create_context(connection_, &context_id);
    if (!status.ok())
      return DRET(status.get());

    status = magma_get_error(connection_);
    if (!status.ok())
      return DRET(status.get());

    uint64_t size;
    magma_buffer_t batch_buffer;
    status = magma_create_buffer(connection_, PAGE_SIZE, &size, &batch_buffer);
    if (!status.ok())
      return DRET(status.get());

    status = magma_map_buffer_gpu(connection_, batch_buffer, 0, 1, gpu_addr_, 0);
    if (!status.ok())
      return DRET(status.get());

    gpu_addr_ += (1 + extra_page_count_) * PAGE_SIZE;

    EXPECT_TRUE(InitBatchBuffer(batch_buffer, size));

    magma_command_descriptor descriptor;
    magma_exec_command_buffer command_buffer;
    magma_exec_resource exec_resource;
    EXPECT_TRUE(InitCommand(&descriptor, &command_buffer, &exec_resource, batch_buffer, size));

    status = magma_execute_command(connection_, context_id, &descriptor);
    if (!status.ok())
      return DRET(status.get());

    magma::InflightList list;
    status = list.WaitForCompletion(connection_, kOneSecondInNs);
    EXPECT_TRUE(status.get() == MAGMA_STATUS_OK || status.get() == MAGMA_STATUS_CONNECTION_LOST);

    magma_release_context(connection_, context_id);
    magma_release_buffer(connection_, batch_buffer);

    status = magma_get_error(connection_);
    return DRET(status.get());
  }

  bool InitBatchBuffer(magma_buffer_t buffer, uint64_t size) {
    void* vaddr;
    if (!magma::MapCpuHelper(buffer, 0 /*offset*/, size, &vaddr))
      return DRETF(false, "couldn't map batch buffer");

    memset(vaddr, 0, size);

    // Intel end-of-batch
    *reinterpret_cast<uint32_t*>(vaddr) = 0xA << 23;

    EXPECT_TRUE(magma::UnmapCpuHelper(vaddr, size));

    return true;
  }

  bool InitCommand(magma_command_descriptor* descriptor, magma_exec_command_buffer* command_buffer,
                   magma_exec_resource* exec_resource, magma_buffer_t batch_buffer,
                   uint64_t batch_buffer_length) {
    exec_resource->buffer_id = magma_get_buffer_id(batch_buffer);
    exec_resource->offset = 0;
    exec_resource->length = batch_buffer_length;

    command_buffer->resource_index = 0;
    command_buffer->start_offset = 0;

    descriptor->resource_count = 1;
    descriptor->command_buffer_count = 1;
    descriptor->wait_semaphore_count = 0;
    descriptor->signal_semaphore_count = 0;
    descriptor->resources = exec_resource;
    descriptor->command_buffers = command_buffer;
    descriptor->semaphore_ids = nullptr;
    descriptor->flags = 0;

    return true;
  }

 private:
  magma_connection_t connection_;
  uint64_t extra_page_count_ = 0;
  uint64_t gpu_addr_ = 0;
};

constexpr uint32_t kMaxCount = 100;
constexpr uint32_t kRestartCount = kMaxCount / 10;

static std::atomic_uint complete_count;

static void looper_thread_entry() {
  std::unique_ptr<TestConnection> test(new TestConnection());
  while (complete_count < kMaxCount) {
    magma_status_t status = test->Test();
    if (status == MAGMA_STATUS_OK) {
      complete_count++;
    } else {
      EXPECT_EQ(status, MAGMA_STATUS_CONNECTION_LOST);
      test.reset(new TestConnection());
    }
  }
}

static void test_shutdown(uint32_t iters) {
  for (uint32_t i = 0; i < iters; i++) {
    complete_count = 0;

    magma::TestDeviceBase test_base(MAGMA_VENDOR_ID_INTEL);

    {
      auto result = fidl::WireCall<fuchsia_gpu_magma::Device>(test_base.channel()->borrow())
                        ->Query2(MAGMA_QUERY_IS_TEST_RESTART_SUPPORTED);
      uint64_t is_supported = result->result.response().result;
      if (result.status() != ZX_OK || !is_supported) {
        printf("Test restart not supported: status %d is_supported %lu\n", result.status(),
               is_supported);
        return;
      }
    }

    std::thread looper(looper_thread_entry);
    std::thread looper2(looper_thread_entry);

    uint32_t count = kRestartCount;
    while (complete_count < kMaxCount) {
      if (complete_count > count) {
        // TODO(fxbug.dev/13126) replace this with a request to devmgr to restart the driver
        auto result =
            fidl::WireCall<fuchsia_gpu_magma::Device>(test_base.channel()->borrow())->TestRestart();
        EXPECT_EQ(ZX_OK, result.status());
        count += kRestartCount;
      }
      std::this_thread::yield();
    }

    looper.join();
    looper2.join();
  }
}

}  // namespace

TEST(Shutdown, Test) { test_shutdown(1); }

TEST(Shutdown, DISABLED_Stress) { test_shutdown(1000); }
