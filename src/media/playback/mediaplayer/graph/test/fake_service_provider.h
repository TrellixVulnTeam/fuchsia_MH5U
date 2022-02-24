// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TEST_FAKE_SERVICE_PROVIDER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TEST_FAKE_SERVICE_PROVIDER_H_

#include <fuchsia/sysmem/cpp/fidl_test_base.h>

#include <unordered_map>

#include "lib/fidl/cpp/binding_set.h"
#include "lib/gtest/test_loop_fixture.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"
#include "src/media/playback/mediaplayer/graph/payloads/payload_manager.h"

namespace media_player::test {

class FakeServiceProvider;

class FakeBufferCollection : public fuchsia::sysmem::testing::BufferCollection_TestBase {
 public:
  explicit FakeBufferCollection(FakeServiceProvider* owner);

  ~FakeBufferCollection() override;

  void Bind(fidl::InterfaceRequest<fuchsia::sysmem::BufferCollection> request);

  const std::vector<fuchsia::sysmem::BufferCollectionConstraints>& constraints() const {
    return constraints_;
  }

  void SetBufferCollection(zx_status_t status,
                           fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info);

 private:
  // fuchsia::sysmem::BufferCollection implementation.
  void Sync(SyncCallback callback) override;

  void SetConstraints(bool has_constraints,
                      fuchsia::sysmem::BufferCollectionConstraints constraints) override;

  void WaitForBuffersAllocated(WaitForBuffersAllocatedCallback callback) override;

  void CheckBuffersAllocated(CheckBuffersAllocatedCallback callback) override;

  void SetName(uint32_t priority, std::string name) override {}
  void SetDebugClientInfo(std::string name, uint64_t id) override {}
  void NotImplemented_(const std::string& name) override;

  FakeServiceProvider* owner_;
  fidl::BindingSet<fuchsia::sysmem::BufferCollection> bindings_;
  std::vector<fuchsia::sysmem::BufferCollectionConstraints> constraints_;
  zx_status_t buffer_allocation_status_ = ZX_ERR_UNAVAILABLE;
  std::vector<WaitForBuffersAllocatedCallback> wait_callbacks_;
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info_;
};

class FakeBufferCollectionToken : public fuchsia::sysmem::testing::BufferCollectionToken_TestBase {
 public:
  explicit FakeBufferCollectionToken(FakeServiceProvider* owner);
  ~FakeBufferCollectionToken() override;

  void Bind(fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken> request);

 private:
  // fuchsia::sysmem::BufferCollectionToken implementation.
  void Duplicate(uint32_t rights_attenuation_mask,
                 fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken> request) override;

  void Sync(SyncCallback callback) override;

  void SetDebugClientInfo(std::string name, uint64_t id) override {}
  void SetDebugTimeoutLogDeadline(int64_t deadline) override {}
  void SetDispensable() override {}
  void NotImplemented_(const std::string& name) override;

  FakeServiceProvider* owner_;
  fidl::BindingSet<fuchsia::sysmem::BufferCollectionToken> bindings_;
};

class FakeServiceProvider : public ServiceProvider,
                            public fuchsia::sysmem::testing::Allocator_TestBase {
 public:
  FakeBufferCollection* GetCollectionFromToken(fuchsia::sysmem::BufferCollectionTokenPtr token);

  ~FakeServiceProvider() override;

  // Methods called by FakeBufferCollection and FakeBufferCollectionToken.
  void AddTokenBinding(FakeBufferCollectionToken* token, const zx::channel& channel);

  void RemoveToken(FakeBufferCollectionToken* token);

  void RemoveCollection(FakeBufferCollection* collection);

  // ServiceProvider implementation.
  void ConnectToService(std::string service_path, zx::channel channel) override;

 private:
  // fuchsia::sysmem::Allocator implementation.
  void AllocateSharedCollection(
      fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken> token_request) override;

  void BindSharedCollection(
      fidl::InterfaceHandle<class fuchsia::sysmem::BufferCollectionToken> token,
      fidl::InterfaceRequest<fuchsia::sysmem::BufferCollection> buffer_collection_request) override;

  void ValidateBufferCollectionToken(
      uint64_t token_server_koid,
      fuchsia::sysmem::Allocator::ValidateBufferCollectionTokenCallback callback) override;
  void SetDebugClientInfo(std::string name, uint64_t id) override {}
  void NotImplemented_(const std::string& name) override;

  FakeBufferCollection* FindOrCreateCollectionForToken(zx::channel client_channel);

  fidl::BindingSet<fuchsia::sysmem::Allocator> bindings_;
  std::unordered_map<FakeBufferCollectionToken*, std::unique_ptr<FakeBufferCollectionToken>>
      tokens_;
  std::unordered_map<zx_koid_t, FakeBufferCollectionToken*> tokens_by_server_koid_;
  std::unordered_map<FakeBufferCollection*, std::unique_ptr<FakeBufferCollection>> collections_;
  std::unordered_map<FakeBufferCollectionToken*, FakeBufferCollection*> collections_by_token_;
};

}  // namespace media_player::test

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TEST_FAKE_SERVICE_PROVIDER_H_
