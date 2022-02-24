// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_PTYSVC_PTY_CLIENT_DEVICE_H_
#define SRC_BRINGUP_BIN_PTYSVC_PTY_CLIENT_DEVICE_H_

#include <fidl/fuchsia.hardware.pty/cpp/wire.h>
#include <lib/zx/channel.h>

#include <fbl/ref_ptr.h>

#include "pty-client.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

class PtyClientDevice : public fidl::WireServer<fuchsia_hardware_pty::Device> {
 public:
  explicit PtyClientDevice(fbl::RefPtr<PtyClient> client) : client_(std::move(client)) {}

  ~PtyClientDevice() override = default;

  // fuchsia.hardware.pty.Device methods
  void OpenClient(OpenClientRequestView request, OpenClientCompleter::Sync& completer) final;
  void ClrSetFeature(ClrSetFeatureRequestView request,
                     ClrSetFeatureCompleter::Sync& completer) final;
  void GetWindowSize(GetWindowSizeRequestView request,
                     GetWindowSizeCompleter::Sync& completer) final;
  void MakeActive(MakeActiveRequestView request, MakeActiveCompleter::Sync& completer) final;
  void ReadEvents(ReadEventsRequestView request, ReadEventsCompleter::Sync& completer) final;
  void SetWindowSize(SetWindowSizeRequestView request,
                     SetWindowSizeCompleter::Sync& completer) final;

  // fuchsia.io.File methods
  void ReadDeprecated(ReadDeprecatedRequestView request,
                      ReadDeprecatedCompleter::Sync& completer) final;
  void Read(ReadRequestView request, ReadCompleter::Sync& completer) final;
  void ReadAtDeprecated(ReadAtDeprecatedRequestView request,
                        ReadAtDeprecatedCompleter::Sync& completer) final;
  void ReadAt(ReadAtRequestView request, ReadAtCompleter::Sync& completer) final;

  void WriteDeprecated(WriteDeprecatedRequestView request,
                       WriteDeprecatedCompleter::Sync& completer) final;
  void Write(WriteRequestView request, WriteCompleter::Sync& completer) final;
  void WriteAtDeprecated(WriteAtDeprecatedRequestView request,
                         WriteAtDeprecatedCompleter::Sync& completer) final;
  void WriteAt(WriteAtRequestView request, WriteAtCompleter::Sync& completer) final;

  void SeekDeprecated(SeekDeprecatedRequestView request,
                      SeekDeprecatedCompleter::Sync& completer) final;
  void Seek(SeekRequestView request, SeekCompleter::Sync& completer) final;
  void Truncate(TruncateRequestView request, TruncateCompleter::Sync& completer) final;
  void Resize(ResizeRequestView request, ResizeCompleter::Sync& completer) final;
  void GetFlagsDeprecatedUseNode(GetFlagsDeprecatedUseNodeRequestView request,
                                 GetFlagsDeprecatedUseNodeCompleter::Sync& completer) final;
  void SetFlagsDeprecatedUseNode(SetFlagsDeprecatedUseNodeRequestView request,
                                 SetFlagsDeprecatedUseNodeCompleter::Sync& completer) final;
  void GetBuffer(GetBufferRequestView request, GetBufferCompleter::Sync& completer) final;
  void GetBackingMemory(GetBackingMemoryRequestView request,
                        GetBackingMemoryCompleter::Sync& completer) final;

  void AdvisoryLock(AdvisoryLockRequestView request, AdvisoryLockCompleter::Sync& completer) final;
  void Clone(CloneRequestView request, CloneCompleter::Sync& completer) final;
  void CloseDeprecated(CloseDeprecatedRequestView request,
                       CloseDeprecatedCompleter::Sync& completer) final;
  void Close(CloseRequestView request, CloseCompleter::Sync& completer) final;
  void Describe(DescribeRequestView request, DescribeCompleter::Sync& completer) final;
  void Describe2(Describe2RequestView request, Describe2Completer::Sync& completer) final;
  void SyncDeprecated(SyncDeprecatedRequestView request,
                      SyncDeprecatedCompleter::Sync& completer) final;
  void Sync(SyncRequestView request, SyncCompleter::Sync& completer) final;
  void GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) final;
  void SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) final;
  void GetFlags(GetFlagsRequestView request, GetFlagsCompleter::Sync& completer) final;
  void SetFlags(SetFlagsRequestView request, SetFlagsCompleter::Sync& completer) final;
  void QueryFilesystem(QueryFilesystemRequestView request,
                       QueryFilesystemCompleter::Sync& completer) final;

 private:
  fbl::RefPtr<PtyClient> client_;
};

#endif  // SRC_BRINGUP_BIN_PTYSVC_PTY_CLIENT_DEVICE_H_
