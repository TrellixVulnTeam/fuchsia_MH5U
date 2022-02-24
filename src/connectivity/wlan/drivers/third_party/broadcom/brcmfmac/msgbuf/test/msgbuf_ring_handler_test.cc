// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/msgbuf_ring_handler.h"

#include <lib/zx/time.h>
#include <netinet/if_ether.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/dma_buffer.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/dma_pool.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fweh.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/msgbuf_structs.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/test/fake_msgbuf_interfaces.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/test/test_utils.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/test/stub_netbuf.h"

namespace wlan {
namespace brcmfmac {
namespace {

// Time out value for test waits.
constexpr zx::duration kTestTimeout = zx::sec(1);

// Sleep duration when spinning and and retrying a test operation.
constexpr zx::duration kTestSpinSleep = zx::usec(100);

// Count and size of buffers.  We use small counts to force buffer recycling in tests.
constexpr int kPoolBufferSize = 1024;
constexpr size_t kPoolBufferCount = 32;

// Attempt multiple times to invoke a function.
template <typename F, typename... Args>
zx_status_t SpinInvoke(F&& f, Args&&... args) {
  zx_status_t status = ZX_OK;
  int iterations = 0;
  while ((status = std::invoke(std::forward<F>(f), std::forward<Args>(args)...)) ==
         ZX_ERR_SHOULD_WAIT) {
    if (iterations++ >= kTestTimeout / kTestSpinSleep) {
      return status;
    }
    zx::nanosleep(zx::deadline_after(kTestSpinSleep));
  }
  return status;
}

// Create a DMA pool.
void CreateDmaPool(FakeMsgbufInterfaces* interfaces, size_t buffer_size, int buffer_count,
                   std::unique_ptr<DmaPool>* dma_pool_out) {
  std::unique_ptr<DmaBuffer> dma_buffer;
  ASSERT_EQ(ZX_OK, interfaces->CreateDmaBuffer(ZX_CACHE_POLICY_CACHED, buffer_count * buffer_size,
                                               &dma_buffer));
  std::unique_ptr<DmaPool> dma_pool;
  ASSERT_EQ(ZX_OK, DmaPool::Create(buffer_size, buffer_count, std::move(dma_buffer), &dma_pool));
  *dma_pool_out = std::move(dma_pool);
}

// Stub implementation of the EventHandler interface.
class StubEventHandler : public MsgbufRingHandler::EventHandler {
 public:
  ~StubEventHandler();

  // MsgbufRingHandler::EventHandler implementation.
  void HandleWlEvent(const void* data, size_t size) override;
  void HandleRxData(int interface_index, const void* data, size_t size) override;
};

StubEventHandler::~StubEventHandler() = default;

void StubEventHandler::HandleWlEvent(const void* data, size_t size) {}

void StubEventHandler::HandleRxData(int interface_index, const void* data, size_t size) {}

// Test creation of the MsgbufRingHandler using various creation parameters.
TEST(MsgbufRingHandlerTest, CreationParameters) {
  std::unique_ptr<FakeMsgbufInterfaces> fake_interfaces;
  std::unique_ptr<DmaPool> rx_buffer_pool;
  std::unique_ptr<DmaPool> tx_buffer_pool;
  StubEventHandler event_handler;

  ASSERT_EQ(ZX_OK, FakeMsgbufInterfaces::Create(&fake_interfaces));
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &rx_buffer_pool);
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &tx_buffer_pool);

  std::unique_ptr<MsgbufRingHandler> ring_handler;
  EXPECT_EQ(ZX_OK, MsgbufRingHandler::Create(fake_interfaces.get(), fake_interfaces.get(),
                                             std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                             &event_handler, &ring_handler));
}

// Test the ioctl interfaces of the MsgbufRingHandler.  This test sends `kTestIterationCount`
// Ioctl() calls with set interface index, command, and data, expecting the bitwise negation of the
// data in return, and in order.
TEST(MsgbufRingHandlerTest, Ioctl) {
  struct IoctlTestData {
    uint8_t interface_index;
    uint32_t command;
    std::string data;
  };

  using namespace std::string_literals;
  IoctlTestData test_data[7] = {
      {0, 0, ""s},
      {42, 27, std::string(kPoolBufferSize, '\0')},
      {1, 2, "Lorem Ipsum"},
      {0xFF, ~0u, std::string(kPoolBufferSize / 2, '\x08')},
      {255, 65536, "foo\0bar\0baz\n\0"s},
      {3, 14, "159265358979323846264338327950288419716939937510"s},
      {0xFF, ~0u, std::string(kPoolBufferSize - 31, '\xFF')},
  };
  constexpr int kTestIterationCount = 256;

  std::unique_ptr<FakeMsgbufInterfaces> fake_interfaces;
  std::unique_ptr<DmaPool> rx_buffer_pool;
  std::unique_ptr<DmaPool> tx_buffer_pool;
  StubEventHandler event_handler;
  std::unique_ptr<MsgbufRingHandler> ring_handler;

  ASSERT_EQ(ZX_OK, FakeMsgbufInterfaces::Create(&fake_interfaces));
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &rx_buffer_pool);
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &tx_buffer_pool);
  ASSERT_EQ(ZX_OK, MsgbufRingHandler::Create(fake_interfaces.get(), fake_interfaces.get(),
                                             std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                             &event_handler, &ring_handler));

  // Set up the expectations for the control submit ring.
  for (int i = 0; i < kTestIterationCount; ++i) {
    const IoctlTestData& datum = test_data[i % std::size(test_data)];

    // The operations we perform here should not require explicit synchronization with the Ioctl()
    // call itself, since the Ioctl() call should block on a zx::event until the response is
    // received.
    fake_interfaces->AddControlSubmitRingCallback([&](const void* data, size_t size) {
      // Make sure we received the ioctl request, as expected.
      const auto ioctl_request = GetMsgStruct<MsgbufIoctlRequest>(data, size);
      if (ioctl_request == nullptr) {
        return;
      }
      EXPECT_EQ(datum.interface_index, ioctl_request->msg.ifidx);
      EXPECT_EQ(datum.command, ioctl_request->cmd);
      EXPECT_EQ(datum.data.size(), ioctl_request->input_buf_len);
      const uintptr_t tx_buffer_address =
          fake_interfaces->GetDmaBufferAddress(ioctl_request->req_buf_addr);
      EXPECT_NE(0u, tx_buffer_address);

      // Now construct the expected response data, by bitwise NOT of all the byte data.
      auto buffer = fake_interfaces->GetIoctlRxBuffer();
      EXPECT_NE(0u, buffer.address);
      ASSERT_TRUE(buffer.size <= std::numeric_limits<uint16_t>::max());
      const auto write_size =
          std::min<uint16_t>(ioctl_request->input_buf_len, static_cast<uint16_t>(buffer.size));
      if (tx_buffer_address != 0 && buffer.address != 0) {
        const char* tx_buffer_data = reinterpret_cast<const char*>(tx_buffer_address);
        char* rx_buffer_data = reinterpret_cast<char*>(buffer.address);
        for (size_t i = 0; i < write_size; ++i) {
          rx_buffer_data[i] = ~tx_buffer_data[i];
        }
      }

      // Send it back in a kIoctlResponse message.
      MsgbufIoctlResponse ioctl_response = {};
      ioctl_response.msg.msgtype = MsgbufIoctlResponse::kMsgType;
      ioctl_response.msg.request_id = buffer.index;
      ioctl_response.resp_len = write_size;
      ioctl_response.trans_id = ioctl_request->trans_id;
      ioctl_response.compl_hdr.status = BCME_OK;

      EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddControlCompleteRingEntry,
                                  fake_interfaces.get(), &ioctl_response, sizeof(ioctl_response)));
    });
  }

  // Now perform the Ioctl() calls.  Each of these will complete sequentially, and should trigger
  // the responses we set up above.
  for (int i = 0; i < kTestIterationCount; ++i) {
    // Send the ioctl.
    const IoctlTestData& datum = test_data[i % std::size(test_data)];
    DmaPool::Buffer tx_buffer;
    void* tx_buffer_data = nullptr;
    EXPECT_EQ(ZX_OK, ring_handler->GetTxBuffer(&tx_buffer));
    EXPECT_EQ(ZX_OK, tx_buffer.MapWrite(datum.data.size(), &tx_buffer_data));
    if (tx_buffer_data != nullptr) {
      std::memcpy(tx_buffer_data, datum.data.data(), datum.data.size());
    }

    // Confirm receipt of the response.
    DmaPool::Buffer rx_buffer;
    const void* rx_buffer_data = nullptr;
    size_t rx_data_size = 0;
    bcme_status_t firmware_error = BCME_OK;
    EXPECT_EQ(ZX_OK, ring_handler->Ioctl(datum.interface_index, datum.command, std::move(tx_buffer),
                                         datum.data.size(), &rx_buffer, &rx_data_size,
                                         &firmware_error, kTestTimeout));
    EXPECT_EQ(ZX_OK, rx_buffer.MapRead(rx_data_size, &rx_buffer_data));
    EXPECT_EQ(BCME_OK, firmware_error);

    // Confirm the response.
    EXPECT_EQ(datum.data.size(), rx_data_size);
    const size_t compare_size = std::min<size_t>(datum.data.size(), rx_data_size);
    const char* rx_buffer_data_char = reinterpret_cast<const char*>(rx_buffer_data);
    for (size_t i = 0; i < compare_size; ++i) {
      EXPECT_EQ(static_cast<char>(~datum.data[i]), rx_buffer_data_char[i]);
    }
  }
}

// Test the MsgbufRingHandler WlEvent handling by sending a series of events.
TEST(MsgbufRingHandlerTest, WlEvent) {
  static constexpr size_t kMaxEventSize = kPoolBufferSize;

  // We expect a series of events with increasing event size, where the data is just a (wrapping)
  // array of increasing bytes.
  class WlEventHandler : public StubEventHandler {
   public:
    void HandleWlEvent(const void* data, size_t size) override {
      EXPECT_EQ(event_size_, size);
      const auto char_data = reinterpret_cast<const int8_t*>(data);
      int8_t expected_data = 0;
      for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(expected_data, char_data[i]);
        ++expected_data;
      }

      // The next event will be one larger.
      ++event_size_;
    }

   private:
    size_t event_size_ = 0;
  };

  std::unique_ptr<FakeMsgbufInterfaces> fake_interfaces;
  std::unique_ptr<DmaPool> rx_buffer_pool;
  std::unique_ptr<DmaPool> tx_buffer_pool;
  WlEventHandler event_handler;
  std::unique_ptr<MsgbufRingHandler> ring_handler;

  ASSERT_EQ(ZX_OK, FakeMsgbufInterfaces::Create(&fake_interfaces));
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &rx_buffer_pool);
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &tx_buffer_pool);
  ASSERT_EQ(ZX_OK, MsgbufRingHandler::Create(fake_interfaces.get(), fake_interfaces.get(),
                                             std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                             &event_handler, &ring_handler));

  for (size_t event_size = 0; event_size < kMaxEventSize; ++event_size) {
    auto rx_event_buffer = fake_interfaces->GetEventRxBuffer();
    ASSERT_LE(event_size, rx_event_buffer.size);
    const auto rx_event_buffer_data = reinterpret_cast<int8_t*>(rx_event_buffer.address);
    std::iota(rx_event_buffer_data, rx_event_buffer_data + event_size, 0);

    MsgbufWlEvent wl_event = {};
    wl_event.msg.msgtype = MsgbufWlEvent::kMsgType;
    wl_event.msg.request_id = rx_event_buffer.index;
    ASSERT_TRUE(event_size <= std::numeric_limits<uint16_t>::max());
    wl_event.event_data_len = static_cast<uint16_t>(event_size);

    EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddControlCompleteRingEntry,
                                fake_interfaces.get(), &wl_event, sizeof(wl_event)));
  }
}

// Test the MsgbufRingHandler TX flow.
TEST(MsgbufRingHandlerTest, TxData) {
  static constexpr int kTxIterations = 1024;

  std::unique_ptr<FakeMsgbufInterfaces> fake_interfaces;
  std::unique_ptr<DmaPool> rx_buffer_pool;
  std::unique_ptr<DmaPool> tx_buffer_pool;
  StubEventHandler event_handler;
  std::unique_ptr<MsgbufRingHandler> ring_handler;

  ASSERT_EQ(ZX_OK, FakeMsgbufInterfaces::Create(&fake_interfaces));
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &rx_buffer_pool);
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &tx_buffer_pool);
  ASSERT_EQ(ZX_OK, MsgbufRingHandler::Create(fake_interfaces.get(), fake_interfaces.get(),
                                             std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                             &event_handler, &ring_handler));

  // Expectations for unicast TX: a flow ring is created, then the unicast data in transmitted.
  static constexpr ethhdr kUnicastHeader = {
      {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5}, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 0};
  static constexpr int kUnicastInterface = 1;
  static constexpr bool kUnicastApMode = false;
  uint16_t unicast_flow_ring_id = 0;
  sync_completion_t unicast_tx_complete;
  fake_interfaces->AddControlSubmitRingCallback([&](const void* buffer, size_t size) {
    const MsgbufFlowRingCreateRequest* const create_request =
        GetMsgStruct<MsgbufFlowRingCreateRequest>(buffer, size);
    if (create_request == nullptr) {
      return;
    }
    EXPECT_EQ(
        0, std::memcmp(kUnicastHeader.h_dest, create_request->da, sizeof(kUnicastHeader.h_dest)));
    unicast_flow_ring_id = create_request->flow_ring_id;

    for (int i = 0; i < kTxIterations; ++i) {
      // Every time a TX request comes over the flow ring, respond with a TX response on the TX
      // complete ring.
      fake_interfaces->AddFlowRingCallback(
          create_request->flow_ring_id - fake_interfaces->GetDmaConfig().flow_ring_offset,
          [&, i](const void* buffer, size_t size) {
            const MsgbufTxRequest* const tx_request = GetMsgStruct<MsgbufTxRequest>(buffer, size);
            if (tx_request == nullptr) {
              return;
            }
            EXPECT_EQ(0, std::memcmp(&kUnicastHeader, tx_request->txhdr, sizeof(kUnicastHeader)));

            MsgbufTxResponse tx_response = {};
            tx_response.msg.msgtype = MsgbufTxResponse::kMsgType;
            tx_response.msg.ifidx = tx_request->msg.ifidx;
            tx_response.msg.request_id = tx_request->msg.request_id;
            EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddTxCompleteRingEntry,
                                        fake_interfaces.get(), &tx_response, sizeof(tx_response)));

            if (i == kTxIterations - 1) {
              sync_completion_signal(&unicast_tx_complete);
            }
          });
    }

    MsgbufFlowRingCreateResponse response = {};
    response.msg.msgtype = MsgbufFlowRingCreateResponse::kMsgType;
    response.msg.ifidx = create_request->msg.ifidx;
    response.compl_hdr.flow_ring_id = create_request->flow_ring_id;

    EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddControlCompleteRingEntry,
                                fake_interfaces.get(), &response, sizeof(response)));
  });

  // Without an interface, TX fails.
  ring_handler->QueueTxData(
      kUnicastInterface,
      std::make_unique<StubNetbuf>(&kUnicastHeader, sizeof(kUnicastHeader), ZX_ERR_NOT_FOUND));

  // Create an interface, transmit, and wait.
  ring_handler->ResetInterface(kUnicastInterface, kUnicastApMode);
  for (int i = 0; i < kTxIterations; ++i) {
    ring_handler->QueueTxData(
        kUnicastInterface,
        std::make_unique<StubNetbuf>(&kUnicastHeader, sizeof(kUnicastHeader), ZX_OK));
  }
  sync_completion_wait(&unicast_tx_complete, kTestTimeout.get());

  // Now do the same for multicast TX in AP mode: note in particular that the created flow ring is
  // mapped to destination address FF:FF:FF:FF:FF:FF, common to all multicast destinations in AP
  // mode.
  static constexpr ethhdr kMulticastHeader = {
      {0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6}, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 0};
  static constexpr int kMulticastInterface = 2;
  static constexpr bool kMulticastApMode = true;
  uint16_t multicast_flow_ring_id = 0;
  sync_completion_t multicast_tx_complete;
  fake_interfaces->AddControlSubmitRingCallback([&](const void* buffer, size_t size) {
    const MsgbufFlowRingCreateRequest* const create_request =
        GetMsgStruct<MsgbufFlowRingCreateRequest>(buffer, size);
    if (create_request == nullptr) {
      return;
    }
    static constexpr uint8_t kAllFF[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(0, std::memcmp(kAllFF, create_request->da, sizeof(kAllFF)));
    multicast_flow_ring_id = create_request->flow_ring_id;

    for (int i = 0; i < kTxIterations; ++i) {
      // Every time a TX request comes over the flow ring, respond with a TX response on the TX
      // complete ring.
      fake_interfaces->AddFlowRingCallback(
          create_request->flow_ring_id - fake_interfaces->GetDmaConfig().flow_ring_offset,
          [&, i](const void* buffer, size_t size) {
            const MsgbufTxRequest* const tx_request = GetMsgStruct<MsgbufTxRequest>(buffer, size);
            if (tx_request == nullptr) {
              return;
            }
            EXPECT_EQ(0,
                      std::memcmp(&kMulticastHeader, tx_request->txhdr, sizeof(kMulticastHeader)));

            MsgbufTxResponse tx_response = {};
            tx_response.msg.msgtype = MsgbufTxResponse::kMsgType;
            tx_response.msg.ifidx = tx_request->msg.ifidx;
            tx_response.msg.request_id = tx_request->msg.request_id;
            EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddTxCompleteRingEntry,
                                        fake_interfaces.get(), &tx_response, sizeof(tx_response)));

            if (i == kTxIterations - 1) {
              sync_completion_signal(&multicast_tx_complete);
            }
          });
    }

    MsgbufFlowRingCreateResponse response = {};
    response.msg.msgtype = MsgbufFlowRingCreateResponse::kMsgType;
    response.msg.ifidx = create_request->msg.ifidx;
    response.compl_hdr.flow_ring_id = create_request->flow_ring_id;

    EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddControlCompleteRingEntry,
                                fake_interfaces.get(), &response, sizeof(response)));
  });

  // Without an interface, TX fails.
  ring_handler->QueueTxData(
      kMulticastInterface,
      std::make_unique<StubNetbuf>(&kMulticastHeader, sizeof(kMulticastHeader), ZX_ERR_NOT_FOUND));

  // Create an interface, transmit, and wait.
  ring_handler->ResetInterface(kMulticastInterface, kMulticastApMode);
  for (int i = 0; i < kTxIterations; ++i) {
    ring_handler->QueueTxData(
        kMulticastInterface,
        std::make_unique<StubNetbuf>(&kMulticastHeader, sizeof(kMulticastHeader), ZX_OK));
  }
  sync_completion_wait(&multicast_tx_complete, kTestTimeout.get());

  // Make sure we get the ring deletion messages now.
  sync_completion_t unicast_delete_complete;
  sync_completion_t multicast_delete_complete;
  auto delete_callback = [&](const void* buffer, size_t size) {
    const MsgbufFlowRingDeleteRequest* const delete_request =
        GetMsgStruct<MsgbufFlowRingDeleteRequest>(buffer, size);
    if (delete_request == nullptr) {
      return;
    }
    sync_completion_t* completion = nullptr;
    if (delete_request->flow_ring_id == unicast_flow_ring_id) {
      completion = &unicast_delete_complete;
      unicast_flow_ring_id = 0;
    } else if (delete_request->flow_ring_id == multicast_flow_ring_id) {
      completion = &multicast_delete_complete;
      multicast_flow_ring_id = 0;
    } else {
      EXPECT_TRUE(delete_request->flow_ring_id == unicast_flow_ring_id ||
                  delete_request->flow_ring_id == multicast_flow_ring_id);
      return;
    }

    MsgbufFlowRingDeleteResponse response = {};
    response.msg.msgtype = MsgbufFlowRingDeleteResponse::kMsgType;
    response.msg.ifidx = delete_request->msg.ifidx;
    response.compl_hdr.flow_ring_id = delete_request->flow_ring_id;

    EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddControlCompleteRingEntry,
                                fake_interfaces.get(), &response, sizeof(response)));

    sync_completion_signal(completion);
  };
  fake_interfaces->AddControlSubmitRingCallback(delete_callback);
  fake_interfaces->AddControlSubmitRingCallback(delete_callback);
  ring_handler->ResetInterface(kUnicastInterface, kUnicastApMode);
  ring_handler->ResetInterface(kMulticastInterface, kMulticastApMode);
  sync_completion_wait(&unicast_delete_complete, kTestTimeout.get());
  sync_completion_wait(&multicast_delete_complete, kTestTimeout.get());
}

// Test the MsgbufRingHandler RxData handling by sending a series of data buffers.
TEST(MsgbufRingHandlerTest, RxData) {
  static constexpr size_t kMaxDataSize = kPoolBufferSize;

  // We expect a series of data RX with increasing data size, where the data is just a (wrapping)
  // array of increasing bytes.
  class RxDataHandler : public StubEventHandler {
   public:
    void HandleRxData(int interface_index, const void* data, size_t size) override {
      EXPECT_EQ(data_size_, size);
      const auto char_data = reinterpret_cast<const int8_t*>(data);
      int8_t expected_data = 0;
      for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(expected_data, char_data[i]);
        ++expected_data;
      }

      // The next data buffer will be one larger.
      ++data_size_;
    }

   private:
    size_t data_size_ = 0;
  };

  std::unique_ptr<FakeMsgbufInterfaces> fake_interfaces;
  std::unique_ptr<DmaPool> rx_buffer_pool;
  std::unique_ptr<DmaPool> tx_buffer_pool;
  RxDataHandler event_handler;
  std::unique_ptr<MsgbufRingHandler> ring_handler;

  ASSERT_EQ(ZX_OK, FakeMsgbufInterfaces::Create(&fake_interfaces));
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &rx_buffer_pool);
  CreateDmaPool(fake_interfaces.get(), kPoolBufferSize, kPoolBufferCount, &tx_buffer_pool);
  ASSERT_EQ(ZX_OK, MsgbufRingHandler::Create(fake_interfaces.get(), fake_interfaces.get(),
                                             std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                             &event_handler, &ring_handler));

  for (size_t data_size = 0; data_size < kMaxDataSize; ++data_size) {
    auto rx_data_buffer = fake_interfaces->GetRxBuffer();
    ASSERT_LE(data_size, rx_data_buffer.size);
    const auto rx_data_buffer_data = reinterpret_cast<int8_t*>(rx_data_buffer.address);
    std::iota(rx_data_buffer_data, rx_data_buffer_data + data_size, 0);

    MsgbufRxEvent rx_data = {};
    rx_data.msg.msgtype = MsgbufRxEvent::kMsgType;
    rx_data.msg.request_id = rx_data_buffer.index;
    rx_data.data_len = static_cast<uint16_t>(data_size);

    EXPECT_EQ(ZX_OK, SpinInvoke(&FakeMsgbufInterfaces::AddRxCompleteRingEntry,
                                fake_interfaces.get(), &rx_data, sizeof(rx_data)));
  }
}

}  // namespace
}  // namespace brcmfmac
}  // namespace wlan
