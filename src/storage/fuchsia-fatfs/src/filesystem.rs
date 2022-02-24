// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::{
        directory::{FatDirectory, InsensitiveStringRef},
        node::{Closer, FatNode, Node},
        refs::FatfsDirRef,
        types::{Dir, Disk, FileSystem},
        util::fatfs_error_to_status,
        FATFS_INFO_NAME, MAX_FILENAME_LEN, VFS_TYPE_FATFS,
    },
    anyhow::Error,
    async_trait::async_trait,
    fatfs::{self, validate_filename, DefaultTimeProvider, FsOptions, LossyOemCpConverter},
    fidl_fuchsia_io::FilesystemInfo,
    fuchsia_async::{Task, Time, Timer},
    fuchsia_zircon::{AsHandleRef, Event},
    fuchsia_zircon::{Duration, Status},
    std::{
        any::Any,
        marker::PhantomPinned,
        pin::Pin,
        sync::{Arc, LockResult, Mutex, MutexGuard},
    },
    vfs::{
        filesystem::{Filesystem, FilesystemRename},
        path::Path,
    },
};

pub struct FatFilesystemInner {
    filesystem: Option<FileSystem>,
    // We don't implement unpin: we want `filesystem` to be pinned so that we can be sure
    // references to filesystem objects (see refs.rs) will remain valid across different locks.
    _pinned: PhantomPinned,
}

impl FatFilesystemInner {
    /// Get the root fatfs Dir.
    pub fn root_dir(&self) -> Dir<'_> {
        self.filesystem.as_ref().unwrap().root_dir()
    }

    pub fn with_disk<F, T>(&self, func: F) -> T
    where
        F: FnOnce(&Box<dyn Disk>) -> T,
    {
        self.filesystem.as_ref().unwrap().with_disk(func)
    }

    pub fn shut_down(&mut self) -> Result<(), Status> {
        self.filesystem.take().ok_or(Status::BAD_STATE)?.unmount().map_err(fatfs_error_to_status)
    }

    pub fn cluster_size(&self) -> u32 {
        self.filesystem.as_ref().map_or(0, |f| f.cluster_size())
    }

    pub fn total_clusters(&self) -> Result<u32, Status> {
        Ok(self
            .filesystem
            .as_ref()
            .ok_or(Status::BAD_STATE)?
            .stats()
            .map_err(fatfs_error_to_status)?
            .total_clusters())
    }

    pub fn free_clusters(&self) -> Result<u32, Status> {
        Ok(self
            .filesystem
            .as_ref()
            .ok_or(Status::BAD_STATE)?
            .stats()
            .map_err(fatfs_error_to_status)?
            .free_clusters())
    }

    pub fn sector_size(&self) -> Result<u16, Status> {
        Ok(self
            .filesystem
            .as_ref()
            .ok_or(Status::BAD_STATE)?
            .stats()
            .map_err(fatfs_error_to_status)?
            .sector_size())
    }
}

pub struct FatFilesystem {
    inner: Mutex<FatFilesystemInner>,
    dirty_task: Mutex<Option<(Time, Task<()>)>>,
    fs_id: Event,
}

impl FatFilesystem {
    /// Create a new FatFilesystem.
    pub fn new(
        disk: Box<dyn Disk>,
        options: FsOptions<DefaultTimeProvider, LossyOemCpConverter>,
    ) -> Result<(Pin<Arc<Self>>, Arc<FatDirectory>), Error> {
        let inner = Mutex::new(FatFilesystemInner {
            filesystem: Some(fatfs::FileSystem::new(disk, options)?),
            _pinned: PhantomPinned,
        });
        let result = Arc::pin(FatFilesystem {
            inner,
            dirty_task: Mutex::new(None),
            fs_id: Event::create()?,
        });
        Ok((result.clone(), result.root_dir()))
    }

    #[cfg(test)]
    pub fn from_filesystem(filesystem: FileSystem) -> (Pin<Arc<Self>>, Arc<FatDirectory>) {
        let inner =
            Mutex::new(FatFilesystemInner { filesystem: Some(filesystem), _pinned: PhantomPinned });
        let result = Arc::pin(FatFilesystem {
            inner,
            dirty_task: Mutex::new(None),
            fs_id: Event::create().unwrap(),
        });
        (result.clone(), result.root_dir())
    }

    pub fn fs_id(&self) -> &Event {
        &self.fs_id
    }

    /// Get the FatDirectory that represents the root directory of this filesystem.
    /// Note this should only be called once per filesystem, otherwise multiple conflicting
    /// FatDirectories will exist.
    /// We only call it from new() and from_filesystem().
    fn root_dir(self: Pin<Arc<Self>>) -> Arc<FatDirectory> {
        // We start with an empty FatfsDirRef and an open_count of zero.
        let dir = FatfsDirRef::empty();
        FatDirectory::new(dir, None, self, "/".to_owned())
    }

    /// Try and lock the underlying filesystem. Returns a LockResult, see `Mutex::lock`.
    pub fn lock(&self) -> LockResult<MutexGuard<'_, FatFilesystemInner>> {
        self.inner.lock()
    }

    /// Mark the filesystem as dirty. This will cause the disk to automatically be flushed after
    /// one second, and cancel any previous pending flushes.
    pub fn mark_dirty(self: &Pin<Arc<Self>>) {
        let deadline = Time::after(Duration::from_seconds(1));
        match &mut *self.dirty_task.lock().unwrap() {
            Some((time, _)) => *time = deadline,
            x @ None => {
                let this = self.clone();
                *x = Some((
                    deadline,
                    Task::spawn(async move {
                        loop {
                            let deadline;
                            {
                                let mut task = this.dirty_task.lock().unwrap();
                                deadline = task.as_ref().unwrap().0;
                                if Time::now() >= deadline {
                                    *task = None;
                                    break;
                                }
                            }
                            Timer::new(deadline).await;
                        }
                        let _ = this.lock().unwrap().filesystem.as_ref().map(|f| f.flush());
                    }),
                ));
            }
        }
    }

    /// Do a simple rename of the file, without unlinking dst.
    /// This assumes that either "dst" and "src" are the same file, or that "dst" has already been
    /// unlinked.
    fn rename_internal(
        &self,
        filesystem: &FatFilesystemInner,
        src_dir: &Arc<FatDirectory>,
        src_name: &str,
        dst_dir: &Arc<FatDirectory>,
        dst_name: &str,
        existing: ExistingRef<'_, '_>,
    ) -> Result<(), Status> {
        // We're ready to go: remove the entry from the source cache, and close the reference to
        // the underlying file (this ensures all pending writes, etc. have been flushed).
        // We remove the entry with rename() below, and hold the filesystem lock so nothing will
        // put the entry back in the cache. After renaming we also re-attach the entry to its
        // parent.

        // Do the rename.
        let src_fatfs_dir = src_dir.borrow_dir(&filesystem)?;
        let dst_fatfs_dir = dst_dir.borrow_dir(&filesystem)?;

        match existing {
            ExistingRef::None => {
                src_fatfs_dir
                    .rename(src_name, &dst_fatfs_dir, dst_name)
                    .map_err(fatfs_error_to_status)?;
            }
            ExistingRef::File(file) => {
                src_fatfs_dir
                    .rename_over_file(src_name, &dst_fatfs_dir, dst_name, file)
                    .map_err(fatfs_error_to_status)?;
            }
            ExistingRef::Dir(dir) => {
                src_fatfs_dir
                    .rename_over_dir(src_name, &dst_fatfs_dir, dst_name, dir)
                    .map_err(fatfs_error_to_status)?;
            }
        }

        src_dir.did_remove(src_name);
        dst_dir.did_add(dst_name);

        src_dir.fs().mark_dirty();

        // TODO: do the watcher event for existing.

        Ok(())
    }

    /// Helper for rename which returns FatNodes that need to be dropped without the fs lock held.
    fn rename_locked(
        &self,
        filesystem: &FatFilesystemInner,
        src_dir: &Arc<FatDirectory>,
        src_name: &str,
        dst_dir: &Arc<FatDirectory>,
        dst_name: &str,
        src_is_dir: bool,
        closer: &mut Closer<'_>,
    ) -> Result<(), Status> {
        // Renaming a file to itself is trivial, but we do it after we've checked that the file
        // exists and that src and dst have the same type.
        if Arc::ptr_eq(&src_dir, &dst_dir)
            && (&src_name as &dyn InsensitiveStringRef) == (&dst_name as &dyn InsensitiveStringRef)
        {
            if src_name != dst_name {
                // Cases don't match - we don't unlink, but we still need to fix the file's LFN.
                return self.rename_internal(
                    &filesystem,
                    src_dir,
                    src_name,
                    dst_dir,
                    dst_name,
                    ExistingRef::None,
                );
            }
            return Ok(());
        }

        if let Some(src_node) = src_dir.cache_get(src_name) {
            // We can't move a directory into itself.
            if let FatNode::Dir(ref dir) = src_node {
                if Arc::ptr_eq(&dir, &dst_dir) {
                    return Err(Status::INVALID_ARGS);
                }
            }
            src_node.flush_dir_entry(filesystem)?;
        }

        let mut dir;
        let mut file;
        let mut existing_node = dst_dir.cache_get(dst_name);
        let existing = match existing_node {
            None => {
                dst_dir.open_ref(filesystem)?;
                closer.add(FatNode::Dir(dst_dir.clone()));
                match dst_dir.find_child(filesystem, dst_name)? {
                    Some(ref dir_entry) => {
                        if dir_entry.is_dir() {
                            dir = Some(dir_entry.to_dir());
                            ExistingRef::Dir(dir.as_mut().unwrap())
                        } else {
                            file = Some(dir_entry.to_file());
                            ExistingRef::File(file.as_mut().unwrap())
                        }
                    }
                    None => ExistingRef::None,
                }
            }
            Some(ref mut node) => {
                node.open_ref(filesystem)?;
                closer.add(node.clone());
                match node {
                    FatNode::Dir(ref mut node_dir) => {
                        ExistingRef::Dir(node_dir.borrow_dir_mut(filesystem).unwrap())
                    }
                    FatNode::File(ref mut node_file) => {
                        ExistingRef::File(node_file.borrow_file_mut(filesystem).unwrap())
                    }
                }
            }
        };

        match existing {
            ExistingRef::File(_) => {
                if src_is_dir {
                    return Err(Status::NOT_DIR);
                }
            }
            ExistingRef::Dir(_) => {
                if !src_is_dir {
                    return Err(Status::NOT_FILE);
                }
            }
            ExistingRef::None => {}
        }

        self.rename_internal(&filesystem, src_dir, src_name, dst_dir, dst_name, existing)?;

        if let Some(_) = existing_node {
            dst_dir.cache_remove(&filesystem, &dst_name).unwrap().did_delete();
        }

        // We suceeded in renaming, so now move the nodes around.
        if let Some(node) = src_dir.remove_child(&filesystem, &src_name) {
            dst_dir
                .add_child(&filesystem, dst_name.to_owned(), node)
                .unwrap_or_else(|e| panic!("Rename failed, but fatfs says it didn't? - {:?}", e));
        }

        Ok(())
    }

    pub fn query_filesystem(&self) -> Result<FilesystemInfo, Status> {
        let fs_lock = self.lock().unwrap();

        let cluster_size = fs_lock.cluster_size() as u64;
        let total_clusters = fs_lock.total_clusters()? as u64;
        let free_clusters = fs_lock.free_clusters()? as u64;
        let total_bytes = cluster_size * total_clusters;
        let used_bytes = cluster_size * (total_clusters - free_clusters);

        Ok(FilesystemInfo {
            total_bytes,
            used_bytes,
            total_nodes: 0,
            used_nodes: 0,
            free_shared_pool_bytes: 0,
            fs_id: self.fs_id().get_koid()?.raw_koid(),
            block_size: cluster_size as u32,
            max_filename_size: MAX_FILENAME_LEN,
            fs_type: VFS_TYPE_FATFS,
            padding: 0,
            name: FATFS_INFO_NAME,
        })
    }
}

#[async_trait]
impl FilesystemRename for FatFilesystem {
    async fn rename(
        &self,
        src_dir: Arc<dyn Any + Sync + Send + 'static>,
        src_path: Path,
        dst_dir: Arc<dyn Any + Sync + Send + 'static>,
        dst_path: Path,
    ) -> Result<(), Status> {
        let src_dir = src_dir.downcast::<FatDirectory>().map_err(|_| Status::INVALID_ARGS)?;
        let dst_dir = dst_dir.downcast::<FatDirectory>().map_err(|_| Status::INVALID_ARGS)?;
        if dst_dir.is_deleted() {
            // Can't rename into a deleted folder.
            return Err(Status::NOT_FOUND);
        }

        let src_name = src_path.peek().unwrap();
        validate_filename(src_name).map_err(fatfs_error_to_status)?;
        let dst_name = dst_path.peek().unwrap();
        validate_filename(dst_name).map_err(fatfs_error_to_status)?;

        let mut closer = Closer::new(&self);
        let filesystem = self.inner.lock().unwrap();

        // Figure out if src is a directory.
        let entry = src_dir.find_child(&filesystem, &src_name)?;
        if entry.is_none() {
            // No such src (if we don't return NOT_FOUND here, fatfs will return it when we
            // call rename() later).
            return Err(Status::NOT_FOUND);
        }
        let src_is_dir = entry.unwrap().is_dir();
        if (dst_path.is_dir() || src_path.is_dir()) && !src_is_dir {
            // The caller wanted a directory (src or dst), but src is not a directory. This is
            // an error.
            return Err(Status::NOT_DIR);
        }

        self.rename_locked(
            &filesystem,
            &src_dir,
            src_name,
            &dst_dir,
            dst_name,
            src_is_dir,
            &mut closer,
        )
    }
}

impl Filesystem for FatFilesystem {
    fn block_size(&self) -> u32 {
        self.inner.lock().unwrap().cluster_size()
    }
}

pub(crate) enum ExistingRef<'a, 'b> {
    None,
    File(&'a mut crate::types::File<'b>),
    Dir(&'a mut crate::types::Dir<'b>),
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::tests::{TestDiskContents, TestFatDisk},
        fidl::endpoints::Proxy,
        fidl_fuchsia_io::{FileProxy, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE},
        scopeguard::defer,
        vfs::{directory::entry::DirectoryEntry, execution_scope::ExecutionScope, path::Path},
    };

    const TEST_DISK_SIZE: u64 = 2048 << 10; // 2048K

    #[fuchsia_async::run_singlethreaded(test)]
    #[ignore] // TODO(fxbug.dev/56138): Clean up tasks to prevent panic on drop in FatfsFileRef
    async fn test_automatic_flush() {
        let disk = TestFatDisk::empty_disk(TEST_DISK_SIZE);
        let structure = TestDiskContents::dir().add_child("test", "Hello".into());
        structure.create(&disk.root_dir());

        let fs = disk.into_fatfs();
        let dir = fs.get_fatfs_root();
        dir.open_ref(&fs.filesystem().lock().unwrap()).unwrap();
        defer! { dir.close_ref(&fs.filesystem().lock().unwrap()) };

        let scope = ExecutionScope::new();
        let (proxy, server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_io::NodeMarker>().unwrap();
        dir.clone().open(
            scope.clone(),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            0,
            Path::validate_and_split("test").unwrap(),
            server_end,
        );

        assert!(fs.filesystem().dirty_task.lock().unwrap().is_none());
        let file = FileProxy::new(proxy.into_channel().unwrap());
        file.write("hello there".as_bytes()).await.unwrap().map_err(Status::from_raw).unwrap();
        {
            let fs_lock = fs.filesystem().lock().unwrap();
            // fs should be dirty until the timer expires.
            assert!(fs_lock.filesystem.as_ref().unwrap().is_dirty());
        }
        // Wait some time for the flush to happen. Don't hold the lock while waiting, otherwise
        // the flush will get stuck waiting on the lock.
        Timer::new(Time::after(Duration::from_millis(1500))).await;
        {
            let fs_lock = fs.filesystem().lock().unwrap();
            assert_eq!(fs_lock.filesystem.as_ref().unwrap().is_dirty(), false);
        }
    }
}
