// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_STORE_H_
#define SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_STORE_H_

#include <optional>
#include <string>
#include <vector>

#include "src/developer/forensics/crash_reports/info/info_context.h"
#include "src/developer/forensics/crash_reports/info/store_info.h"
#include "src/developer/forensics/crash_reports/log_tags.h"
#include "src/developer/forensics/crash_reports/report.h"
#include "src/developer/forensics/crash_reports/report_id.h"
#include "src/developer/forensics/crash_reports/store_metadata.h"
#include "src/developer/forensics/utils/storage_size.h"

namespace forensics {
namespace crash_reports {

// Stores the contents of reports that have not yet been uploaded.
class Store {
 public:
  // A directory to store snapshots under and the maximum amount of data that can be stored under
  // that directory before garbage collection or adds fail.
  struct Root {
    std::string dir;
    StorageSize max_size;
  };

  // |temp_root| is where reports that don't need to survive a device reboot should be stored
  // whereas reports that need to do will be stored under |persistent_root|.
  //
  // Regardless of which is actually used, reports will be stored in a similar manner. For example,
  // if a report is filed for "foo" and it is determined that it will be stored under |temp_root|,
  // that report will be stored in the filesytem under |temp_root|.dir/foo/<report ReportId>.
  Store(LogTags* tags, std::shared_ptr<InfoContext> info_context, const Root& temp_root,
        const Root& persistent_root);

  // Adds a report to the store and returns the ReportIds of any report garbage collected in the
  // process.
  bool Add(Report report, std::vector<ReportId>* garbage_collected_reports);

  // Gets a report from the store. If no report exists for |id| or there is an error reading the
  // report from the filesystem, return std::nullopt.
  Report Get(ReportId id);

  // Returns true if a report with ReportId |id| is removed from the store.
  bool Remove(ReportId id);

  void RemoveAll();

  std::vector<ReportId> GetReports() const;
  SnapshotUuid GetSnapshotUuid(ReportId id);

  bool Contains(ReportId id);

 private:
  bool Add(ReportId report_id, const std::string& program_shortname, StorageSize report_size,
           const std::map<std::string, SizedData>& attachments, StoreMetadata& store_root,
           std::vector<ReportId>* garbage_collected_reports);

  // Recreates |store_root| from the filesystem and initializes necessary state.
  bool RecreateFromFilesystem(StoreMetadata& store_root);

  // The root that the report with ReportId |id| is stored under.
  StoreMetadata& RootFor(ReportId id);

  // Pick the root to store a report with size of |report_size| under.
  StoreMetadata& PickRootForStorage(StorageSize report_size);

  // Returns true if another storage root can be used.
  bool HasFallbackRoot(const StoreMetadata& store_root);

  // Returns a storage root that can be used if |store_root| fails.
  StoreMetadata& FallbackRoot(StoreMetadata& store_root);

  // Removes reports until |required_space| is free under |root_metadata| and returns the ReportIds
  // of the reports removed.
  //
  // Return false if |required_space| cannot be freed.
  bool MakeFreeSpace(const StoreMetadata& root_metadata, StorageSize required_space,
                     std::vector<ReportId>* garbage_collected_reports);

  StoreMetadata tmp_metadata_;
  StoreMetadata cache_metadata_;
  LogTags* tags_;
  StoreInfo info_;
};

}  // namespace crash_reports
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_STORE_H_
