// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_FS_MANAGER_H_
#define SRC_STORAGE_FSHOST_FS_MANAGER_H_

#include <fidl/fuchsia.process.lifecycle/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/wait.h>
#include <lib/memfs/cpp/vnode.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <lib/zx/job.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <array>
#include <iterator>
#include <map>

#include "src/lib/loader_service/loader_service.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/storage/fshost/delayed-outdir.h"
#include "src/storage/fshost/fdio.h"
#include "src/storage/fshost/fshost-boot-args.h"
#include "src/storage/fshost/inspect-manager.h"
#include "src/storage/fshost/metrics.h"

namespace fshost {

class BlockWatcher;

// FsManager owns multiple sub-filesystems, managing them within a top-level
// in-memory filesystem.
class FsManager {
 public:
  explicit FsManager(std::shared_ptr<FshostBootArgs> boot_args,
                     std::unique_ptr<FsHostMetrics> metrics);
  ~FsManager();

  zx_status_t Initialize(fidl::ServerEnd<fuchsia_io::Directory> dir_request,
                         fidl::ServerEnd<fuchsia_process_lifecycle::Lifecycle> lifecycle_request,
                         std::shared_ptr<loader::LoaderServiceBase> loader, BlockWatcher& watcher);

  // TODO(fxbug.dev/39588): delete this
  // Starts servicing the delayed portion of the outgoing directory, called once
  // "/system" has been mounted.
  void FuchsiaStart() { delayed_outdir_.Start(); }

  // MountPoint is a possible location that a filesystem can be installed at.
  enum class MountPoint {
    kUnknown = 0,
    kBin,
    kData,
    kVolume,
    kSystem,
    kInstall,
    kPkgfs,
    kFactory,
    kDurable,
    kMnt,
  };

  // Returns the fully qualified for the given mount point.
  static const char* MountPointPath(MountPoint);

  constexpr static std::array<MountPoint, 9> kAllMountPoints{
      MountPoint::kBin,     MountPoint::kData,    MountPoint::kVolume,
      MountPoint::kSystem,  MountPoint::kInstall, MountPoint::kPkgfs,
      MountPoint::kFactory, MountPoint::kDurable, MountPoint::kMnt,
  };

  // Installs the filesystem with |root_directory| at |mount_point| (which must not already have an
  // installed filesystem).
  // |root_directory| should be a connection to a Directory, but this is not verified.
  zx::status<> InstallFs(MountPoint mount_point, std::string_view device_path,
                         zx::channel export_root_directory, zx::channel root_directory);

  // Serves connection to the root directory ("/") on |server|.
  zx_status_t ServeRoot(fidl::ServerEnd<fuchsia_io::Directory> server);

  // Asynchronously shut down all the filesystems managed by fshost and then signal the main thread
  // to exit. Calls |callback| when complete.
  void Shutdown(fit::function<void(zx_status_t)> callback);

  // Returns a pointer to the |FsHostMetrics| instance.
  FsHostMetrics* mutable_metrics() { return metrics_.get(); }

  InspectManager& inspect_manager() { return inspect_; }

  // Flushes FsHostMetrics to cobalt.
  void FlushMetrics();

  std::shared_ptr<FshostBootArgs> boot_args() { return boot_args_; }

  bool IsShutdown();
  void WaitForShutdown();

  // Creates a new subdirectory in the fshost diagnostics directory by the name of
  // |diagnostics_dir_name|, which forwards the diagnostics dir exposed in the export root directory
  // of the given filesystem previously installed via |InstallFs()| at |point|.
  zx_status_t ForwardFsDiagnosticsDirectory(MountPoint point,
                                            std::string_view diagnostics_dir_name);

  // Creates a new subdirectory in the fshost svc directory by the name of
  // |service_name|, which forwards the service by the same name exposed in the outgoing service
  // directory of the given filesystem previously installed via |InstallFs()| at |point|.
  zx_status_t ForwardFsService(MountPoint point, const char* service_name);

  // Disables reporting.  Future calls to |FileReport| will be NOPs.
  void DisableCrashReporting() { file_crash_report_ = false; }

  // Note that additional reasons should be added sparingly, and only in cases where the data is
  // useful and it would be difficult to debug the issue otherwise.
  enum ReportReason {
    kMinfsCorrupted,
    kMinfsNotUpgradeable,
  };

  // Files a synthetic crash report.  This is done in the background on a new thread, so never
  // blocks. Note that there is no indication if the reporting fails.
  void FileReport(ReportReason reason);

  zx_status_t AttachMount(std::string_view device_path,
                          fidl::ClientEnd<fuchsia_io::Directory> export_root,
                          std::string_view name);

  zx_status_t DetachMount(std::string_view name);

  zx::status<std::string> GetDevicePath(uint64_t fs_id) const;

 private:
  class MountedFilesystem {
   public:
    struct Compare {
      using is_transparent = void;
      bool operator()(const std::unique_ptr<MountedFilesystem>& a,
                      const std::unique_ptr<MountedFilesystem>& b) const {
        return a->name_ < b->name_;
      }

      bool operator()(const std::unique_ptr<MountedFilesystem>& a, std::string_view b) const {
        return a->name_ < b;
      }

      bool operator()(std::string_view a, const std::unique_ptr<MountedFilesystem>& b) const {
        return a < b->name_;
      }
    };

    MountedFilesystem(std::string_view name, fidl::ClientEnd<fuchsia_io::Directory> export_root,
                      fbl::RefPtr<fs::Vnode> node, uint64_t fs_id)
        : name_(name), export_root_(std::move(export_root)), node_(node), fs_id_(fs_id) {}
    ~MountedFilesystem();

    uint64_t fs_id() const { return fs_id_; }

   private:
    std::string name_;
    fidl::ClientEnd<fuchsia_io::Directory> export_root_;
    fbl::RefPtr<fs::Vnode> node_;
    uint64_t fs_id_;
  };

  zx_status_t SetupOutgoingDirectory(fidl::ServerEnd<fuchsia_io::Directory> dir_request,
                                     std::shared_ptr<loader::LoaderServiceBase> loader,
                                     BlockWatcher& watcher);

  zx_status_t SetupLifecycleServer(
      fidl::ServerEnd<fuchsia_process_lifecycle::Lifecycle> lifecycle_request);

  struct MountNode {
    // Set by |InstallFs()|.
    zx::channel export_root;
    fbl::RefPtr<fs::Vnode> root_directory;

    bool Installed() const { return export_root.is_valid(); }
  };
  std::map<MountPoint, MountNode> mount_nodes_;

  // The Root VFS manages the following filesystems:
  // - The global root filesystem (including the mount points)
  // - "/tmp"
  std::unique_ptr<memfs::Vfs> root_vfs_;

  std::unique_ptr<async::Loop> global_loop_;
  fs::ManagedVfs outgoing_vfs_;

  // The base, root directory which serves the rest of the fshost.
  fbl::RefPtr<memfs::VnodeDir> global_root_;

  // Keeps a collection of metrics being track at the FsHost level.
  std::unique_ptr<FsHostMetrics> metrics_;

  // Serves inspect data.
  InspectManager inspect_;

  // Used to lookup configuration options stored in fuchsia.boot.Arguments
  std::shared_ptr<fshost::FshostBootArgs> boot_args_;

  // The outgoing service directory for fshost.
  fbl::RefPtr<fs::PseudoDir> svc_dir_;

  // TODO(fxbug.dev/39588): delete this
  // A RemoteDir in the outgoing directory that ignores requests until Start is
  // called on it.
  DelayedOutdir delayed_outdir_;

  // The diagnostics directory for the fshost inspect tree.
  // Each filesystem gets a subdirectory to host their own inspect tree.
  // Archivist will parse all the inspect trees found in this directory tree.
  fbl::RefPtr<fs::PseudoDir> diagnostics_dir_;

  std::mutex lock_;
  bool shutdown_called_ TA_GUARDED(lock_) = false;
  sync_completion_t shutdown_;

  bool file_crash_report_ = true;

  std::set<std::unique_ptr<MountedFilesystem>, MountedFilesystem::Compare> mounted_filesystems_;
  std::unordered_map<uint64_t, std::string> device_paths_;
};

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_FS_MANAGER_H_
