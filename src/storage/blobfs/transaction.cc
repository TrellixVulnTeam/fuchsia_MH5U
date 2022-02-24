// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/transaction.h"

#include <lib/syslog/cpp/macros.h>

namespace blobfs {

void BlobTransaction::Commit(fs::Journal& journal, fpromise::promise<void, zx_status_t> data,
                             fit::callback<void()> callback) {
  zx_status_t status = journal.CommitTransaction({
      .metadata_operations = operations_.TakeOperations(),
      .data_promise = std::move(data),
      .trim = std::move(trim_),
      // We reserve extents (by capturing in a lambda) until after the trim has completed.
      .commit_callback = [reserved_extents = std::move(reserved_extents_)] {},
      .complete_callback = std::move(callback),
  });
  // We can't do much except log the error.
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "BlobTransaction::Commit failed: " << zx_status_get_string(status);
  }
}

}  // namespace blobfs
