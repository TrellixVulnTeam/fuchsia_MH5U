// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        errors::FxfsError,
        object_store::{
            directory::{self, Directory, ObjectDescriptor, ReplacedChild},
            transaction::{LockKey, Options},
            vmo_data_buffer::VmoDataBuffer,
            HandleOptions, HandleOwner, ObjectStore,
        },
        pager::Pager,
        server::{
            directory::FxDirectory,
            errors::map_to_status,
            file::FxFile,
            node::{FxNode, GetResult, NodeCache},
        },
    },
    anyhow::{bail, Error},
    async_trait::async_trait,
    fuchsia_async as fasync,
    fuchsia_zircon::Status,
    futures::{self, channel::oneshot, FutureExt},
    std::{
        any::Any,
        convert::TryInto,
        sync::{Arc, Mutex},
        time::Duration,
    },
    vfs::{
        filesystem::{Filesystem, FilesystemRename},
        path::Path,
    },
};

pub const DEFAULT_FLUSH_PERIOD: Duration = Duration::from_secs(20);

/// FxVolume represents an opened volume. It is also a (weak) cache for all opened Nodes within the
/// volume.
pub struct FxVolume {
    cache: NodeCache,
    store: Arc<ObjectStore>,
    pager: Pager,
    executor: fasync::EHandle,

    /// A tuple of the actual task and a channel to signal to terminate the task.
    flush_task: Mutex<Option<(fasync::Task<()>, oneshot::Sender<()>)>>,

    /// Unique identifier of the filesystem that owns this volume.
    fs_id: u64,
}

impl FxVolume {
    pub fn new(store: Arc<ObjectStore>, fs_id: u64) -> Result<Self, Error> {
        Ok(Self {
            cache: NodeCache::new(),
            store,
            pager: Pager::new()?,
            executor: fasync::EHandle::local(),
            flush_task: Mutex::new(None),
            fs_id,
        })
    }

    pub fn store(&self) -> &Arc<ObjectStore> {
        &self.store
    }

    pub fn cache(&self) -> &NodeCache {
        &self.cache
    }

    pub fn pager(&self) -> &Pager {
        &self.pager
    }

    pub fn executor(&self) -> &fasync::EHandle {
        &self.executor
    }

    pub fn id(&self) -> u64 {
        self.fs_id
    }

    pub async fn terminate(&self) {
        self.pager.terminate().await;
        let task = std::mem::replace(&mut *self.flush_task.lock().unwrap(), None);
        if let Some((task, terminate)) = task {
            let _ = terminate.send(());
            task.await;
        }
    }

    /// Attempts to get a node from the node cache. If the node wasn't present in the cache, loads
    /// the object from the object store, installing the returned node into the cache and returns the
    /// newly created FxNode backed by the loaded object.
    /// |parent| is only set on the node if the node was not present in the cache.  Otherwise, it is
    /// ignored.
    pub async fn get_or_load_node(
        self: &Arc<Self>,
        object_id: u64,
        object_descriptor: ObjectDescriptor,
        parent: Option<Arc<FxDirectory>>,
    ) -> Result<Arc<dyn FxNode>, Error> {
        match self.cache.get_or_reserve(object_id).await {
            GetResult::Node(node) => Ok(node),
            GetResult::Placeholder(placeholder) => {
                let node = match object_descriptor {
                    ObjectDescriptor::File => FxFile::new(
                        ObjectStore::open_object(
                            self,
                            object_id,
                            HandleOptions::default(),
                            self.store().crypt().as_deref(),
                        )
                        .await?,
                    ) as Arc<dyn FxNode>,
                    ObjectDescriptor::Directory => {
                        Arc::new(FxDirectory::new(parent, Directory::open(self, object_id).await?))
                            as Arc<dyn FxNode>
                    }
                    _ => bail!(FxfsError::Inconsistent),
                };
                placeholder.commit(&node);
                Ok(node)
            }
        }
    }

    pub fn into_store(self) -> Arc<ObjectStore> {
        self.store
    }

    /// Marks the given directory deleted.
    pub fn mark_directory_deleted(&self, object_id: u64, name: &str) {
        if let Some(node) = self.cache.get(object_id) {
            // It's possible that node is a placeholder, in which case we don't need to wait for it
            // to be resolved because it should be blocked behind the locks that are held by the
            // caller, and once they're dropped, it'll be found to be deleted via the tree.
            if let Ok(dir) = node.into_any().downcast::<FxDirectory>() {
                dir.set_deleted(name);
            }
        }
    }

    /// Removes resources associated with |object_id| (which ought to be a file), if there are no
    /// open connections to that file.
    ///
    /// This must be called *after committing* a transaction which deletes the last reference to
    /// |object_id|, since before that point, new connections could be established.
    pub(super) async fn maybe_purge_file(&self, object_id: u64) -> Result<(), Error> {
        if let Some(node) = self.cache.get(object_id) {
            if let Ok(file) = node.into_any().downcast::<FxFile>() {
                if !file.mark_purged() {
                    return Ok(());
                }
            }
        }
        // If this fails, the graveyard should clean it up on next mount.
        self.store
            .tombstone(object_id, Options { borrow_metadata_space: true, ..Default::default() })
            .await?;
        Ok(())
    }

    /// Starts the background flush task.  This task will periodically scan all files and flush them
    /// to disk.
    /// The task will hold a strong reference to the FxVolume while it is running, so the task must
    /// be closed later with Self::terminate, or the FxVolume will never be dropped.
    pub fn start_flush_task(self: &Arc<Self>, period: Duration) {
        let mut flush_task = self.flush_task.lock().unwrap();
        if flush_task.is_none() {
            let (tx, rx) = oneshot::channel();
            *flush_task = Some((fasync::Task::spawn(self.clone().flush_task(period, rx)), tx));
        }
    }

    async fn flush_task(self: Arc<Self>, period: Duration, terminate: oneshot::Receiver<()>) {
        log::debug!("FxVolume::flush_task start {}", self.store.store_object_id());
        let mut terminate = terminate.fuse();
        loop {
            if futures::select!(
                _ = fasync::Timer::new(period).fuse() => false,
                _ = terminate => true,
            ) {
                break;
            }
            let files = self.cache.files();
            let mut flushed = 0;
            for file in files {
                if let Err(e) = file.flush().await {
                    log::warn!(
                        "Failed to flush {}.{}: {:?}",
                        self.store.store_object_id(),
                        file.object_id(),
                        e
                    )
                }
                flushed += 1;
            }
            log::debug!("FxVolume {} flushed {} files", self.store.store_object_id(), flushed);
        }
        log::debug!("FxVolume::flush_task end {}", self.store.store_object_id());
    }
}

impl HandleOwner for FxVolume {
    type Buffer = VmoDataBuffer;

    fn create_data_buffer(&self, object_id: u64, initial_size: u64) -> Self::Buffer {
        self.pager.create_vmo(object_id, initial_size).unwrap().try_into().unwrap()
    }
}

impl AsRef<ObjectStore> for FxVolume {
    fn as_ref(&self) -> &ObjectStore {
        &self.store
    }
}

#[async_trait]
impl FilesystemRename for FxVolume {
    async fn rename(
        &self,
        src_dir: Arc<dyn Any + Sync + Send + 'static>,
        src_name: Path,
        dst_dir: Arc<dyn Any + Sync + Send + 'static>,
        dst_name: Path,
    ) -> Result<(), Status> {
        if !src_name.is_single_component() || !dst_name.is_single_component() {
            return Err(Status::INVALID_ARGS);
        }
        let (src, dst) = (src_name.peek().unwrap(), dst_name.peek().unwrap());
        let src_dir = src_dir.downcast::<FxDirectory>().map_err(|_| Err(Status::NOT_DIR))?;
        let dst_dir = dst_dir.downcast::<FxDirectory>().map_err(|_| Err(Status::NOT_DIR))?;

        // Acquire a transaction that locks |src_dir|, |dst_dir|, and |dst_name| if it exists.
        let fs = self.store.filesystem();
        let (mut transaction, dst_id_and_descriptor) = match dst_dir
            .acquire_transaction_for_unlink(
                &[LockKey::object(self.store.store_object_id(), src_dir.object_id())],
                dst,
                false,
            )
            .await
        {
            Ok((transaction, id, descriptor)) => (transaction, Some((id, descriptor))),
            Err(e) if FxfsError::NotFound.matches(&e) => {
                let transaction = fs
                    .new_transaction(
                        &[
                            LockKey::object(self.store.store_object_id(), src_dir.object_id()),
                            LockKey::object(self.store.store_object_id(), dst_dir.object_id()),
                        ],
                        // It's ok to borrow metadata space here since after compaction, it should
                        // be a wash.
                        Options { borrow_metadata_space: true, ..Default::default() },
                    )
                    .await
                    .map_err(map_to_status)?;
                (transaction, None)
            }
            Err(e) => return Err(map_to_status(e)),
        };

        if dst_dir.is_deleted() {
            return Err(Status::NOT_FOUND);
        }

        let (moved_id, moved_descriptor) = src_dir
            .directory()
            .lookup(src)
            .await
            .map_err(map_to_status)?
            .ok_or(Status::NOT_FOUND)?;
        // Make sure the dst path is compatible with the moved node.
        if let ObjectDescriptor::File = moved_descriptor {
            if src_name.is_dir() || dst_name.is_dir() {
                return Err(Status::NOT_DIR);
            }
        }

        // Now that we've ensured that the dst path is compatible with the moved node, we can check
        // for the trivial case.
        if src_dir.object_id() == dst_dir.object_id() && src == dst {
            return Ok(());
        }

        if let Some((_, dst_descriptor)) = dst_id_and_descriptor.as_ref() {
            // dst is being overwritten; make sure it's a file iff src is.
            if dst_descriptor != &moved_descriptor {
                match dst_descriptor {
                    ObjectDescriptor::File => return Err(Status::NOT_DIR),
                    ObjectDescriptor::Directory => return Err(Status::NOT_FILE),
                    _ => return Err(Status::IO_DATA_INTEGRITY),
                };
            }
        }

        let moved_node = src_dir
            .volume()
            .get_or_load_node(moved_id, moved_descriptor.clone(), Some(src_dir.clone()))
            .await
            .map_err(map_to_status)?;

        if let ObjectDescriptor::Directory = moved_descriptor {
            // Lastly, ensure that dst_dir isn't a (transitive) child of the moved node.
            let mut node_opt = Some(dst_dir.clone());
            while let Some(node) = node_opt {
                if node.object_id() == moved_node.object_id() {
                    return Err(Status::INVALID_ARGS);
                }
                node_opt = node.parent();
            }
        }

        let replace_result = directory::replace_child(
            &mut transaction,
            Some((src_dir.directory(), src)),
            (dst_dir.directory(), dst),
        )
        .await
        .map_err(map_to_status)?;

        transaction
            .commit_with_callback(|_| {
                moved_node.set_parent(dst_dir.clone());
                src_dir.did_remove(src);

                match replace_result {
                    ReplacedChild::None => dst_dir.did_add(dst),
                    ReplacedChild::FileWithRemainingLinks(..) | ReplacedChild::File(_) => {
                        dst_dir.did_remove(dst);
                        dst_dir.did_add(dst);
                    }
                    ReplacedChild::Directory(id) => {
                        dst_dir.did_remove(dst);
                        dst_dir.did_add(dst);
                        self.mark_directory_deleted(id, dst);
                    }
                }
            })
            .await
            .map_err(map_to_status)?;

        if let ReplacedChild::File(id) = replace_result {
            self.maybe_purge_file(id).await.map_err(map_to_status)?;
        }

        Ok(())
    }
}

impl Filesystem for FxVolume {
    fn block_size(&self) -> u32 {
        self.store.block_size().try_into().unwrap()
    }
}

pub struct FxVolumeAndRoot {
    volume: Arc<FxVolume>,
    root: Arc<dyn FxNode>,
}

impl FxVolumeAndRoot {
    pub async fn new(store: Arc<ObjectStore>, unique_id: u64) -> Result<Self, Error> {
        let volume = Arc::new(FxVolume::new(store, unique_id)?);
        let root_object_id = volume.store().root_directory_object_id();
        let root_dir = Directory::open(&volume, root_object_id).await?;
        let root: Arc<dyn FxNode> = Arc::new(FxDirectory::new(None, root_dir));
        match volume.cache.get_or_reserve(root_object_id).await {
            GetResult::Node(_) => unreachable!(),
            GetResult::Placeholder(placeholder) => placeholder.commit(&root),
        }
        Ok(Self { volume, root })
    }

    pub fn volume(&self) -> &Arc<FxVolume> {
        &self.volume
    }

    pub fn root(&self) -> Arc<FxDirectory> {
        self.root.clone().into_any().downcast::<FxDirectory>().expect("Invalid type for root")
    }

    #[cfg(test)]
    pub(super) fn into_volume(self) -> Arc<FxVolume> {
        self.volume
    }
}

#[cfg(test)]
mod tests {
    use {
        crate::{
            crypt::InsecureCrypt,
            object_handle::{ObjectHandle, ObjectHandleExt},
            object_store::{
                directory::ObjectDescriptor,
                filesystem::FxFilesystem,
                transaction::{Options, TransactionHandler},
                volume::root_volume,
                HandleOptions, ObjectStore,
            },
            server::{
                file::FxFile,
                testing::{
                    close_dir_checked, close_file_checked, open_dir, open_dir_checked, open_file,
                    open_file_checked, TestFixture,
                },
                volume::FxVolumeAndRoot,
            },
        },
        fidl_fuchsia_io::{
            MODE_TYPE_DIRECTORY, MODE_TYPE_FILE, OPEN_FLAG_CREATE, OPEN_FLAG_DIRECTORY,
            OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE,
        },
        fuchsia_async as fasync,
        fuchsia_zircon::Status,
        io_util::{read_file_bytes, write_file_bytes},
        std::sync::Arc,
        storage_device::{fake_device::FakeDevice, DeviceHolder},
        vfs::file::File,
    };

    #[fasync::run(10, test)]
    async fn test_rename_different_dirs() {
        use fuchsia_zircon::Event;

        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let src = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            "foo",
        )
        .await;

        let dst = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DIRECTORY,
            MODE_TYPE_DIRECTORY,
            "bar",
        )
        .await;

        let f = open_file_checked(&root, OPEN_FLAG_CREATE, MODE_TYPE_FILE, "foo/a").await;
        close_file_checked(f).await;

        let (status, dst_token) = dst.get_token().await.expect("FIDL call failed");
        Status::ok(status).expect("get_token failed");
        src.rename("a", Event::from(dst_token.unwrap()), "b")
            .await
            .expect("FIDL call failed")
            .expect("rename failed");

        assert_eq!(
            open_file(&root, 0, MODE_TYPE_FILE, "foo/a")
                .await
                .expect_err("Open succeeded")
                .root_cause()
                .downcast_ref::<Status>()
                .expect("No status"),
            &Status::NOT_FOUND,
        );
        let f = open_file_checked(&root, 0, MODE_TYPE_FILE, "bar/b").await;
        close_file_checked(f).await;

        close_dir_checked(dst).await;
        close_dir_checked(src).await;
        fixture.close().await;
    }

    #[fasync::run(10, test)]
    async fn test_rename_same_dir() {
        use fuchsia_zircon::Event;
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let src = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            "foo",
        )
        .await;

        let f = open_file_checked(&root, OPEN_FLAG_CREATE, MODE_TYPE_FILE, "foo/a").await;
        close_file_checked(f).await;

        let (status, src_token) = src.get_token().await.expect("FIDL call failed");
        Status::ok(status).expect("get_token failed");
        src.rename("a", Event::from(src_token.unwrap()), "b")
            .await
            .expect("FIDL call failed")
            .expect("rename failed");

        assert_eq!(
            open_file(&root, 0, MODE_TYPE_FILE, "foo/a")
                .await
                .expect_err("Open succeeded")
                .root_cause()
                .downcast_ref::<Status>()
                .expect("No status"),
            &Status::NOT_FOUND,
        );
        let f = open_file_checked(&root, 0, MODE_TYPE_FILE, "foo/b").await;
        close_file_checked(f).await;

        close_dir_checked(src).await;
        fixture.close().await;
    }

    #[fasync::run(10, test)]
    async fn test_rename_overwrites_file() {
        use fuchsia_zircon::Event;
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let src = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            "foo",
        )
        .await;

        let dst = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DIRECTORY,
            MODE_TYPE_DIRECTORY,
            "bar",
        )
        .await;

        // The src file is non-empty.
        let src_file = open_file_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_FILE,
            "foo/a",
        )
        .await;
        let buf = vec![0xaa as u8; 8192];
        write_file_bytes(&src_file, buf.as_slice()).await.expect("Failed to write to file");
        close_file_checked(src_file).await;

        // The dst file is empty (so we can distinguish it).
        let f = open_file_checked(&root, OPEN_FLAG_CREATE, MODE_TYPE_FILE, "bar/b").await;
        close_file_checked(f).await;

        let (status, dst_token) = dst.get_token().await.expect("FIDL call failed");
        Status::ok(status).expect("get_token failed");
        src.rename("a", Event::from(dst_token.unwrap()), "b")
            .await
            .expect("FIDL call failed")
            .expect("rename failed");

        assert_eq!(
            open_file(&root, 0, MODE_TYPE_FILE, "foo/a")
                .await
                .expect_err("Open succeeded")
                .root_cause()
                .downcast_ref::<Status>()
                .expect("No status"),
            &Status::NOT_FOUND,
        );
        let file = open_file_checked(&root, OPEN_RIGHT_READABLE, MODE_TYPE_FILE, "bar/b").await;
        let buf = read_file_bytes(&file).await.expect("read file failed");
        assert_eq!(buf, vec![0xaa as u8; 8192]);
        close_file_checked(file).await;

        close_dir_checked(dst).await;
        close_dir_checked(src).await;
        fixture.close().await;
    }

    #[fasync::run(10, test)]
    async fn test_rename_overwrites_dir() {
        use fuchsia_zircon::Event;
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let src = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            "foo",
        )
        .await;

        let dst = open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DIRECTORY,
            MODE_TYPE_DIRECTORY,
            "bar",
        )
        .await;

        // The src dir is non-empty.
        open_dir_checked(
            &root,
            OPEN_FLAG_CREATE | OPEN_RIGHT_WRITABLE,
            MODE_TYPE_DIRECTORY,
            "foo/a",
        )
        .await;
        open_file_checked(&root, OPEN_FLAG_CREATE, MODE_TYPE_FILE, "foo/a/file").await;
        open_dir_checked(&root, OPEN_FLAG_CREATE, MODE_TYPE_DIRECTORY, "bar/b").await;

        let (status, dst_token) = dst.get_token().await.expect("FIDL call failed");
        Status::ok(status).expect("get_token failed");
        src.rename("a", Event::from(dst_token.unwrap()), "b")
            .await
            .expect("FIDL call failed")
            .expect("rename failed");

        assert_eq!(
            open_dir(&root, 0, MODE_TYPE_DIRECTORY, "foo/a")
                .await
                .expect_err("Open succeeded")
                .root_cause()
                .downcast_ref::<Status>()
                .expect("No status"),
            &Status::NOT_FOUND,
        );
        let f = open_file_checked(&root, 0, MODE_TYPE_FILE, "bar/b/file").await;
        close_file_checked(f).await;

        close_dir_checked(dst).await;
        close_dir_checked(src).await;

        fixture.close().await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_background_flush() {
        // We have to do a bit of set-up ourselves for this test, since we want to be able to access
        // the underlying StoreObjectHandle at the same time as the FxFile which corresponds to it.
        let device = DeviceHolder::new(FakeDevice::new(8192, 512));
        let filesystem = FxFilesystem::new_empty(device).await.unwrap();
        {
            let root_volume = root_volume(&filesystem).await.unwrap();
            let volume =
                root_volume.new_volume("vol", Arc::new(InsecureCrypt::new())).await.unwrap();
            let mut transaction = filesystem
                .clone()
                .new_transaction(&[], Options::default())
                .await
                .expect("new_transaction failed");
            let object_id = ObjectStore::create_object(
                &volume,
                &mut transaction,
                HandleOptions::default(),
                None,
            )
            .await
            .expect("create_object failed")
            .object_id();
            transaction.commit().await.expect("commit failed");
            let vol = FxVolumeAndRoot::new(volume.clone(), 0).await.unwrap();

            let file = vol
                .volume()
                .get_or_load_node(object_id, ObjectDescriptor::File, None)
                .await
                .expect("get_or_load_node failed")
                .into_any()
                .downcast::<FxFile>()
                .expect("Not a file");

            // Write some data to the file, which will only go to the cache for now.
            file.write_at(0, &[123u8]).await.expect("write_at failed");

            let data_has_persisted = || async {
                // We have to reopen the object each time since this is a distinct handle from the
                // one managed by the FxFile.
                let object =
                    ObjectStore::open_object(&volume, object_id, HandleOptions::default(), None)
                        .await
                        .expect("open_object failed");
                let data = object.contents(8192).await.expect("read failed");
                data.len() == 1 && data[..] == [123u8]
            };
            assert!(!data_has_persisted().await);

            vol.volume().start_flush_task(std::time::Duration::from_millis(100));

            let mut wait = 100;
            loop {
                if data_has_persisted().await {
                    break;
                }
                fasync::Timer::new(std::time::Duration::from_millis(wait)).await;
                wait *= 2;
            }

            vol.volume().terminate().await;
        }

        filesystem.close().await.expect("close filesystem failed");
        let device = filesystem.take_device().await;
        device.ensure_unique();
    }
}
