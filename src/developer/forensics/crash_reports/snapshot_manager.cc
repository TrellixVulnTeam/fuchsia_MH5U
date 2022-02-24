// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/snapshot_manager.h"

#include <lib/async/cpp/task.h>
#include <lib/fpromise/bridge.h>
#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <vector>

#include "src/developer/forensics/crash_reports/errors.h"
#include "src/lib/uuid/uuid.h"

namespace forensics {
namespace crash_reports {
namespace {

using fuchsia::feedback::Annotation;
using fuchsia::feedback::GetSnapshotParameters;
using FidlSnapshot = fuchsia::feedback::Snapshot;

template <typename V>
void AddAnnotation(const std::string& key, const V& value, FidlSnapshot* snapshot) {
  snapshot->mutable_annotations()->push_back(Annotation{
      .key = key,
      .value = std::to_string(value),
  });
}

template <>
void AddAnnotation<std::string>(const std::string& key, const std::string& value,
                                FidlSnapshot* snapshot) {
  snapshot->mutable_annotations()->push_back(Annotation{
      .key = key,
      .value = value,
  });
}

AnnotationMap ToAnnotationMap(const std::vector<Annotation>& annotations) {
  AnnotationMap map;
  map.Set(annotations);
  return map;
}

// Helper function to make a shared_ptr from a rvalue-reference of a type.
template <typename T>
std::shared_ptr<T> MakeShared(T&& t) {
  return std::make_shared<T>(static_cast<std::remove_reference_t<T>&&>(t));
}

}  // namespace

SnapshotManager::SnapshotManager(async_dispatcher_t* dispatcher, timekeeper::Clock* clock,
                                 fuchsia::feedback::DataProvider* data_provider,
                                 zx::duration shared_request_window,
                                 const std::string& garbage_collected_snapshots_path,
                                 StorageSize max_annotations_size, StorageSize max_archives_size)
    : dispatcher_(dispatcher),
      clock_(clock),
      data_provider_(data_provider),
      shared_request_window_(shared_request_window),
      garbage_collected_snapshots_path_(garbage_collected_snapshots_path),
      max_annotations_size_(max_annotations_size),
      current_annotations_size_(0u),
      max_archives_size_(max_archives_size),
      current_archives_size_(0u),
      garbage_collected_snapshot_("garbage collected"),
      not_persisted_snapshot_("not persisted"),
      timed_out_snapshot_("timed out"),
      shutdown_snapshot_("shutdown"),
      no_uuid_snapshot_(UuidForNoSnapshotUuid()) {
  garbage_collected_snapshot_.annotations->Set("debug.snapshot.error", "garbage collected");
  garbage_collected_snapshot_.annotations->Set("debug.snapshot.present", false);

  not_persisted_snapshot_.annotations->Set("debug.snapshot.error", "not persisted");
  not_persisted_snapshot_.annotations->Set("debug.snapshot.present", false);

  timed_out_snapshot_.annotations->Set("debug.snapshot.error", "timeout");
  timed_out_snapshot_.annotations->Set("debug.snapshot.present", false);

  shutdown_snapshot_.annotations->Set("debug.snapshot.error", "system shutdown");
  shutdown_snapshot_.annotations->Set("debug.snapshot.present", false);

  no_uuid_snapshot_.annotations->Set("debug.snapshot.error", "missing uuid");
  no_uuid_snapshot_.annotations->Set("debug.snapshot.present", false);

  // Load the file lines into a set of UUIDs.
  std::ifstream file(garbage_collected_snapshots_path_);
  for (std::string uuid; getline(file, uuid);) {
    garbage_collected_snapshots_.insert(uuid);
  }
}

Snapshot SnapshotManager::GetSnapshot(const SnapshotUuid& uuid) {
  if (uuid == garbage_collected_snapshot_.uuid) {
    return Snapshot(garbage_collected_snapshot_.annotations);
  }

  if (uuid == not_persisted_snapshot_.uuid) {
    return Snapshot(not_persisted_snapshot_.annotations);
  }

  if (uuid == timed_out_snapshot_.uuid) {
    return Snapshot(timed_out_snapshot_.annotations);
  }

  if (uuid == shutdown_snapshot_.uuid) {
    return Snapshot(shutdown_snapshot_.annotations);
  }

  if (uuid == no_uuid_snapshot_.uuid) {
    return Snapshot(no_uuid_snapshot_.annotations);
  }

  auto* data = FindSnapshotData(uuid);

  if (!data) {
    if (garbage_collected_snapshots_.find(uuid) != garbage_collected_snapshots_.end()) {
      return Snapshot(garbage_collected_snapshot_.annotations);
    } else {
      return Snapshot(not_persisted_snapshot_.annotations);
    }
  }

  return Snapshot(data->annotations, data->archive);
}

::fpromise::promise<SnapshotUuid> SnapshotManager::GetSnapshotUuid(zx::duration timeout) {
  const zx::time current_time{clock_->Now()};

  SnapshotUuid uuid;

  if (UseLatestRequest()) {
    uuid = requests_.back()->uuid;
  } else {
    uuid = MakeNewSnapshotRequest(current_time, timeout);
  }

  auto* data = FindSnapshotData(uuid);
  FX_CHECK(data);

  data->num_clients_with_uuid += 1;

  const zx::time deadline = current_time + timeout;

  // The snapshot for |uuid| may not be ready, so the logic for returning |uuid| to the client
  // needs to be wrapped in an asynchronous task that can be re-executed when the conditions for
  // returning a UUID are met, e.g., the snapshot for |uuid| is received from |data_provider_| or
  // the call to GetSnapshotUuid times out.
  return ::fpromise::make_promise(
      [this, uuid, deadline](::fpromise::context& context) -> ::fpromise::result<SnapshotUuid> {
        if (shutdown_) {
          return ::fpromise::ok(shutdown_snapshot_.uuid);
        }

        auto request = FindSnapshotRequest(uuid);

        // The request and its data were deleted before the promise executed. This should only occur
        // if a snapshot is dropped immediately after it is received because its annotations and
        // archive are too large and it is one of the oldest in the FIFO.
        if (!request) {
          return ::fpromise::ok(garbage_collected_snapshot_.uuid);
        }

        if (!request->is_pending) {
          return ::fpromise::ok(request->uuid);
        }

        if (clock_->Now() >= deadline) {
          return ::fpromise::ok(timed_out_snapshot_.uuid);
        }

        WaitForSnapshot(uuid, deadline, context.suspend_task());
        return ::fpromise::pending();
      });
}

void SnapshotManager::Release(const SnapshotUuid& uuid) {
  if (uuid == garbage_collected_snapshot_.uuid || uuid == not_persisted_snapshot_.uuid ||
      uuid == timed_out_snapshot_.uuid || uuid == no_uuid_snapshot_.uuid) {
    return;
  }

  auto* data = FindSnapshotData(uuid);

  // The snapshot was likely dropped due to size constraints.
  if (!data) {
    return;
  }

  data->num_clients_with_uuid -= 1;

  // There are still clients that need the snapshot.
  if (data->num_clients_with_uuid > 0) {
    return;
  }

  DropAnnotations(data);
  DropArchive(data);

  // No calls to GetUuid should be blocked.
  if (auto request = FindSnapshotRequest(uuid); request) {
    FX_CHECK(request->blocked_promises.empty());
  }

  requests_.erase(std::remove_if(
      requests_.begin(), requests_.end(),
      [uuid](const std::unique_ptr<SnapshotRequest>& request) { return uuid == request->uuid; }));
  RecordAsGarbageCollected(uuid);
  data_.erase(uuid);
}

void SnapshotManager::Shutdown() {
  // Unblock all pending promises to return |shutdown_snapshot_|.
  shutdown_ = true;
  for (auto& request : requests_) {
    if (!request->is_pending) {
      continue;
    }

    for (auto& blocked_promise : request->blocked_promises) {
      if (blocked_promise) {
        blocked_promise.resume_task();
      }
    }
    request->blocked_promises.clear();
  }
}

SnapshotUuid SnapshotManager::MakeNewSnapshotRequest(const zx::time start_time,
                                                     const zx::duration timeout) {
  const auto uuid = uuid::Generate();
  requests_.emplace_back(std::unique_ptr<SnapshotRequest>(new SnapshotRequest{
      .uuid = uuid,
      .is_pending = true,
      .blocked_promises = {},
      .delayed_get_snapshot = async::TaskClosure(),
  }));
  data_.emplace(uuid, SnapshotData{
                          .num_clients_with_uuid = 0,
                          .annotations_size = StorageSize::Bytes(0u),
                          .archive_size = StorageSize::Bytes(0u),
                          .annotations = nullptr,
                          .archive = nullptr,
                      });

  requests_.back()->delayed_get_snapshot.set_handler([this, timeout, uuid]() {
    GetSnapshotParameters params;

    // Give 15s for the packaging of the snapshot and the round-trip between the client and
    // the server and the rest is given to each data collection.
    params.set_collection_timeout_per_data((timeout - zx::sec(15)).get());

    data_provider_->GetSnapshot(std::move(params), [this, uuid](FidlSnapshot snapshot) {
      CompleteWithSnapshot(uuid, std::move(snapshot));
      EnforceSizeLimits();
    });
  });
  requests_.back()->delayed_get_snapshot.PostForTime(dispatcher_,
                                                     start_time + shared_request_window_);

  return uuid;
}

void SnapshotManager::WaitForSnapshot(const SnapshotUuid& uuid, zx::time deadline,
                                      ::fpromise::suspended_task get_uuid_promise) {
  auto* request = FindSnapshotRequest(uuid);
  if (!request) {
    get_uuid_promise.resume_task();
    return;
  }

  request->blocked_promises.push_back(std::move(get_uuid_promise));
  const size_t idx = request->blocked_promises.size() - 1;

  // Resume |get_uuid_promise| after |deadline| has passed.
  if (const zx_status_t status = async::PostTaskForTime(
          dispatcher_,
          [this, idx, uuid] {
            if (auto* request = FindSnapshotRequest(uuid); request && request->is_pending) {
              FX_CHECK(idx < request->blocked_promises.size());
              if (request->blocked_promises[idx]) {
                request->blocked_promises[idx].resume_task();
              }
            }
          },
          deadline);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to post async task";

    // Immediately resume the promise if posting the task fails.
    request->blocked_promises.back().resume_task();
    request->blocked_promises.pop_back();
  }
}

void SnapshotManager::CompleteWithSnapshot(const SnapshotUuid& uuid, FidlSnapshot fidl_snapshot) {
  auto* request = FindSnapshotRequest(uuid);
  auto* data = FindSnapshotData(uuid);

  // A pending request shouldn't be deleted.
  FX_CHECK(request);
  FX_CHECK(data);
  FX_CHECK(request->is_pending);

  // Add debug annotations.
  if (fidl_snapshot.IsEmpty()) {
    AddAnnotation("debug.snapshot.present", std::string("false"), &fidl_snapshot);
  }
  AddAnnotation("debug.snapshot.shared-request.num-clients", data->num_clients_with_uuid,
                &fidl_snapshot);
  AddAnnotation("debug.snapshot.shared-request.uuid", request->uuid, &fidl_snapshot);

  // Take ownership of |fidl_snapshot| and the record the size of its annotations and archive.
  if (fidl_snapshot.has_annotations()) {
    data->annotations = MakeShared(ToAnnotationMap(fidl_snapshot.annotations()));

    for (const auto& [k, v] : data->annotations->Raw()) {
      data->annotations_size += StorageSize::Bytes(k.size());
      data->annotations_size += StorageSize::Bytes(v.size());
    }
    current_annotations_size_ += data->annotations_size;
  }

  if (fidl_snapshot.has_archive()) {
    data->archive = MakeShared(Snapshot::Archive(fidl_snapshot.archive()));

    data->archive_size += StorageSize::Bytes(data->archive->key.size());
    data->archive_size += StorageSize::Bytes(data->archive->value.size());
    current_archives_size_ += data->archive_size;
  }

  // The request is completed and unblock all promises that need |snapshot|.
  request->is_pending = false;
  for (auto& blocked_promise : request->blocked_promises) {
    if (blocked_promise) {
      blocked_promise.resume_task();
    }
  }
  request->blocked_promises.clear();
}

void SnapshotManager::EnforceSizeLimits() {
  std::vector<std::unique_ptr<SnapshotRequest>> surviving_requests;
  for (auto& request : requests_) {
    // If the request is pending or the size limits aren't exceeded, keep the request.
    if (request->is_pending || (current_annotations_size_ <= max_annotations_size_ &&
                                current_archives_size_ <= max_archives_size_)) {
      surviving_requests.push_back(std::move(request));

      // Continue in order to keep the rest of the requests alive.
      continue;
    }

    auto* data = FindSnapshotData(request->uuid);
    FX_CHECK(data);

    // Drop |request|'s annotations if necessary.
    if (current_annotations_size_ > max_annotations_size_) {
      DropAnnotations(data);
    }

    // Drop |request|'s archive if necessary.
    if (current_archives_size_ > max_archives_size_) {
      DropArchive(data);
      RecordAsGarbageCollected(request->uuid);
    }

    // Delete the SnapshotRequest and SnapshotData if the annotations and archive have been
    // dropped, either in this iteration of the loop or a prior one.
    if (!data->annotations && !data->archive) {
      RecordAsGarbageCollected(request->uuid);
      data_.erase(request->uuid);
      continue;
    }

    surviving_requests.push_back(std::move(request));
  }

  requests_.swap(surviving_requests);
}

void SnapshotManager::DropAnnotations(SnapshotData* data) {
  data->annotations = nullptr;

  current_annotations_size_ -= data->annotations_size;
  data->annotations_size = StorageSize::Bytes(0u);
}

void SnapshotManager::DropArchive(SnapshotData* data) {
  data->archive = nullptr;

  current_archives_size_ -= data->archive_size;
  data->archive_size = StorageSize::Bytes(0u);

  // If annotations still exist, add an annotation indicating the archive was garbage collected.
  if (data->annotations) {
    for (const auto& [k, v] : garbage_collected_snapshot_.annotations->Raw()) {
      data->annotations->Set(k, v);
      data->annotations_size += StorageSize::Bytes(k.size()) + StorageSize::Bytes(v.size());
      current_annotations_size_ += StorageSize::Bytes(k.size()) + StorageSize::Bytes(v.size());
    }
  }
}

void SnapshotManager::RecordAsGarbageCollected(const SnapshotUuid& uuid) {
  if (garbage_collected_snapshots_.find(uuid) != garbage_collected_snapshots_.end()) {
    return;
  }

  garbage_collected_snapshots_.insert(uuid);

  // Append the UUID to the file on its own line.
  std::ofstream file(garbage_collected_snapshots_path_, std::ofstream::out | std::ofstream::app);
  file << uuid << "\n";
  file.close();
}

bool SnapshotManager::UseLatestRequest() const {
  if (requests_.empty()) {
    return false;
  }

  // Whether the FIDL call for the latest request has already been made or not. If it has, the
  // snapshot might not contain all the logs up until now for instance so it's better to create a
  // new request.
  return requests_.back()->delayed_get_snapshot.is_pending();
}

SnapshotManager::SnapshotRequest* SnapshotManager::FindSnapshotRequest(const SnapshotUuid& uuid) {
  auto request = std::find_if(
      requests_.begin(), requests_.end(),
      [uuid](const std::unique_ptr<SnapshotRequest>& request) { return uuid == request->uuid; });
  return (request == requests_.end()) ? nullptr : request->get();
}

SnapshotManager::SnapshotData* SnapshotManager::FindSnapshotData(const SnapshotUuid& uuid) {
  return (data_.find(uuid) == data_.end()) ? nullptr : &(data_.at(uuid));
}

}  // namespace crash_reports
}  // namespace forensics
