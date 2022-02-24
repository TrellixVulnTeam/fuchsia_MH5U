// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_SNAPSHOT_MANAGER_H_
#define SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_SNAPSHOT_MANAGER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/fpromise/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "src/developer/forensics/crash_reports/snapshot.h"
#include "src/developer/forensics/utils/storage_size.h"
#include "src/lib/timekeeper/clock.h"

namespace forensics {
namespace crash_reports {

using SnapshotUuid = std::string;

// Manages the collection, distribution, and lifetime of snapshots.
//
// To limit memory usage, the managed snapshots' annotations/archives cannot exceed
// |max_{annotations,archives}_size_| in size and snapshot manager will return the same Uuid to all
// calls to GetUuid that occur within  |shared_request_window_| of a
// fuchsia.feedback.DataProvider/GetSnapshot request.
//
// When space is constrained, SnapshotManager will drop the oldest annotations/archives it
// manages. Additionally, SnapshotManager tracks the number of clients that have received a specific
// Uuid from GetUuid and will automatically delete a snapshot when each client has called Release.
class SnapshotManager {
 public:
  SnapshotManager(async_dispatcher_t* dispatcher, timekeeper::Clock* clock,
                  fuchsia::feedback::DataProvider* data_provider,
                  zx::duration shared_request_window,
                  const std::string& garbage_collected_snapshots_path,
                  StorageSize max_annotations_size, StorageSize max_archives_size);

  // Returns a promise of a snapshot uuid for a snapshot that contains the most up-to-date system
  // data (a new snapshot will be created if all existing snapshots contain data that is
  // out-of-date). No uuid will be returned if |timeout| expires.
  ::fpromise::promise<SnapshotUuid> GetSnapshotUuid(zx::duration timeout);

  // Returns the snapshot for |uuid|, if one exists. If no snapshot exists for |uuid| a snapshot
  // containing annotations indicating the error will be returned.
  //
  // When a client no longer needs the data contained in a Snapshot, they should call Release to
  // inform the SnapshotManager. If all clients call release, the SnapshotManager will voluntarily
  // drop the Snapshot, freeing up space for new data.
  Snapshot GetSnapshot(const SnapshotUuid& uuid);

  // Tell SnapshotManager that a client no longer needs the snapshot for |uuid|. If the difference
  // between the number of calls to GetUuid and Release reaches 0, the snapshot for |uuid| will be
  // dropped by SnapshotManager.
  void Release(const SnapshotUuid& uuid);

  // Shuts down the snapshot manager by cancelling any pending FIDL calls and provides waiting
  // clients with a UUID for a generic "shutdown" snapshot.
  void Shutdown();

  // Returns a Uuid a client can use if it doesn't have one, e.g., it was previously stored in a
  // file and the file is gone.
  static SnapshotUuid UuidForNoSnapshotUuid() { return "no uuid"; }

 private:
  // State associated with an async call to fuchsia.feedback.DataProvider/GetSnapshot.
  struct SnapshotRequest {
    // The uuid of the request's snapshot.
    SnapshotUuid uuid;

    // Whether the request is pending.
    bool is_pending;

    // Promises that are waiting on the call to complete.
    std::vector<::fpromise::suspended_task> blocked_promises;

    // The actual request that we delay by |shared_request_window_| after the SnapshotRequest is
    // created.
    async::TaskClosure delayed_get_snapshot;
  };

  // State associated with a snapshot.
  //   * The number of clients with its uuid.
  //   * The size of its annotations.
  //   * The size of its archive.
  //   * The snapshot annotations.
  //   * The snapshot archive.
  struct SnapshotData {
    size_t num_clients_with_uuid;
    StorageSize annotations_size;
    StorageSize archive_size;
    std::shared_ptr<AnnotationMap> annotations;
    std::shared_ptr<Snapshot::Archive> archive;
  };

  // Determine if the most recent SnapshotRequest's delayed call to
  // fuchsia.feedback.DataProvider/GetSnapshopt has executed.
  bool UseLatestRequest() const;

  // Find the Snapshot{Request,Data} with Uuid |uuid|. If none exists, return nullptr.
  SnapshotRequest* FindSnapshotRequest(const SnapshotUuid& uuid);
  SnapshotData* FindSnapshotData(const SnapshotUuid& uuid);

  // Resume |get_uuid_promise| when |deadline| expires or request |uuid| completes with a snapshot.
  void WaitForSnapshot(const SnapshotUuid& uuid, zx::time deadline,
                       ::fpromise::suspended_task get_uuid_promise);

  // Make a call to fuchsia.feedback.DataProvider/GetSnapshot, started at |start_time|, and return
  // the Uuid of its eventual snapshot.
  SnapshotUuid MakeNewSnapshotRequest(zx::time start_time, zx::duration timeout);

  // Use |fidl_snapshot| to update all snapshot-related state with Uuid |uuid|.
  void CompleteWithSnapshot(const SnapshotUuid& uuid, fuchsia::feedback::Snapshot fidl_snapshot);

  // Remove the annotations and archives of the oldest requests, independently of one another, until
  // |current_annotations_size_| is less than or equal to |max_annotations_size_| and
  // |current_archives_size_| is less than or equal to |max_archives_size_|.
  //
  // Note: References into |requests_| and |data_| will be invalidated during this process. Be
  // cautious using the function!
  void EnforceSizeLimits();

  // Drop the {annotation,archive} for |uuid| and clean up state associated with them.
  void DropAnnotations(SnapshotData* data);
  void DropArchive(SnapshotData* data);

  void RecordAsGarbageCollected(const SnapshotUuid& uuid);

  async_dispatcher_t* dispatcher_;
  std::shared_ptr<sys::ServiceDirectory> services_;
  timekeeper::Clock* clock_;

  fuchsia::feedback::DataProvider* data_provider_;

  zx::duration shared_request_window_;

  std::string garbage_collected_snapshots_path_;

  StorageSize max_annotations_size_;
  StorageSize current_annotations_size_;

  StorageSize max_archives_size_;
  StorageSize current_archives_size_;

  std::vector<std::unique_ptr<SnapshotRequest>> requests_;
  std::map<SnapshotUuid, SnapshotData> data_;
  std::set<SnapshotUuid> garbage_collected_snapshots_;

  bool shutdown_{false};

  // SnapshotUuid and annotations to return under specific conditions, e.g., garbage collection,
  // time outs.
  struct SpecialCaseSnapshot {
    explicit SpecialCaseSnapshot(SnapshotUuid uuid)
        : uuid(std::move(uuid)), annotations(std::make_unique<AnnotationMap>()) {}
    SnapshotUuid uuid;
    std::shared_ptr<AnnotationMap> annotations;
  };

  SpecialCaseSnapshot garbage_collected_snapshot_;
  SpecialCaseSnapshot not_persisted_snapshot_;
  SpecialCaseSnapshot timed_out_snapshot_;
  SpecialCaseSnapshot shutdown_snapshot_;
  SpecialCaseSnapshot no_uuid_snapshot_;
};

}  // namespace crash_reports
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_SNAPSHOT_MANAGER_H_
