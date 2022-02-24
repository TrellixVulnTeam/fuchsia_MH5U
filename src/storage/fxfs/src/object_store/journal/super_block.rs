// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        errors::FxfsError,
        lsm_tree::types::{Item, LayerIterator},
        object_store::{
            allocator::Reservation,
            constants::{SUPER_BLOCK_A_OBJECT_ID, SUPER_BLOCK_B_OBJECT_ID},
            extent_record::{ExtentKey, ExtentValue},
            journal::{
                handle::Handle,
                reader::{JournalReader, ReadResult},
                writer::JournalWriter,
                JournalCheckpoint,
            },
            object_record::ObjectItem,
            transaction::Options,
            ObjectStore, StoreObjectHandle,
        },
        range::RangeExt,
        serialized_types::{Versioned, VersionedLatest},
    },
    anyhow::{bail, ensure, Error},
    serde::{Deserialize, Serialize},
    std::{
        collections::HashMap,
        io::{Read, Write},
        ops::{Bound, Range},
        sync::Arc,
    },
    storage_device::Device,
};

/// The block size used when reading and writing journal entries.
const SUPER_BLOCK_BLOCK_SIZE: usize = 8192;

/// The superblock is extended in units of `SUPER_BLOCK_CHUNK_SIZE` as required.
const SUPER_BLOCK_CHUNK_SIZE: u64 = 65536;

/// The first 2 * 512 KiB on the disk are reserved for two A/B super-blocks.
const MIN_SUPER_BLOCK_SIZE: u64 = 524_288;

/// All superblocks start with the magic bytes "FxfsSupr".
const SUPER_BLOCK_MAGIC: &[u8; 8] = b"FxfsSupr";

pub const SUPER_BLOCK_MAJOR_VERSION: u32 = 1;
pub const SUPER_BLOCK_MINOR_VERSION: u32 = 1;

/// An enum representing one of our [SuperBlock] copies.
///
/// This provides hard-coded constants related to the location and properties of the super blocks
/// that are required to bootstrap the filesystem.
#[derive(Copy, Clone, Debug)]
pub enum SuperBlockCopy {
    A,
    B,
}

impl SuperBlockCopy {
    /// Returns the next [SuperBlockCopy] for use in round-robining writes across super-blocks.
    pub fn next(&self) -> SuperBlockCopy {
        match self {
            SuperBlockCopy::A => SuperBlockCopy::B,
            SuperBlockCopy::B => SuperBlockCopy::A,
        }
    }

    pub fn object_id(&self) -> u64 {
        match self {
            SuperBlockCopy::A => SUPER_BLOCK_A_OBJECT_ID,
            SuperBlockCopy::B => SUPER_BLOCK_B_OBJECT_ID,
        }
    }

    /// Returns the byte range where the first extent of the [SuperBlockCopy] is stored.
    /// (Note that a [SuperBlockCopy] may still have multiple extents.)
    pub fn first_extent(&self) -> Range<u64> {
        match self {
            SuperBlockCopy::A => 0..MIN_SUPER_BLOCK_SIZE,
            SuperBlockCopy::B => MIN_SUPER_BLOCK_SIZE..2 * MIN_SUPER_BLOCK_SIZE,
        }
    }
}

/// A super-block structure describing the filesystem.
///
/// We currently store two of these super blocks (A/B) located in two logical consecutive
/// 512kiB extents at the start of the device.
///
/// Immediately following the serialized `SuperBlock` structure below is a stream of serialized
/// operations that are replayed into the root parent `ObjectStore`. Note that the root parent
/// object store exists entirely in RAM until serialized back into the super-block.
///
/// Super blocks are updated alternately with a monotonically increasing generation number.
/// At mount time, the super block used is the valid `SuperBlock` with the highest generation
/// number.
// TODO(csuter): Add a UUID
#[derive(Clone, Debug, Default, Eq, PartialEq, Serialize, Deserialize, Versioned)]
pub struct SuperBlock {
    /// The major version of the super-block's format.
    pub major_version: u32,

    /// The minor version of the oldest driver which touched the super-block in writeable mode.
    /// See //src/storage/docs/versioning.md.
    // TODO(jfsulliv): Actually set this based on the driver version.
    // TODO(jfsulliv): Check this value.
    pub oldest_minor_version: u32,

    /// There are two super-blocks which are used in an A/B configuration. The super-block with the
    /// greatest generation number is what is used when mounting an Fxfs image; the other is
    /// discarded.
    pub generation: u64,

    /// The root parent store is an in-memory only store and serves as the backing store for the
    /// root store and the journal.  The records for this store are serialized into the super-block
    /// and mutations are also recorded in the journal.
    pub root_parent_store_object_id: u64,

    /// The root parent needs a graveyard and there's nowhere else to store it other than in the
    /// super-block.
    pub root_parent_graveyard_directory_object_id: u64,

    /// The root object store contains all other metadata objects (including the allocator, the
    /// journal and the super-blocks) and is the parent for all other object stores.
    pub root_store_object_id: u64,

    /// This is in the root object store.
    pub allocator_object_id: u64,

    /// This is in the root parent object store.
    pub journal_object_id: u64,

    /// Start checkpoint for the journal file.
    pub journal_checkpoint: JournalCheckpoint,

    /// Offset of the journal file when the super-block was written.  If no entry is present in
    /// journal_file_offsets for a particular object, then an object might have dependencies on the
    /// journal from super_block_journal_file_offset onwards, but not earlier.
    pub super_block_journal_file_offset: u64,

    /// object id -> journal file offset. Indicates where each object has been flushed to.
    pub journal_file_offsets: HashMap<u64, u64>,

    /// Records the amount of borrowed metadata space as applicable at
    /// `super_block_journal_file_offset`.
    pub borrowed_metadata_space: u64,
}

#[derive(Serialize, Deserialize, Versioned)]
pub enum SuperBlockRecord {
    // When reading the super-block we know the initial extent, but not subsequent extents, so these
    // records need to exist to allow us to completely read the super-block.
    Extent(Range<u64>),

    // Following the super-block header are ObjectItem records that are to be replayed into the root
    // parent object store.
    ObjectItem(ObjectItem),

    // After the ObjectItem records, come the extent records.
    ExtentItem(Item<ExtentKey, ExtentValue>),

    // Marks the end of the full super-block.
    End,
}

impl SuperBlock {
    pub(super) fn new(
        root_parent_store_object_id: u64,
        root_parent_graveyard_directory_object_id: u64,
        root_store_object_id: u64,
        allocator_object_id: u64,
        journal_object_id: u64,
        journal_checkpoint: JournalCheckpoint,
    ) -> Self {
        SuperBlock {
            major_version: SUPER_BLOCK_MAJOR_VERSION,
            oldest_minor_version: SUPER_BLOCK_MINOR_VERSION,
            generation: 1u64,
            root_parent_store_object_id,
            root_parent_graveyard_directory_object_id,
            root_store_object_id,
            allocator_object_id,
            journal_object_id,
            journal_checkpoint,
            ..Default::default()
        }
    }

    /// Shreds the super-block, rendering it unreadable.  This is used in mkfs to ensure that we
    /// wipe out any stale super-blocks when rewriting Fxfs.
    /// This isn't a secure shred in any way, it just ensures the super-block is not recognized as a
    /// super-block.
    pub(super) async fn shred<S: AsRef<ObjectStore> + Send + Sync + 'static>(
        handle: StoreObjectHandle<S>,
    ) -> Result<(), Error> {
        let mut buf =
            handle.store().device().allocate_buffer(handle.store().device().block_size() as usize);
        buf.as_mut_slice().fill(0u8);
        handle.overwrite(0, buf.as_ref()).await
    }

    /// Read the super-block header, and return it and a reader that produces the records that are
    /// to be replayed in to the root parent object store.
    pub async fn read(
        device: Arc<dyn Device>,
        target_super_block: SuperBlockCopy,
    ) -> Result<(SuperBlock, ItemReader), Error> {
        let mut handle = Handle::new(target_super_block.object_id(), device);
        handle.push_extent(target_super_block.first_extent());
        let mut reader = JournalReader::new(
            handle,
            SUPER_BLOCK_BLOCK_SIZE as u64,
            &JournalCheckpoint::default(),
        );

        reader.fill_buf().await?;
        let super_block;
        let version;
        reader.consume({
            let mut cursor = std::io::Cursor::new(reader.buffer());
            // Validate magic bytes.
            let mut magic_bytes: [u8; 8] = [0; 8];
            cursor.read_exact(&mut magic_bytes)?;
            if magic_bytes.as_slice() != SUPER_BLOCK_MAGIC.as_slice() {
                bail!(format!("Invalid magic: {:?}", magic_bytes));
            }
            (super_block, version) = SuperBlock::deserialize_with_version(&mut cursor)?;
            cursor.position() as usize
        });
        reader.set_version(version);
        Ok((super_block, ItemReader { reader }))
    }

    /// Writes the super-block and the records from the root parent store.
    pub(super) async fn write<'a, S: AsRef<ObjectStore> + Send + Sync + 'static>(
        &self,
        root_parent_store: &'a ObjectStore,
        handle: StoreObjectHandle<S>,
    ) -> Result<(), Error> {
        assert_eq!(root_parent_store.store_object_id(), self.root_parent_store_object_id);

        let object_manager = root_parent_store.filesystem().object_manager().clone();
        // TODO(ripper): Don't use the same code here for Journal and SuperBlock. They aren't the
        // same things and it is already getting convoluted. e.g of diff sstream content:
        //   Superblock:  (Magic, Ver, Header(Ver), SuperBlockRecord(Ver)*, ...)
        //   Journal:     (Ver, JournalRecord(Ver)*, RESET, Ver2, JournalRecord(Ver2)*, ...)
        // We should abstract away the checksum code and implement these separately.
        let mut writer = SuperBlockWriter::new(handle, object_manager.metadata_reservation());

        writer.writer.write(SUPER_BLOCK_MAGIC)?;
        self.serialize_with_version(&mut writer.writer)?;

        let tree = root_parent_store.tree();
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await?;

        while let Some(item_ref) = iter.get() {
            writer.maybe_extend().await?;
            SuperBlockRecord::ObjectItem(item_ref.cloned()).serialize_into(&mut writer.writer)?;
            iter.advance().await?;
        }

        let tree = root_parent_store.extent_tree();
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await?;

        while let Some(item_ref) = iter.get() {
            writer.maybe_extend().await?;
            SuperBlockRecord::ExtentItem(item_ref.cloned()).serialize_into(&mut writer.writer)?;
            iter.advance().await?;
        }

        SuperBlockRecord::End.serialize_into(&mut writer.writer)?;
        writer.writer.pad_to_block()?;
        writer.flush_buffer().await
    }
}

struct SuperBlockWriter<'a, S: AsRef<ObjectStore> + Send + Sync + 'static> {
    handle: StoreObjectHandle<S>,
    writer: JournalWriter,
    next_extent_offset: u64,
    reservation: &'a Reservation,
}

impl<'a, S: AsRef<ObjectStore> + Send + Sync + 'static> SuperBlockWriter<'a, S> {
    fn new(handle: StoreObjectHandle<S>, reservation: &'a Reservation) -> Self {
        Self {
            handle,
            writer: JournalWriter::new(SUPER_BLOCK_BLOCK_SIZE, 0),
            next_extent_offset: MIN_SUPER_BLOCK_SIZE,
            reservation,
        }
    }

    async fn maybe_extend(&mut self) -> Result<(), Error> {
        if self.writer.journal_file_checkpoint().file_offset
            < self.next_extent_offset - SUPER_BLOCK_CHUNK_SIZE
        {
            return Ok(());
        }
        let mut transaction = self
            .handle
            .new_transaction_with_options(Options {
                skip_journal_checks: true,
                borrow_metadata_space: true,
                allocator_reservation: Some(self.reservation),
                ..Default::default()
            })
            .await?;
        let allocated = self
            .handle
            .preallocate_range(
                &mut transaction,
                self.next_extent_offset..self.next_extent_offset + SUPER_BLOCK_CHUNK_SIZE,
            )
            .await?;
        transaction.commit().await?;
        for device_range in allocated {
            self.next_extent_offset += device_range.end - device_range.start;
            SuperBlockRecord::Extent(device_range).serialize_into(&mut self.writer)?;
        }
        Ok(())
    }

    async fn flush_buffer(&mut self) -> Result<(), Error> {
        let (offset, buf) = self.writer.take_buffer(&self.handle).unwrap();
        self.handle.overwrite(offset, buf.as_ref()).await
    }
}

#[derive(Debug)]
pub enum SuperBlockItem {
    End,
    Object(ObjectItem),
    Extent(Item<ExtentKey, ExtentValue>),
}

pub struct ItemReader {
    reader: JournalReader<Handle>,
}

impl ItemReader {
    pub async fn next_item(&mut self) -> Result<SuperBlockItem, Error> {
        loop {
            match self.reader.deserialize().await? {
                ReadResult::Reset => bail!("Unexpected reset"),
                ReadResult::ChecksumMismatch => bail!("Checksum mismatch"),
                ReadResult::Some(SuperBlockRecord::Extent(extent)) => {
                    ensure!(extent.valid(), FxfsError::Inconsistent);
                    self.reader.handle().push_extent(extent)
                }
                ReadResult::Some(SuperBlockRecord::ObjectItem(item)) => {
                    return Ok(SuperBlockItem::Object(item))
                }
                ReadResult::Some(SuperBlockRecord::ExtentItem(item)) => {
                    return Ok(SuperBlockItem::Extent(item))
                }
                ReadResult::Some(SuperBlockRecord::End) => return Ok(SuperBlockItem::End),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{SuperBlock, SuperBlockCopy, SuperBlockItem, MIN_SUPER_BLOCK_SIZE},
        crate::{
            lsm_tree::types::LayerIterator,
            object_store::{
                constants::{SUPER_BLOCK_A_OBJECT_ID, SUPER_BLOCK_B_OBJECT_ID},
                filesystem::Filesystem,
                journal::{journal_handle_options, JournalCheckpoint},
                testing::{fake_allocator::FakeAllocator, fake_filesystem::FakeFilesystem},
                transaction::{Options, TransactionHandler},
                HandleOptions, ObjectHandle, ObjectStore, StoreObjectHandle,
            },
            serialized_types::LATEST_VERSION,
        },
        fuchsia_async as fasync,
        std::{ops::Bound, sync::Arc},
        storage_device::{fake_device::FakeDevice, DeviceHolder},
    };

    const TEST_DEVICE_BLOCK_SIZE: u32 = 512;

    async fn filesystem_and_super_block_handles(
    ) -> (Arc<FakeFilesystem>, StoreObjectHandle<ObjectStore>, StoreObjectHandle<ObjectStore>) {
        let device = DeviceHolder::new(FakeDevice::new(8192, TEST_DEVICE_BLOCK_SIZE));
        let fs = FakeFilesystem::new(device);
        let allocator = Arc::new(FakeAllocator::new());
        fs.object_manager().set_allocator(allocator.clone());
        fs.object_manager().init_metadata_reservation();
        let root_parent_store = ObjectStore::new_empty(None, 3, fs.clone());
        fs.object_manager().set_root_parent_store(root_parent_store.clone());
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let root_store = root_parent_store
            .create_child_store_with_id(&mut transaction, 4)
            .await
            .expect("create_child_store failed");
        fs.object_manager().set_root_store(root_store.clone());

        let handle_a; // extend will borrow handle and needs to outlive transaction.
        let handle_b; // extend will borrow handle and needs to outlive transaction.
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        handle_a = ObjectStore::create_object_with_id(
            &root_store,
            &mut transaction,
            SUPER_BLOCK_A_OBJECT_ID,
            journal_handle_options(),
            None,
        )
        .await
        .expect("create_object_with_id failed");
        handle_a
            .extend(&mut transaction, super::SuperBlockCopy::A.first_extent())
            .await
            .expect("extend failed");
        handle_b = ObjectStore::create_object_with_id(
            &root_store,
            &mut transaction,
            SUPER_BLOCK_B_OBJECT_ID,
            journal_handle_options(),
            None,
        )
        .await
        .expect("create_object_with_id failed");
        handle_b
            .extend(&mut transaction, super::SuperBlockCopy::B.first_extent())
            .await
            .expect("extend failed");

        transaction.commit().await.expect("commit failed");

        (fs, handle_a, handle_b)
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_written_super_block() {
        let (fs, handle_a, handle_b) = filesystem_and_super_block_handles().await;
        const JOURNAL_OBJECT_ID: u64 = 5;

        // Create a large number of objects in the root parent store so that we test handling of
        // extents.
        let mut journal_offset = 0;
        for _ in 0..32000 {
            let mut transaction = fs
                .clone()
                .new_transaction(&[], Options::default())
                .await
                .expect("new_transaction failed");
            ObjectStore::create_object(
                &fs.object_manager().root_parent_store(),
                &mut transaction,
                HandleOptions::default(),
                None,
            )
            .await
            .expect("create_object failed");
            journal_offset = transaction.commit().await.expect("commit failed");
        }

        let mut super_block_a = SuperBlock::new(
            fs.object_manager().root_parent_store().store_object_id(),
            /* root_parent_graveyard_directory_object_id: */ 1000,
            fs.root_store().store_object_id(),
            fs.allocator().object_id(),
            JOURNAL_OBJECT_ID,
            JournalCheckpoint { file_offset: 1234, checksum: 5678, version: LATEST_VERSION },
        );
        super_block_a.super_block_journal_file_offset = journal_offset + 1;
        let mut super_block_b = super_block_a.clone();
        super_block_b.journal_file_offsets.insert(1, 2);
        super_block_b.generation += 1;

        let layer_set = fs.object_manager().root_parent_store().tree().layer_set();
        let mut merger = layer_set.merger();

        super_block_a
            .write(fs.object_manager().root_parent_store().as_ref(), handle_a)
            .await
            .expect("write failed");
        super_block_b
            .write(fs.object_manager().root_parent_store().as_ref(), handle_b)
            .await
            .expect("write failed");

        // Make sure we did actually extend the super block.
        let handle = ObjectStore::open_object(
            &fs.root_store(),
            SUPER_BLOCK_A_OBJECT_ID,
            HandleOptions::default(),
            None,
        )
        .await
        .expect("open_object failed");
        assert!(handle.get_size() > MIN_SUPER_BLOCK_SIZE);

        let mut written_super_block_a =
            SuperBlock::read(fs.device(), SuperBlockCopy::A).await.expect("read failed");
        assert_eq!(written_super_block_a.0, super_block_a);
        let written_super_block_b =
            SuperBlock::read(fs.device(), SuperBlockCopy::B).await.expect("read failed");
        assert_eq!(written_super_block_b.0, super_block_b);

        // Check that the records match what we expect in the root parent store.
        let mut iter = merger.seek(Bound::Unbounded).await.expect("seek failed");

        let mut written_item = written_super_block_a.1.next_item().await.expect("next_item failed");

        while let Some(item) = iter.get() {
            if let SuperBlockItem::Object(i) = written_item {
                assert_eq!(i.as_item_ref(), item);
            } else {
                panic!("missing item: {:?}", item);
            }
            iter.advance().await.expect("advance failed");
            written_item = written_super_block_a.1.next_item().await.expect("next_item failed");
        }

        let layer_set = fs.object_manager().root_parent_store().extent_tree().layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await.expect("seek failed");

        while let Some(item) = iter.get() {
            if let SuperBlockItem::Extent(i) = written_item {
                assert_eq!(i.as_item_ref(), item);
            } else {
                panic!("missing item: {:?}", item);
            }
            iter.advance().await.expect("advance failed");
            written_item = written_super_block_a.1.next_item().await.expect("next_item failed");
        }

        if let SuperBlockItem::End = written_item {
        } else {
            panic!("unexpected extra item: {:?}", written_item);
        }
    }
}
