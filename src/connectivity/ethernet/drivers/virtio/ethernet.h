// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_ETHERNET_DRIVERS_VIRTIO_ETHERNET_H_
#define SRC_CONNECTIVITY_ETHERNET_DRIVERS_VIRTIO_ETHERNET_H_

#include <fuchsia/hardware/ethernet/c/banjo.h>
#include <fuchsia/hardware/ethernet/cpp/banjo.h>
#include <lib/ddk/io-buffer.h>
#include <lib/virtio/device.h>
#include <lib/virtio/ring.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <memory>

#include <ddktl/device.h>
#include <fbl/macros.h>
#include <virtio/net.h>

namespace virtio {

class EthernetDevice;
using DeviceType = ddk::Device<EthernetDevice, ddk::GetProtocolable, ddk::Unbindable>;
class EthernetDevice : public Device,
                       // Mixins for protocol device:
                       public DeviceType,
                       // Mixin for Ethernet banjo protocol:
                       public ddk::EthernetImplProtocol<EthernetDevice, ddk::base_protocol> {
 public:
  explicit EthernetDevice(zx_device_t* device, zx::bti, std::unique_ptr<Backend> backend);
  virtual ~EthernetDevice();

  zx_status_t Init() override TA_EXCL(state_lock_);
  void DdkRelease() TA_EXCL(state_lock_);
  void DdkUnbind(ddk::UnbindTxn txn) { virtio::Device::Unbind(std::move(txn)); }
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);

  // VirtIO callbacks
  void IrqRingUpdate() override TA_EXCL(state_lock_);
  void IrqConfigChange() override TA_EXCL(state_lock_);

  // DDK protocol hooks; see fuchsia/hardware/ethernet/c/banjo.h
  zx_status_t EthernetImplQuery(uint32_t options, ethernet_info_t* info) TA_EXCL(state_lock_);
  void EthernetImplStop() TA_EXCL(state_lock_);
  zx_status_t EthernetImplStart(const ethernet_ifc_protocol_t* ifc) TA_EXCL(state_lock_);
  void EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                           ethernet_impl_queue_tx_callback completion_cb, void* cookie)
      TA_EXCL(state_lock_);

  const char* tag() const override { return "virtio-net"; }
  zx_status_t EthernetImplSetParam(uint32_t param, int32_t value, const uint8_t* data_buffer,
                                   size_t data_size) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  void EthernetImplGetBti(zx::bti* out_bti) {
    // return ZX_ERR_NOT_SUPPORTED;
  }

 private:
  static constexpr size_t kFailureWarnRate = 100;

  DISALLOW_COPY_ASSIGN_AND_MOVE(EthernetDevice);

  // Implementation of IrqRingUpdate; returns true if it should be called again.
  bool IrqRingUpdateInternal() TA_EXCL(state_lock_);

  // DDK device hooks; see ddk/device.h
  void ReleaseLocked() TA_REQ(state_lock_);

  // Mutexes to control concurrent access
  mtx_t state_lock_;
  mtx_t tx_lock_;

  // Virtqueues; see section 5.1.2 of the spec
  // This driver doesn't currently support multi-queueing, automatic
  // steering, or the control virtqueue, so only a single queue is needed in
  // each direction.
  Ring rx_;
  Ring tx_;
  std::unique_ptr<io_buffer_t[]> bufs_;
  size_t unkicked_ TA_GUARDED(tx_lock_);
  size_t tx_failed_descriptor_alloc_ TA_GUARDED(tx_lock_);

  size_t unrecycled_;

  // Saved net device configuration out of the pci config BAR
  virtio_net_config_t config_ TA_GUARDED(state_lock_);
  size_t virtio_hdr_len_;

  // Ethernet callback interface; see fuchsia/hardware/ethernet/c/banjo.h
  ethernet_ifc_protocol_t ifc_ TA_GUARDED(state_lock_);
};

}  // namespace virtio

#endif  // SRC_CONNECTIVITY_ETHERNET_DRIVERS_VIRTIO_ETHERNET_H_
