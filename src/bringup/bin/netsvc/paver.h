// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_NETSVC_PAVER_H_
#define SRC_BRINGUP_BIN_NETSVC_PAVER_H_

#include <fidl/fuchsia.paver/cpp/wire.h>
#include <lib/fzl/resizeable-vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <limits.h>
#include <threads.h>
#include <zircon/boot/netboot.h>
#include <zircon/types.h>

#include <atomic>
#include <optional>
#include <string_view>

#include <fbl/unique_fd.h>
#include <tftp/tftp.h>

#include "src/bringup/bin/netsvc/tftp.h"

namespace netsvc {

class PaverInterface {
 public:
  virtual bool InProgress() = 0;
  virtual zx_status_t exit_code() = 0;
  virtual void reset_exit_code() = 0;

  // TODO: Explore returning an object which implements write and when it goes
  // out of scope, closes.
  virtual tftp_status OpenWrite(std::string_view filename, size_t size, zx::duration timeout) = 0;
  virtual tftp_status Write(const void* data, size_t* length, off_t offset) = 0;
  virtual void Close() = 0;
  virtual void Abort() = 0;
};

class Paver : public PaverInterface {
 public:
  // Get the singleton instance.
  static Paver* Get();

  bool InProgress() final;
  zx_status_t exit_code() final;
  void reset_exit_code() final;

  tftp_status OpenWrite(std::string_view filename, size_t size, zx::duration timeout) final;
  tftp_status Write(const void* data, size_t* length, off_t offset) final;
  void Close() final;
  void Abort() final;

  // Visible for testing.
  explicit Paver(fidl::ClientEnd<fuchsia_io::Directory> svc_root, fbl::unique_fd devfs_root)
      : svc_root_(std::move(svc_root)), devfs_root_(std::move(devfs_root)) {}

 private:
  static constexpr uint32_t kBufferRefWorker = 1 << 0;
  static constexpr uint32_t kBufferRefApi = 1 << 1;
  // Refer to //zircon/system/fidl/fuchsia.paver/paver.fidl for a list of what
  // these commands translate to.
  enum class Command {
    kAsset,
    kFirmware,
    kDataFile,
    kFvm,
    kInitPartitionTables,
    kWipePartitionTables,
  };

  uint8_t* buffer() { return static_cast<uint8_t*>(buffer_mapper_.start()); }

  zx_status_t WriteABImage(fidl::WireSyncClient<fuchsia_paver::DataSink> data_sink,
                           fuchsia_mem::wire::Buffer buffer);

  zx_status_t OpenDataSink(fuchsia_mem::wire::Buffer buffer,
                           fidl::WireSyncClient<fuchsia_paver::DynamicDataSink>* data_sink);
  zx_status_t InitPartitionTables(fuchsia_mem::wire::Buffer buffer);
  zx_status_t WipePartitionTables(fuchsia_mem::wire::Buffer buffer);

  // Pushes all data from the paver buffer (filled by netsvc) into the paver input VMO. When
  // there's no data to copy, blocks on data_ready until more data is written into the buffer.
  int StreamBuffer();

  // Monitors the vmo progress, and calls into paver service once finished.
  int MonitorBuffer();

  // Clear Sysconfig if the device has one.
  zx_status_t ClearSysconfig();

  tftp_status ProcessAsFirmwareImage(std::string_view host_filename);

  void ClearBufferRef(uint32_t ref);

  std::atomic<bool> in_progress_ = false;
  std::atomic<zx_status_t> exit_code_ = ZX_OK;

  // Total size of file
  size_t size_ = 0;

  // Paver command to call into.
  Command command_;

  // Channel to svc.
  fidl::ClientEnd<fuchsia_io::Directory> svc_root_;

  // File descriptor to dev.
  fbl::unique_fd devfs_root_;

  fidl::WireSyncClient<fuchsia_paver::Paver> paver_svc_;

  fuchsia_paver::wire::Configuration configuration_;
  // Only meaningful when command == Command::kAsset.
  fuchsia_paver::wire::Asset asset_;
  // Only meaningful when command == Command::kDataFile.
  char path_[PATH_MAX];
  // Only meaningful when command == Command::kFirmware.
  char firmware_type_[NB_FIRMWARE_TYPE_MAX_LENGTH + 1];

  // Buffer used for stashing data from tftp until it can be written out to the paver.
  fzl::ResizeableVmoMapper buffer_mapper_;
  // Buffer write offset.
  std::atomic<size_t> write_offset_ = 0;
  std::atomic<uint32_t> buffer_refs_ = 0;
  thrd_t buf_thrd_ = 0;
  sync_completion_t data_ready_;
  std::atomic<bool> aborted_;

  // Timeout monitor thread uses before timing out.
  zx::duration timeout_;
};

}  // namespace netsvc

#endif  // SRC_BRINGUP_BIN_NETSVC_PAVER_H_
