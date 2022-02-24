// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        crypt::{UnwrappedKeys, XtsCipherSet},
        errors::FxfsError,
        lsm_tree::types::{ItemRef, LayerIterator},
        object_handle::{
            GetProperties, ObjectHandle, ObjectProperties, ReadObjectHandle, WriteBytes,
            WriteObjectHandle,
        },
        object_store::{
            extent_record::{Checksums, ExtentKey, ExtentValue},
            journal::fletcher64,
            object_manager::ObjectManager,
            object_record::{
                ObjectAttributes, ObjectItem, ObjectKey, ObjectKind, ObjectValue, Timestamp,
            },
            transaction::{
                self, AssocObj, AssociatedObject, LockKey, Mutation, ObjectStoreMutation, Options,
                Transaction,
            },
            HandleOptions, ObjectStore,
        },
        round::{round_down, round_up},
    },
    anyhow::{anyhow, bail, Context, Error},
    async_trait::async_trait,
    futures::{
        stream::{FuturesOrdered, FuturesUnordered},
        try_join, TryStreamExt,
    },
    std::{
        cmp::min,
        ops::{Bound, Range},
        sync::{
            atomic::{self, AtomicBool, AtomicU64},
            Arc,
        },
    },
    storage_device::buffer::{Buffer, BufferRef, MutableBufferRef},
};

// TODO(csuter): We should probably be a little more frugal about what we store here since there
// could be a lot of these structures.
pub struct StoreObjectHandle<S: AsRef<ObjectStore> + Send + Sync + 'static> {
    owner: Arc<S>,
    pub(super) object_id: u64,
    pub(super) attribute_id: u64,
    pub(super) options: HandleOptions,
    pub(super) trace: AtomicBool,
    keys: Option<XtsCipherSet>,
    content_size: AtomicU64,
}

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> StoreObjectHandle<S> {
    pub fn new(
        owner: Arc<S>,
        object_id: u64,
        keys: Option<UnwrappedKeys>,
        attribute_id: u64,
        size: u64,
        options: HandleOptions,
        trace: bool,
    ) -> Self {
        Self {
            owner,
            object_id,
            keys: keys.as_ref().map(XtsCipherSet::new),
            attribute_id,
            options,
            trace: AtomicBool::new(trace),
            content_size: AtomicU64::new(size),
        }
    }

    pub fn owner(&self) -> &Arc<S> {
        &self.owner
    }

    pub fn store(&self) -> &ObjectStore {
        self.owner.as_ref().as_ref()
    }

    /// Extend the file with the given extent.  The only use case for this right now is for files
    /// that must exist at certain offsets on the device, such as super-blocks.
    pub async fn extend<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        device_range: Range<u64>,
    ) -> Result<(), Error> {
        let old_end =
            round_up(self.txn_get_size(transaction), self.block_size()).ok_or(FxfsError::TooBig)?;
        let new_size = old_end + device_range.end - device_range.start;
        self.store().allocator().mark_allocated(transaction, device_range.clone()).await?;
        transaction.add_with_object(
            self.store().store_object_id,
            Mutation::replace_or_insert_object(
                ObjectKey::attribute(self.object_id, self.attribute_id),
                ObjectValue::attribute(new_size),
            ),
            AssocObj::Borrowed(self),
        );
        transaction.add(
            self.store().store_object_id,
            Mutation::extent(
                ExtentKey::new(self.object_id, self.attribute_id, old_end..new_size),
                ExtentValue::new(device_range.start),
            ),
        );
        self.update_allocated_size(transaction, device_range.end - device_range.start, 0).await
    }

    // Returns a new aligned buffer (reading the head and tail blocks if necessary) with a copy of
    // the data from `buf`.
    async fn align_buffer(
        &self,
        offset: u64,
        buf: BufferRef<'_>,
    ) -> Result<(std::ops::Range<u64>, Buffer<'_>), Error> {
        let block_size = self.block_size();
        let end = offset + buf.len() as u64;
        let aligned =
            round_down(offset, block_size)..round_up(end, block_size).ok_or(FxfsError::TooBig)?;

        let mut aligned_buf =
            self.store().device.allocate_buffer((aligned.end - aligned.start) as usize);

        // Deal with head alignment.
        if aligned.start < offset {
            let mut head_block = aligned_buf.subslice_mut(..block_size as usize);
            let read = self.read(aligned.start, head_block.reborrow()).await?;
            head_block.as_mut_slice()[read..].fill(0);
        }

        // Deal with tail alignment.
        if aligned.end > end {
            let end_block_offset = aligned.end - block_size;
            // There's no need to read the tail block if we read it as part of the head block.
            if offset <= end_block_offset {
                let mut tail_block =
                    aligned_buf.subslice_mut(aligned_buf.len() - block_size as usize..);
                let read = self.read(end_block_offset, tail_block.reborrow()).await?;
                tail_block.as_mut_slice()[read..].fill(0);
            }
        }

        aligned_buf.as_mut_slice()
            [(offset - aligned.start) as usize..(end - aligned.start) as usize]
            .copy_from_slice(buf.as_slice());

        Ok((aligned, aligned_buf))
    }

    // Writes potentially unaligned data at `device_offset` and returns checksums if requested. The
    // data will be encrypted if necessary.
    async fn write_at(
        &self,
        offset: u64,
        buf: BufferRef<'_>,
        device_offset: u64,
        compute_checksum: bool,
    ) -> Result<Checksums, Error> {
        let (aligned, mut transfer_buf) = self.align_buffer(offset, buf).await?;

        if let Some(keys) = &self.keys {
            // TODO(https://fxbug.dev/92975): Support key_id != 0.
            keys.encrypt(aligned.start, 0, transfer_buf.as_mut_slice())?;
        }

        self.write_aligned(
            transfer_buf.as_ref(),
            device_offset - (offset - aligned.start),
            compute_checksum,
        )
        .await
    }

    // Writes aligned data (that should already be encrypted) to the given offset and computes
    // checksums if requested.
    async fn write_aligned(
        &self,
        buf: BufferRef<'_>,
        device_offset: u64,
        compute_checksum: bool,
    ) -> Result<Checksums, Error> {
        if self.trace.load(atomic::Ordering::Relaxed) {
            log::info!(
                "{}.{} W {:?} ({})",
                self.store().store_object_id(),
                self.object_id,
                device_offset..device_offset + buf.len() as u64,
                buf.len(),
            );
        }
        let mut checksums = Vec::new();
        try_join!(self.store().device.write(device_offset, buf), async {
            if compute_checksum {
                let block_size = self.block_size();
                for chunk in buf.as_slice().chunks_exact(block_size as usize) {
                    checksums.push(fletcher64(chunk, 0));
                }
            }
            Ok(())
        })?;
        Ok(if compute_checksum { Checksums::Fletcher(checksums) } else { Checksums::None })
    }

    // Returns the amount deallocated.
    async fn deallocate_old_extents(
        &self,
        transaction: &mut Transaction<'_>,
        range: Range<u64>,
    ) -> Result<u64, Error> {
        let block_size = self.block_size();
        assert_eq!(range.start % block_size, 0);
        assert_eq!(range.end % block_size, 0);
        if range.start == range.end {
            return Ok(0);
        }
        let tree = &self.store().extent_tree;
        let layer_set = tree.layer_set();
        let key = ExtentKey::new(self.object_id, self.attribute_id, range);
        let lower_bound = key.search_key();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Included(&lower_bound)).await?;
        let allocator = self.store().allocator();
        let mut deallocated = 0;
        let trace = self.trace.load(atomic::Ordering::Relaxed);
        while let Some(ItemRef { key: extent_key, value: extent_value, .. }) = iter.get() {
            if extent_key.object_id != self.object_id
                || extent_key.attribute_id != self.attribute_id
            {
                break;
            }
            if let ExtentValue::Some { device_offset, .. } = extent_value {
                if let Some(overlap) = key.overlap(extent_key) {
                    let range = device_offset + overlap.start - extent_key.range.start
                        ..device_offset + overlap.end - extent_key.range.start;
                    if trace {
                        log::info!(
                            "{}.{} D {:?} ({}) from extent {:?}",
                            self.store().store_object_id(),
                            self.object_id,
                            range,
                            range.end - range.start,
                            extent_key
                        );
                    }
                    allocator.deallocate(transaction, range).await?;
                    deallocated += overlap.end - overlap.start;
                } else {
                    break;
                }
            }
            iter.advance().await?;
        }
        Ok(deallocated)
    }

    /// Zeroes the given range.  The range must be aligned.  Returns the amount of data deallocated.
    pub async fn zero(
        &self,
        transaction: &mut Transaction<'_>,
        range: Range<u64>,
    ) -> Result<(), Error> {
        let deallocated = self.deallocate_old_extents(transaction, range.clone()).await?;
        if deallocated > 0 {
            self.update_allocated_size(transaction, 0, deallocated).await?;
            transaction.add(
                self.store().store_object_id,
                Mutation::extent(
                    ExtentKey::new(self.object_id, self.attribute_id, range),
                    ExtentValue::deleted_extent(),
                ),
            );
        }
        Ok(())
    }

    /// Return information on a contiguous set of extents that has the same allocation status,
    /// starting from `start_offset`. The information returned is if this set of extents are marked
    /// allocated/not allocated and also the size of this set (in bytes). This is used when
    /// querying slices for volumes.
    /// This function expects `start_offset` to be aligned to block size
    pub async fn is_allocated(&self, start_offset: u64) -> Result<(bool, u64), Error> {
        let block_size = self.block_size();
        assert_eq!(start_offset % block_size, 0);

        if start_offset > self.get_size() {
            bail!(FxfsError::OutOfRange)
        }

        if start_offset == self.get_size() {
            return Ok((false, 0));
        }

        let tree = &self.store().extent_tree;
        let layer_set = tree.layer_set();
        let offset_key =
            ExtentKey::search_key_from_offset(self.object_id, self.attribute_id, start_offset);
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Included(&offset_key)).await?;

        let mut allocated = None;
        let mut end = start_offset;

        loop {
            // Iterate through the extents, each time setting `end` as the end of the previous
            // extent
            match iter.get() {
                Some(ItemRef { key: extent_key, value: extent_value, .. }) => {
                    // Equivalent of getting no extents back
                    if extent_key.object_id != self.object_id
                        || extent_key.attribute_id != self.attribute_id
                    {
                        if allocated == Some(false) || allocated.is_none() {
                            end = self.get_size();
                            allocated = Some(false);
                        }
                        break;
                    }

                    if extent_key.range.start > end {
                        // If a previous extent has already been visited and we are tracking an
                        // allocated set, we are only interested in an extent where the range of the
                        // current extent follows immediately after the previous one.
                        if allocated == Some(true) {
                            break;
                        } else {
                            // The gap between the previous `end` and this extent is not allocated
                            end = extent_key.range.start;
                            allocated = Some(false);
                            // Continue this iteration, except now the `end` is set to the end of
                            // the "previous" extent which is this gap between the start_offset
                            // and the current extent
                        }
                    }

                    // We can assume that from here, the `end` points to the end of a previous
                    // extent.
                    match extent_value {
                        // The current extent has been allocated
                        ExtentValue::Some { .. } => {
                            // Stop searching if previous extent was marked deleted
                            if allocated == Some(false) {
                                break;
                            }
                            allocated = Some(true);
                        }
                        // This extent has been marked deleted
                        ExtentValue::None => {
                            // Stop searching if previous extent was marked allocated
                            if allocated == Some(true) {
                                break;
                            }
                            allocated = Some(false);
                        }
                    }
                    end = extent_key.range.end;
                    iter.advance().await?;
                }
                // This occurs when there are no extents left
                None => {
                    if allocated == Some(false) || allocated.is_none() {
                        end = self.get_size();
                        allocated = Some(false);
                    }
                    // Otherwise, we were monitoring extents that were allocated, so just exit.
                    break;
                }
            }
        }

        Ok((allocated.unwrap(), end - start_offset))
    }

    pub async fn txn_write<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        offset: u64,
        buf: BufferRef<'_>,
    ) -> Result<(), Error> {
        if buf.is_empty() {
            return Ok(());
        }
        let (aligned, mut transfer_buf) = self.align_buffer(offset, buf).await?;
        self.multi_write(transaction, &[aligned], transfer_buf.as_mut()).await?;
        if offset + buf.len() as u64 > self.txn_get_size(transaction) {
            transaction.add_with_object(
                self.store().store_object_id,
                Mutation::replace_or_insert_object(
                    ObjectKey::attribute(self.object_id, self.attribute_id),
                    ObjectValue::attribute(offset + buf.len() as u64),
                ),
                AssocObj::Borrowed(self),
            );
        }
        Ok(())
    }

    // Writes to multiple ranges with data provided in `buf`.  The buffer can be modified in place
    // if encryption takes place.  The ranges must all be aligned and no change to content size is
    // applied; the caller is responsible for updating size if required.
    pub async fn multi_write<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        ranges: &[Range<u64>],
        mut buf: MutableBufferRef<'_>,
    ) -> Result<(), Error> {
        if buf.is_empty() {
            return Ok(());
        }
        let block_size = self.block_size();
        let store = self.store();
        let store_id = store.store_object_id;

        if let Some(keys) = &self.keys {
            let mut slice = buf.as_mut_slice();
            for r in ranges {
                let l = r.end - r.start;
                let (head, tail) = slice.split_at_mut(l as usize);
                // TODO(https://fxbug.dev/92975): Support key_id != 0.
                keys.encrypt(r.start, 0, head)?;
                slice = tail;
            }
        }

        let mut allocated = 0;
        let allocator = store.allocator();
        let trace = self.trace.load(atomic::Ordering::Relaxed);
        let mut writes = FuturesOrdered::new();
        while !buf.is_empty() {
            let device_range = allocator
                .allocate(transaction, buf.len() as u64)
                .await
                .context("allocation failed")?;
            if trace {
                log::info!(
                    "{}.{} A {:?} ({})",
                    store_id,
                    self.object_id,
                    device_range,
                    device_range.end - device_range.start
                );
            }
            let device_range_len = device_range.end - device_range.start;
            allocated += device_range_len;

            let (head, tail) = buf.split_at_mut(device_range_len as usize);
            buf = tail;

            writes.push(async move {
                let len = head.len() as u64;
                Result::<_, Error>::Ok((
                    device_range.start,
                    len,
                    self.write_aligned(head.as_ref(), device_range.start, true).await?,
                ))
            });
        }

        let (mutations, deallocated) = try_join!(
            async {
                let mut current_range = 0..0;
                let mut mutations = Vec::new();
                let mut ranges = ranges.iter();
                while let Some((mut device_offset, mut len, mut checksums)) =
                    writes.try_next().await?
                {
                    while len > 0 {
                        if current_range.end <= current_range.start {
                            current_range = ranges.next().unwrap().clone();
                        }
                        let l = std::cmp::min(len, current_range.end - current_range.start);
                        let tail = checksums.split_off((l / block_size) as usize);
                        mutations.push(Mutation::extent(
                            ExtentKey::new(
                                self.object_id,
                                self.attribute_id,
                                current_range.start..current_range.start + l,
                            ),
                            ExtentValue::with_checksum(device_offset, checksums),
                        ));
                        checksums = tail;
                        device_offset += l;
                        len -= l;
                        current_range.start += l;
                    }
                }
                Result::<_, Error>::Ok(mutations)
            },
            async {
                let mut deallocated = 0;
                let aligned_size = round_up(self.txn_get_size(transaction), block_size)
                    .ok_or(anyhow!(FxfsError::Inconsistent).context("flush: Bad size"))?;
                for r in ranges {
                    if r.start < aligned_size {
                        deallocated += self.deallocate_old_extents(transaction, r.clone()).await?;
                    }
                }
                Result::<_, Error>::Ok(deallocated)
            }
        )?;
        for m in mutations {
            transaction.add(store_id, m);
        }
        self.update_allocated_size(transaction, allocated, deallocated).await
    }

    // All the extents for the range must have been preallocated using preallocate_range or from
    // existing writes.
    pub async fn overwrite(&self, mut offset: u64, buf: BufferRef<'_>) -> Result<(), Error> {
        let tree = &self.store().extent_tree;
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let end = offset + buf.len() as u64;
        let mut iter = merger
            .seek(Bound::Included(
                &ExtentKey::new(self.object_id, self.attribute_id, offset..end).search_key(),
            ))
            .await?;
        let mut pos = 0;
        loop {
            let (device_offset, to_do) = match iter.get() {
                Some(ItemRef {
                    key: ExtentKey { object_id, attribute_id, range },
                    value: ExtentValue::Some { device_offset, checksums: Checksums::None, .. },
                    ..
                }) if *object_id == self.object_id
                    && *attribute_id == self.attribute_id
                    && range.start <= offset =>
                {
                    (
                        device_offset + (offset - range.start),
                        min(buf.len() - pos, (range.end - offset) as usize),
                    )
                }
                _ => bail!("offset {} not allocated/has checksums", offset),
            };
            self.write_at(offset, buf.subslice(pos..pos + to_do), device_offset, false).await?;
            pos += to_do;
            if pos == buf.len() {
                break;
            }
            offset += to_do as u64;
            iter.advance().await?;
        }
        Ok(())
    }

    // If |transaction| has an impending mutation for the underlying object, returns that.
    // Otherwise, looks up the object from the tree.
    async fn txn_get_object_mutation(
        &self,
        transaction: &Transaction<'_>,
    ) -> Result<ObjectStoreMutation, Error> {
        self.store().txn_get_object_mutation(transaction, self.object_id).await
    }

    // Within a transaction, the size of the object might have changed, so get the size from there
    // if it exists, otherwise, fall back on the cached size.
    fn txn_get_size(&self, transaction: &Transaction<'_>) -> u64 {
        transaction
            .get_object_mutation(
                self.store().store_object_id,
                ObjectKey::attribute(self.object_id, self.attribute_id),
            )
            .and_then(|m| {
                if let ObjectItem { value: ObjectValue::Attribute { size }, .. } = m.item {
                    Some(size)
                } else {
                    None
                }
            })
            .unwrap_or_else(|| self.get_size())
    }

    // TODO(csuter): make this used
    #[cfg(test)]
    async fn get_allocated_size(&self) -> Result<u64, Error> {
        if let ObjectItem {
            value: ObjectValue::Object { kind: ObjectKind::File { allocated_size, .. }, .. },
            ..
        } = self
            .store()
            .tree
            .find(&ObjectKey::object(self.object_id))
            .await?
            .expect("Unable to find object record")
        {
            Ok(allocated_size)
        } else {
            panic!("Unexpected object value");
        }
    }

    async fn update_allocated_size(
        &self,
        transaction: &mut Transaction<'_>,
        allocated: u64,
        deallocated: u64,
    ) -> Result<(), Error> {
        if allocated == deallocated {
            return Ok(());
        }
        let mut mutation = self.txn_get_object_mutation(transaction).await?;
        if let ObjectValue::Object { kind: ObjectKind::File { allocated_size, .. }, .. } =
            &mut mutation.item.value
        {
            // The only way for these to fail are if the volume is inconsistent.
            *allocated_size = allocated_size
                .checked_add(allocated)
                .ok_or_else(|| anyhow!(FxfsError::Inconsistent).context("Allocated size overflow"))?
                .checked_sub(deallocated)
                .ok_or_else(|| {
                    anyhow!(FxfsError::Inconsistent).context("Allocated size overflow")
                })?;
        } else {
            panic!("Unexpceted object value");
        }
        transaction.add(self.store().store_object_id, Mutation::ObjectStore(mutation));
        Ok(())
    }

    pub async fn truncate<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        size: u64,
    ) -> Result<(), Error> {
        let old_size = self.txn_get_size(transaction);
        if self.trace.load(atomic::Ordering::Relaxed) {
            log::info!(
                "{}.{} T {}/{} -> {}",
                self.store().store_object_id(),
                self.object_id,
                old_size,
                self.get_size(),
                size
            );
        }
        if size < old_size {
            let block_size = self.block_size();
            let aligned_size = round_up(size, block_size).ok_or(FxfsError::TooBig)?;
            self.zero(
                transaction,
                aligned_size..round_up(old_size, block_size).ok_or_else(|| {
                    anyhow!(FxfsError::Inconsistent).context("truncate: Bad size")
                })?,
            )
            .await?;
            let to_zero = aligned_size - size;
            if to_zero > 0 {
                assert!(to_zero < block_size);
                // We intentionally use the COW write path even if we're in overwrite mode. There's
                // no need to support overwrite mode here, and it would be difficult since we'd need
                // to transactionalize zeroing the tail of the last block with the other metadata
                // changes, which we don't currently have a way to do.
                // TODO(csuter): This is allocating a small buffer that we'll just end up copying.
                // Is there a better way?
                // TODO(csuter): This might cause an allocation when there needs be none; ideally
                // this would know if the tail block is allocated and if it isn't, it should leave
                // it be.
                let mut buf = self.store().device.allocate_buffer(to_zero as usize);
                buf.as_mut_slice().fill(0);
                self.txn_write(transaction, size, buf.as_ref()).await?;
            }
        }
        transaction.add_with_object(
            self.store().store_object_id,
            Mutation::replace_or_insert_object(
                ObjectKey::attribute(self.object_id, self.attribute_id),
                ObjectValue::attribute(size),
            ),
            AssocObj::Borrowed(self),
        );

        Ok(())
    }

    // Must be multiple of block size.
    pub async fn preallocate_range<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        mut file_range: Range<u64>,
    ) -> Result<Vec<Range<u64>>, Error> {
        assert_eq!(file_range.start % self.block_size(), 0);
        assert_eq!(file_range.end % self.block_size(), 0);
        assert!(self.keys.is_none());
        let mut ranges = Vec::new();
        let tree = &self.store().extent_tree;
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger
            .seek(Bound::Included(
                &ExtentKey::new(self.object_id, self.attribute_id, file_range.clone()).search_key(),
            ))
            .await?;
        let mut allocated = 0;
        'outer: while file_range.start < file_range.end {
            let allocate_end = loop {
                match iter.get() {
                    Some(ItemRef {
                        key: ExtentKey { object_id, attribute_id, range },
                        value: ExtentValue::Some { device_offset, .. },
                        ..
                    }) if *object_id == self.object_id
                        && *attribute_id == self.attribute_id
                        && range.start < file_range.end =>
                    {
                        if range.start <= file_range.start {
                            // Record the existing extent and move on.
                            let device_range = device_offset + file_range.start - range.start
                                ..device_offset + min(range.end, file_range.end) - range.start;
                            file_range.start += device_range.end - device_range.start;
                            ranges.push(device_range);
                            if file_range.start >= file_range.end {
                                break 'outer;
                            }
                            iter.advance().await?;
                            continue;
                        } else {
                            // There's nothing allocated between file_range.start and the beginning
                            // of this extent.
                            break range.start;
                        }
                    }
                    Some(ItemRef {
                        key: ExtentKey { object_id, attribute_id, range },
                        value: ExtentValue::None,
                        ..
                    }) if *object_id == self.object_id
                        && *attribute_id == self.attribute_id
                        && range.end < file_range.end =>
                    {
                        iter.advance().await?;
                    }
                    _ => {
                        // We can just preallocate the rest.
                        break file_range.end;
                    }
                }
            };
            let device_range = self
                .store()
                .allocator()
                .allocate(transaction, allocate_end - file_range.start)
                .await
                .context("Allocation failed")?;
            allocated += device_range.end - device_range.start;
            let this_file_range =
                file_range.start..file_range.start + device_range.end - device_range.start;
            file_range.start = this_file_range.end;
            transaction.add(
                self.store().store_object_id,
                Mutation::extent(
                    ExtentKey::new(self.object_id, self.attribute_id, this_file_range),
                    ExtentValue::new(device_range.start),
                ),
            );
            ranges.push(device_range);
            // If we didn't allocate all that we requested, we'll loop around and try again.
        }
        // Update the file size if it changed.
        if file_range.end > self.txn_get_size(transaction) {
            transaction.add_with_object(
                self.store().store_object_id,
                Mutation::replace_or_insert_object(
                    ObjectKey::attribute(self.object_id, self.attribute_id),
                    ObjectValue::attribute(file_range.end),
                ),
                AssocObj::Borrowed(self),
            );
        }
        self.update_allocated_size(transaction, allocated, 0).await?;
        Ok(ranges)
    }

    pub async fn write_timestamps<'a>(
        &'a self,
        transaction: &mut Transaction<'a>,
        crtime: Option<Timestamp>,
        mtime: Option<Timestamp>,
    ) -> Result<(), Error> {
        if let (None, None) = (crtime.as_ref(), mtime.as_ref()) {
            return Ok(());
        }
        let mut mutation = self.txn_get_object_mutation(transaction).await?;
        if let ObjectValue::Object { ref mut attributes, .. } = mutation.item.value {
            if let Some(time) = crtime {
                attributes.creation_time = time;
            }
            if let Some(time) = mtime {
                attributes.modification_time = time;
            }
        } else {
            bail!(
                anyhow!(FxfsError::Inconsistent).context("write_timestamps: Expected object value")
            );
        };
        transaction.add(self.store().store_object_id(), Mutation::ObjectStore(mutation));
        Ok(())
    }

    pub async fn new_transaction<'b>(&self) -> Result<Transaction<'b>, Error> {
        self.new_transaction_with_options(Options {
            skip_journal_checks: self.options.skip_journal_checks,
            ..Default::default()
        })
        .await
    }

    pub async fn new_transaction_with_options<'b>(
        &self,
        options: Options<'b>,
    ) -> Result<Transaction<'b>, Error> {
        Ok(self
            .store()
            .filesystem()
            .new_transaction(
                &[LockKey::object_attribute(
                    self.store().store_object_id,
                    self.object_id,
                    self.attribute_id,
                )],
                options,
            )
            .await?)
    }

    /// Flushes the underlying device.  This is expensive and should be used sparingly.
    pub async fn flush_device(&self) -> Result<(), Error> {
        self.store().device().flush().await
    }

    async fn read_and_decrypt(
        &self,
        device_offset: u64,
        file_offset: u64,
        mut buffer: MutableBufferRef<'_>,
        key_id: u64,
    ) -> Result<(), Error> {
        self.store().device.read(device_offset, buffer.reborrow()).await?;
        if let Some(keys) = &self.keys {
            keys.decrypt(file_offset, key_id, buffer.as_mut_slice())?;
        }
        Ok(())
    }
}

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> AssociatedObject for StoreObjectHandle<S> {
    fn will_apply_mutation(&self, mutation: &Mutation, _object_id: u64, _manager: &ObjectManager) {
        match mutation {
            Mutation::ObjectStore(ObjectStoreMutation {
                item: ObjectItem { value: ObjectValue::Attribute { size }, .. },
                ..
            }) => self.content_size.store(*size, atomic::Ordering::Relaxed),
            _ => {}
        }
    }
}

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> ObjectHandle for StoreObjectHandle<S> {
    fn set_trace(&self, v: bool) {
        log::info!("{}.{} tracing: {}", self.store().store_object_id, self.object_id(), v);
        self.trace.store(v, atomic::Ordering::Relaxed);
    }

    fn object_id(&self) -> u64 {
        return self.object_id;
    }

    fn allocate_buffer(&self, size: usize) -> Buffer<'_> {
        self.store().device.allocate_buffer(size)
    }

    fn block_size(&self) -> u64 {
        self.store().block_size()
    }

    fn get_size(&self) -> u64 {
        self.content_size.load(atomic::Ordering::Relaxed)
    }
}

#[async_trait]
impl<S: AsRef<ObjectStore> + Send + Sync + 'static> GetProperties for StoreObjectHandle<S> {
    // TODO(jfsulliv): Make StoreObjectHandle per-object (not per-attribute as it currently is)
    // and pass in a list of attributes to fetch properties for.
    async fn get_properties(&self) -> Result<ObjectProperties, Error> {
        // Take a read guard since we need to return a consistent view of all object properties.
        let fs = self.store().filesystem();
        let _guard = fs
            .read_lock(&[LockKey::object_attribute(
                self.store().store_object_id,
                self.object_id,
                self.attribute_id,
            )])
            .await;
        let item = self
            .store()
            .tree
            .find(&ObjectKey::object(self.object_id))
            .await?
            .expect("Unable to find object record");
        match item.value {
            ObjectValue::Object {
                kind: ObjectKind::File { refs, allocated_size, .. },
                attributes: ObjectAttributes { creation_time, modification_time },
            } => Ok(ObjectProperties {
                refs,
                allocated_size,
                data_attribute_size: self.get_size(),
                creation_time,
                modification_time,
                sub_dirs: 0,
            }),
            _ => bail!(FxfsError::NotFile),
        }
    }
}

#[async_trait]
impl<S: AsRef<ObjectStore> + Send + Sync + 'static> ReadObjectHandle for StoreObjectHandle<S> {
    async fn read(&self, mut offset: u64, mut buf: MutableBufferRef<'_>) -> Result<usize, Error> {
        if buf.len() == 0 {
            return Ok(0);
        }
        // Whilst the read offset must be aligned to the filesystem block size, the buffer need only
        // be aligned to the device's block size.
        let block_size = self.block_size() as u64;
        let device_block_size = self.store().device.block_size() as u64;
        assert_eq!(offset % block_size, 0);
        assert_eq!(buf.range().start as u64 % device_block_size, 0);
        let fs = self.store().filesystem();
        let _guard = fs
            .read_lock(&[LockKey::object_attribute(
                self.store().store_object_id,
                self.object_id,
                self.attribute_id,
            )])
            .await;
        let size = self.get_size();
        if offset >= size {
            return Ok(0);
        }
        let tree = &self.store().extent_tree;
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger
            .seek(Bound::Included(&ExtentKey::new(
                self.object_id,
                self.attribute_id,
                offset..offset + 1,
            )))
            .await?;
        let to_do = min(buf.len() as u64, size - offset) as usize;
        buf = buf.subslice_mut(0..to_do);
        let end_align = ((offset + to_do as u64) % block_size) as usize;
        let trace = self.trace.load(atomic::Ordering::Relaxed);
        let reads = FuturesUnordered::new();
        while let Some(ItemRef { key: extent_key, value: extent_value, .. }) = iter.get() {
            if extent_key.object_id != self.object_id
                || extent_key.attribute_id != self.attribute_id
            {
                break;
            }
            if extent_key.range.start > offset {
                // Zero everything up to the start of the extent.
                let to_zero = min(extent_key.range.start - offset, buf.len() as u64) as usize;
                for i in &mut buf.as_mut_slice()[..to_zero] {
                    *i = 0;
                }
                buf = buf.subslice_mut(to_zero..);
                if buf.is_empty() {
                    break;
                }
                offset += to_zero as u64;
            }

            if let ExtentValue::Some { device_offset, key_id, .. } = extent_value {
                let mut device_offset = device_offset + (offset - extent_key.range.start);

                let to_copy = min(buf.len() - end_align, (extent_key.range.end - offset) as usize);
                if to_copy > 0 {
                    if trace {
                        log::info!(
                            "{}.{} R {:?}",
                            self.store().store_object_id(),
                            self.object_id,
                            device_offset..device_offset + to_copy as u64
                        );
                    }
                    let (head, tail) = buf.split_at_mut(to_copy);
                    reads.push(self.read_and_decrypt(device_offset, offset, head, *key_id));
                    buf = tail;
                    if buf.is_empty() {
                        break;
                    }
                    offset += to_copy as u64;
                    device_offset += to_copy as u64;
                }

                // Deal with end alignment by reading the existing contents into an alignment
                // buffer.
                if offset < extent_key.range.end && end_align > 0 {
                    let mut align_buf = self.store().device.allocate_buffer(block_size as usize);
                    if trace {
                        log::info!(
                            "{}.{} RT {:?}",
                            self.store().store_object_id(),
                            self.object_id,
                            device_offset..device_offset + align_buf.len() as u64
                        );
                    }
                    self.read_and_decrypt(device_offset, offset, align_buf.as_mut(), *key_id)
                        .await?;
                    buf.as_mut_slice().copy_from_slice(&align_buf.as_slice()[..end_align]);
                    buf = buf.subslice_mut(0..0);
                    break;
                }
            } else if extent_key.range.end >= offset + buf.len() as u64 {
                // Deleted extent covers remainder, so we're done.
                break;
            }

            iter.advance().await?;
        }
        reads.try_collect().await?;
        buf.as_mut_slice().fill(0);
        Ok(to_do)
    }
}

#[async_trait]
impl<S: AsRef<ObjectStore> + Send + Sync + 'static> WriteObjectHandle for StoreObjectHandle<S> {
    async fn write_or_append(&self, offset: Option<u64>, buf: BufferRef<'_>) -> Result<u64, Error> {
        let offset = offset.unwrap_or(self.get_size());
        let mut transaction = self.new_transaction().await?;
        self.txn_write(&mut transaction, offset, buf).await?;
        let new_size = self.txn_get_size(&transaction);
        transaction.commit().await?;
        Ok(new_size)
    }

    async fn truncate(&self, size: u64) -> Result<(), Error> {
        let mut transaction = self.new_transaction().await?;
        StoreObjectHandle::truncate(self, &mut transaction, size).await?;
        transaction.commit().await?;
        Ok(())
    }

    async fn write_timestamps(
        &self,
        crtime: Option<Timestamp>,
        mtime: Option<Timestamp>,
    ) -> Result<(), Error> {
        if let (None, None) = (crtime.as_ref(), mtime.as_ref()) {
            return Ok(());
        }
        let mut transaction = self.new_transaction().await?;
        StoreObjectHandle::write_timestamps(self, &mut transaction, crtime, mtime).await?;
        transaction.commit().await?;
        Ok(())
    }

    async fn flush(&self) -> Result<(), Error> {
        Ok(())
    }
}

/// Like object_handle::Writer, but allows custom transaction options to be set, and makes every
/// write go directly to the handle in a transaction.
pub struct DirectWriter<'a, S: AsRef<ObjectStore> + Send + Sync + 'static> {
    handle: &'a StoreObjectHandle<S>,
    options: transaction::Options<'a>,
    buffer: Buffer<'a>,
    offset: u64,
    buf_offset: usize,
}

const BUFFER_SIZE: usize = 1_048_576;

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> Drop for DirectWriter<'_, S> {
    fn drop(&mut self) {
        if self.buf_offset != 0 {
            log::warn!("DirectWriter: dropping data, did you forget to call complete?");
        }
    }
}

impl<'a, S: AsRef<ObjectStore> + Send + Sync + 'static> DirectWriter<'a, S> {
    pub fn new(handle: &'a StoreObjectHandle<S>, options: transaction::Options<'a>) -> Self {
        Self {
            handle,
            options,
            buffer: handle.allocate_buffer(BUFFER_SIZE),
            offset: 0,
            buf_offset: 0,
        }
    }

    pub async fn flush(&mut self) -> Result<(), Error> {
        let mut transaction = self.handle.new_transaction_with_options(self.options).await?;
        self.handle
            .txn_write(&mut transaction, self.offset, self.buffer.subslice(..self.buf_offset))
            .await?;
        transaction.commit().await?;
        self.offset += self.buf_offset as u64;
        self.buf_offset = 0;
        Ok(())
    }
}

#[async_trait]
impl<'a, S: AsRef<ObjectStore> + Send + Sync + 'static> WriteBytes for DirectWriter<'a, S> {
    fn handle(&self) -> &dyn WriteObjectHandle {
        self.handle
    }

    async fn write_bytes(&mut self, mut buf: &[u8]) -> Result<(), Error> {
        while buf.len() > 0 {
            let to_do = std::cmp::min(buf.len(), BUFFER_SIZE - self.buf_offset);
            self.buffer
                .subslice_mut(self.buf_offset..self.buf_offset + to_do)
                .as_mut_slice()
                .copy_from_slice(&buf[..to_do]);
            self.buf_offset += to_do;
            if self.buf_offset == BUFFER_SIZE {
                self.flush().await?;
            }
            buf = &buf[to_do..];
        }
        Ok(())
    }

    async fn complete(&mut self) -> Result<(), Error> {
        self.flush().await?;
        Ok(())
    }

    async fn skip(&mut self, amount: u64) -> Result<(), Error> {
        if (BUFFER_SIZE - self.buf_offset) as u64 > amount {
            self.buffer
                .subslice_mut(self.buf_offset..self.buf_offset + amount as usize)
                .as_mut_slice()
                .fill(0);
            self.buf_offset += amount as usize;
        } else {
            self.flush().await?;
            self.offset += amount;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        crate::{
            crypt::{Crypt, InsecureCrypt},
            lsm_tree::types::{ItemRef, LayerIterator},
            object_handle::{
                GetProperties, ObjectHandle, ObjectProperties, ReadObjectHandle, WriteObjectHandle,
            },
            object_store::{
                extent_record::ExtentKey,
                filesystem::{Filesystem, FxFilesystem, Mutations, OpenFxFilesystem},
                object_record::{ObjectKey, ObjectKeyData, ObjectValue, Timestamp},
                transaction::{Options, TransactionHandler},
                HandleOptions, ObjectStore, StoreObjectHandle,
            },
            round::{round_down, round_up},
        },
        assert_matches::assert_matches,
        fuchsia_async as fasync,
        futures::{channel::oneshot::channel, join},
        rand::Rng,
        std::{
            ops::{Bound, Range},
            sync::{Arc, Mutex},
            time::Duration,
        },
        storage_device::{fake_device::FakeDevice, DeviceHolder},
    };

    const TEST_DEVICE_BLOCK_SIZE: u32 = 512;

    // Some tests (the preallocate_range ones) currently assume that the data only occupies a single
    // device block.
    const TEST_DATA_OFFSET: u64 = 5000;
    const TEST_DATA: &[u8] = b"hello";
    const TEST_OBJECT_SIZE: u64 = 5678;
    const TEST_OBJECT_ALLOCATED_SIZE: u64 = 4096;

    async fn test_filesystem() -> OpenFxFilesystem {
        let device = DeviceHolder::new(FakeDevice::new(8192, TEST_DEVICE_BLOCK_SIZE));
        FxFilesystem::new_empty(device).await.expect("new_empty failed")
    }

    async fn test_filesystem_and_object_with_key(
        crypt: Option<&dyn Crypt>,
    ) -> (OpenFxFilesystem, StoreObjectHandle<ObjectStore>) {
        let fs = test_filesystem().await;
        let store = fs.root_store();
        let object;
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        object =
            ObjectStore::create_object(&store, &mut transaction, HandleOptions::default(), crypt)
                .await
                .expect("create_object failed");
        {
            let align = TEST_DATA_OFFSET as usize % TEST_DEVICE_BLOCK_SIZE as usize;
            let mut buf = object.allocate_buffer(align + TEST_DATA.len());
            buf.as_mut_slice()[align..].copy_from_slice(TEST_DATA);
            object
                .txn_write(&mut transaction, TEST_DATA_OFFSET, buf.subslice(align..))
                .await
                .expect("write failed");
        }
        object.truncate(&mut transaction, TEST_OBJECT_SIZE).await.expect("truncate failed");
        transaction.commit().await.expect("commit failed");
        (fs, object)
    }

    async fn test_filesystem_and_object() -> (OpenFxFilesystem, StoreObjectHandle<ObjectStore>) {
        test_filesystem_and_object_with_key(Some(&InsecureCrypt::new())).await
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_zero_buf_len_read() {
        let (fs, object) = test_filesystem_and_object().await;
        let mut buf = object.allocate_buffer(0);
        assert_eq!(object.read(0u64, buf.as_mut()).await.expect("read failed"), 0);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_beyond_eof_read() {
        let (fs, object) = test_filesystem_and_object().await;
        let offset = TEST_OBJECT_SIZE as usize - 2;
        let align = offset % fs.block_size() as usize;
        let len: usize = 2;
        let mut buf = object.allocate_buffer(align + len + 1);
        buf.as_mut_slice().fill(123u8);
        assert_eq!(
            object.read((offset - align) as u64, buf.as_mut()).await.expect("read failed"),
            align + len
        );
        assert_eq!(&buf.as_slice()[align..align + len], &vec![0u8; len]);
        assert_eq!(&buf.as_slice()[align + len..], &vec![123u8; buf.len() - align - len]);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_sparse() {
        let (fs, object) = test_filesystem_and_object().await;
        // Deliberately read not right to eof.
        let len = TEST_OBJECT_SIZE as usize - 1;
        let mut buf = object.allocate_buffer(len);
        buf.as_mut_slice().fill(123u8);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), len);
        let mut expected = vec![0; len];
        let offset = TEST_DATA_OFFSET as usize;
        expected[offset..offset + TEST_DATA.len()].copy_from_slice(TEST_DATA);
        assert_eq!(buf.as_slice()[..len], expected[..]);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_after_writes_interspersed_with_flush() {
        let (fs, object) = test_filesystem_and_object().await;

        object.owner().flush().await.expect("flush failed");

        // Write more test data to the first block fo the file.
        let mut buf = object.allocate_buffer(TEST_DATA.len());
        buf.as_mut_slice().copy_from_slice(TEST_DATA);
        object.write_or_append(Some(0u64), buf.as_ref()).await.expect("write failed");

        let len = TEST_OBJECT_SIZE as usize - 1;
        let mut buf = object.allocate_buffer(len);
        buf.as_mut_slice().fill(123u8);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), len);

        let mut expected = vec![0u8; len];
        let offset = TEST_DATA_OFFSET as usize;
        expected[offset..offset + TEST_DATA.len()].copy_from_slice(TEST_DATA);
        expected[..TEST_DATA.len()].copy_from_slice(TEST_DATA);
        assert_eq!(buf.as_slice(), &expected);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_after_truncate_and_extend() {
        let (fs, object) = test_filesystem_and_object().await;

        // Arrange for there to be <extent><deleted-extent><extent>.
        let mut buf = object.allocate_buffer(TEST_DATA.len());
        buf.as_mut_slice().copy_from_slice(TEST_DATA);
        object.write_or_append(Some(0), buf.as_ref()).await.expect("write failed"); // This adds an extent at 0..512.
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        object.truncate(&mut transaction, 3).await.expect("truncate failed"); // This deletes 512..1024.
        transaction.commit().await.expect("commit failed");
        let data = b"foo";
        let offset = 1500u64;
        let align = (offset % fs.block_size() as u64) as usize;
        let mut buf = object.allocate_buffer(align + data.len());
        buf.as_mut_slice()[align..].copy_from_slice(data);
        object.write_or_append(Some(1500), buf.subslice(align..)).await.expect("write failed"); // This adds 1024..1536.

        const LEN1: usize = 1503;
        let mut buf = object.allocate_buffer(LEN1);
        buf.as_mut_slice().fill(123u8);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), LEN1);
        let mut expected = [0; LEN1];
        expected[..3].copy_from_slice(&TEST_DATA[..3]);
        expected[1500..].copy_from_slice(b"foo");
        assert_eq!(buf.as_slice(), &expected);

        // Also test a read that ends midway through the deleted extent.
        const LEN2: usize = 601;
        let mut buf = object.allocate_buffer(LEN2);
        buf.as_mut_slice().fill(123u8);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), LEN2);
        assert_eq!(buf.as_slice(), &expected[..LEN2]);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_whole_blocks_with_multiple_objects() {
        let (fs, object) = test_filesystem_and_object().await;
        let bs = object.block_size() as usize;
        let mut buffer = object.allocate_buffer(bs);
        buffer.as_mut_slice().fill(0xaf);
        object.write_or_append(Some(0), buffer.as_ref()).await.expect("write failed");

        let store = object.owner();
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let object2 =
            ObjectStore::create_object(&store, &mut transaction, HandleOptions::default(), None)
                .await
                .expect("create_object failed");
        transaction.commit().await.expect("commit failed");
        let mut ef_buffer = object.allocate_buffer(bs);
        ef_buffer.as_mut_slice().fill(0xef);
        object2.write_or_append(Some(0), ef_buffer.as_ref()).await.expect("write failed");

        let mut buffer = object.allocate_buffer(bs);
        buffer.as_mut_slice().fill(0xaf);
        object.write_or_append(Some(bs as u64), buffer.as_ref()).await.expect("write failed");
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object.truncate(&mut transaction, 3 * bs as u64).await.expect("truncate failed");
        transaction.commit().await.expect("commit failed");
        object2.write_or_append(Some(bs as u64), ef_buffer.as_ref()).await.expect("write failed");

        let mut buffer = object.allocate_buffer(4 * bs);
        buffer.as_mut_slice().fill(123);
        assert_eq!(object.read(0, buffer.as_mut()).await.expect("read failed"), 3 * bs);
        assert_eq!(&buffer.as_slice()[..2 * bs], &vec![0xaf; 2 * bs]);
        assert_eq!(&buffer.as_slice()[2 * bs..3 * bs], &vec![0; bs]);
        assert_eq!(object2.read(0, buffer.as_mut()).await.expect("read failed"), 2 * bs);
        assert_eq!(&buffer.as_slice()[..2 * bs], &vec![0xef; 2 * bs]);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_alignment() {
        let (fs, object) = test_filesystem_and_object().await;

        struct AlignTest {
            fill: u8,
            object: StoreObjectHandle<ObjectStore>,
            mirror: Vec<u8>,
        }

        impl AlignTest {
            async fn new(object: StoreObjectHandle<ObjectStore>) -> Self {
                let mirror = {
                    let mut buf = object.allocate_buffer(object.get_size() as usize);
                    assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), buf.len());
                    buf.as_slice().to_vec()
                };
                Self { fill: 0, object, mirror }
            }

            async fn test(&mut self, range: Range<u64>) {
                let mut buf = self.object.allocate_buffer((range.end - range.start) as usize);
                self.fill += 1;
                buf.as_mut_slice().fill(self.fill);
                self.object
                    .write_or_append(Some(range.start), buf.as_ref())
                    .await
                    .expect("write_or_append failed");
                if range.end > self.mirror.len() as u64 {
                    self.mirror.resize(range.end as usize, 0);
                }
                self.mirror[range.start as usize..range.end as usize].fill(self.fill);
                let mut buf = self.object.allocate_buffer(self.mirror.len() + 1);
                assert_eq!(
                    self.object.read(0, buf.as_mut()).await.expect("read failed"),
                    self.mirror.len()
                );
                assert_eq!(&buf.as_slice()[..self.mirror.len()], self.mirror.as_slice());
            }
        }

        let block_size = object.block_size() as u64;
        let mut align = AlignTest::new(object).await;

        // Fill the object to start with.
        align.test(0..2 * block_size + 1).await;

        // Unaligned head.
        align.test(1..block_size).await;
        align.test(1..2 * block_size).await;

        // Unaligned tail.
        align.test(0..block_size - 1).await;
        align.test(0..2 * block_size - 1).await;

        // Both unaligned.
        align.test(1..block_size - 1).await;
        align.test(1..2 * block_size - 1).await;

        fs.close().await.expect("Close failed");
    }

    async fn test_preallocate_common(fs: &FxFilesystem, object: StoreObjectHandle<ObjectStore>) {
        let allocator = fs.allocator();
        let allocated_before = allocator.get_allocated_bytes();
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object
            .preallocate_range(&mut transaction, 0..fs.block_size() as u64)
            .await
            .expect("preallocate_range failed");
        transaction.commit().await.expect("commit failed");
        assert!(object.get_size() < 1048576);
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object
            .preallocate_range(&mut transaction, 0..1048576)
            .await
            .expect("preallocate_range failed");
        transaction.commit().await.expect("commit failed");
        assert_eq!(object.get_size(), 1048576);
        // Check that it didn't reallocate the space for the existing extent
        let allocated_after = allocator.get_allocated_bytes();
        assert_eq!(allocated_after - allocated_before, 1048576 - fs.block_size() as u64);

        let mut buf =
            object.allocate_buffer(round_up(TEST_DATA_OFFSET, fs.block_size()).unwrap() as usize);
        buf.as_mut_slice().fill(47);
        object
            .write_or_append(Some(0), buf.subslice(..TEST_DATA_OFFSET as usize))
            .await
            .expect("write failed");
        buf.as_mut_slice().fill(95);
        let offset = round_up(TEST_OBJECT_SIZE, fs.block_size()).unwrap();
        object.overwrite(offset, buf.as_ref()).await.expect("write failed");

        // Make sure there were no more allocations.
        assert_eq!(allocator.get_allocated_bytes(), allocated_after);

        // Read back the data and make sure it is what we expect.
        let mut buf = object.allocate_buffer(104876);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), buf.len());
        assert_eq!(&buf.as_slice()[..TEST_DATA_OFFSET as usize], &[47; TEST_DATA_OFFSET as usize]);
        assert_eq!(
            &buf.as_slice()[TEST_DATA_OFFSET as usize..TEST_DATA_OFFSET as usize + TEST_DATA.len()],
            TEST_DATA
        );
        assert_eq!(&buf.as_slice()[offset as usize..offset as usize + 2048], &[95; 2048]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_preallocate_range() {
        let (fs, object) = test_filesystem_and_object_with_key(None).await;
        test_preallocate_common(&fs, object).await;
        fs.close().await.expect("Close failed");
    }

    // This is identical to the previous test except that we flush so that extents end up in
    // different layers.
    #[fasync::run_singlethreaded(test)]
    async fn test_preallocate_suceeds_when_extents_are_in_different_layers() {
        let (fs, object) = test_filesystem_and_object_with_key(None).await;
        object.owner().flush().await.expect("flush failed");
        test_preallocate_common(&fs, object).await;
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_already_preallocated() {
        let (fs, object) = test_filesystem_and_object_with_key(None).await;
        let allocator = fs.allocator();
        let allocated_before = allocator.get_allocated_bytes();
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        let offset = TEST_DATA_OFFSET - TEST_DATA_OFFSET % fs.block_size() as u64;
        object
            .preallocate_range(&mut transaction, offset..offset + fs.block_size() as u64)
            .await
            .expect("preallocate_range failed");
        transaction.commit().await.expect("commit failed");
        // Check that it didn't reallocate any new space.
        assert_eq!(allocator.get_allocated_bytes(), allocated_before);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_overwrite_fails_if_not_preallocated() {
        let (fs, object) = test_filesystem_and_object().await;

        let object = ObjectStore::open_object(
            &object.owner,
            object.object_id(),
            HandleOptions::default(),
            Some(&InsecureCrypt::new()),
        )
        .await
        .expect("open_object failed");
        let mut buf = object.allocate_buffer(2048);
        buf.as_mut_slice().fill(95);
        let offset = round_up(TEST_OBJECT_SIZE, fs.block_size()).unwrap();
        object.overwrite(offset, buf.as_ref()).await.expect_err("write succeeded");
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_extend() {
        let fs = test_filesystem().await;
        let handle;
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let store = fs.root_store();
        handle =
            ObjectStore::create_object(&store, &mut transaction, HandleOptions::default(), None)
                .await
                .expect("create_object failed");
        handle
            .extend(&mut transaction, 0..5 * fs.block_size() as u64)
            .await
            .expect("extend failed");
        transaction.commit().await.expect("commit failed");
        let mut buf = handle.allocate_buffer(5 * fs.block_size() as usize);
        buf.as_mut_slice().fill(123);
        handle.write_or_append(Some(0), buf.as_ref()).await.expect("write failed");
        buf.as_mut_slice().fill(67);
        handle.read(0, buf.as_mut()).await.expect("read failed");
        assert_eq!(buf.as_slice(), &vec![123; 5 * fs.block_size() as usize]);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_truncate_deallocates_old_extents() {
        let (fs, object) = test_filesystem_and_object().await;
        let mut buf = object.allocate_buffer(5 * fs.block_size() as usize);
        buf.as_mut_slice().fill(0xaa);
        object.write_or_append(Some(0), buf.as_ref()).await.expect("write failed");

        let allocator = fs.allocator();
        let allocated_before = allocator.get_allocated_bytes();
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        object.truncate(&mut transaction, fs.block_size() as u64).await.expect("truncate failed");
        transaction.commit().await.expect("commit failed");
        let allocated_after = allocator.get_allocated_bytes();
        assert!(
            allocated_after < allocated_before,
            "before = {} after = {}",
            allocated_before,
            allocated_after
        );
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_adjust_refs() {
        let (fs, object) = test_filesystem_and_object().await;
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let store = object.owner();
        assert_eq!(
            store
                .adjust_refs(&mut transaction, object.object_id(), 1)
                .await
                .expect("adjust_refs failed"),
            false
        );
        transaction.commit().await.expect("commit failed");

        let allocator = fs.allocator();
        let allocated_before = allocator.get_allocated_bytes();
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        assert_eq!(
            store
                .adjust_refs(&mut transaction, object.object_id(), -2)
                .await
                .expect("adjust_refs failed"),
            true
        );
        transaction.commit().await.expect("commit failed");

        assert_eq!(allocator.get_allocated_bytes(), allocated_before);

        store
            .tombstone(
                object.object_id,
                Options { borrow_metadata_space: true, ..Default::default() },
            )
            .await
            .expect("purge failed");

        assert_eq!(allocated_before - allocator.get_allocated_bytes(), fs.block_size() as u64,);

        let layer_set = store.tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await.expect("seek failed");
        let mut found = false;
        while let Some(ItemRef { key: ObjectKey { object_id, data }, value, .. }) = iter.get() {
            if *object_id == object.object_id() {
                match (data, value) {
                    (ObjectKeyData::Object, ObjectValue::None) => {
                        assert!(!found);
                        found = true;
                    }
                    _ => assert!(false, "Unexpected item {:?}", iter.get()),
                }
            }
            iter.advance().await.expect("advance failed");
        }
        assert!(found);

        // Make sure there are no extents.
        let layer_set = store.extent_tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await.expect("seek failed");
        while let Some(ItemRef { key: ExtentKey { object_id, .. }, value, .. }) = iter.get() {
            assert!(*object_id != object.object_id() || value.is_deleted());
            iter.advance().await.expect("advance failed");
        }

        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_locks() {
        let (fs, object) = test_filesystem_and_object().await;
        let (send1, recv1) = channel();
        let (send2, recv2) = channel();
        let (send3, recv3) = channel();
        let done = Mutex::new(false);
        join!(
            async {
                let mut t = object.new_transaction().await.expect("new_transaction failed");
                send1.send(()).unwrap(); // Tell the next future to continue.
                send3.send(()).unwrap(); // Tell the last future to continue.
                recv2.await.unwrap();
                let mut buf = object.allocate_buffer(5);
                buf.as_mut_slice().copy_from_slice(b"hello");
                object.txn_write(&mut t, 0, buf.as_ref()).await.expect("write failed");
                // This is a halting problem so all we can do is sleep.
                fasync::Timer::new(Duration::from_millis(100)).await;
                assert!(!*done.lock().unwrap());
                t.commit().await.expect("commit failed");
            },
            async {
                recv1.await.unwrap();
                // Reads should not block.
                let offset = TEST_DATA_OFFSET as usize;
                let align = offset % fs.block_size() as usize;
                let len = TEST_DATA.len();
                let mut buf = object.allocate_buffer(align + len);
                assert_eq!(
                    object.read((offset - align) as u64, buf.as_mut()).await.expect("read failed"),
                    align + TEST_DATA.len()
                );
                assert_eq!(&buf.as_slice()[align..], TEST_DATA);
                // Tell the first future to continue.
                send2.send(()).unwrap();
            },
            async {
                // This should block until the first future has completed.
                recv3.await.unwrap();
                let _t = object.new_transaction().await.expect("new_transaction failed");
                let mut buf = object.allocate_buffer(5);
                assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed"), 5);
                assert_eq!(buf.as_slice(), b"hello");
            }
        );
        fs.close().await.expect("Close failed");
    }

    #[fasync::run(10, test)]
    async fn test_racy_reads() {
        let fs = test_filesystem().await;
        let object;
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let store = fs.root_store();
        object = Arc::new(
            ObjectStore::create_object(&store, &mut transaction, HandleOptions::default(), None)
                .await
                .expect("create_object failed"),
        );
        transaction.commit().await.expect("commit failed");
        for _ in 0..100 {
            let cloned_object = object.clone();
            let writer = fasync::Task::spawn(async move {
                let mut buf = cloned_object.allocate_buffer(10);
                buf.as_mut_slice().fill(123);
                cloned_object.write_or_append(Some(0), buf.as_ref()).await.expect("write failed");
            });
            let cloned_object = object.clone();
            let reader = fasync::Task::spawn(async move {
                let wait_time = rand::thread_rng().gen_range(0..5);
                fasync::Timer::new(Duration::from_millis(wait_time)).await;
                let mut buf = cloned_object.allocate_buffer(10);
                buf.as_mut_slice().fill(23);
                let amount = cloned_object.read(0, buf.as_mut()).await.expect("write failed");
                // If we succeed in reading data, it must include the write; i.e. if we see the size
                // change, we should see the data too.  For this to succeed it requires locking on
                // the read size to ensure that when we read the size, we get the extents changed in
                // that same transaction.
                if amount != 0 {
                    assert_eq!(amount, 10);
                    assert_eq!(buf.as_slice(), &[123; 10]);
                }
            });
            writer.await;
            reader.await;
            let mut transaction = object.new_transaction().await.expect("new_transaction failed");
            object.truncate(&mut transaction, 0).await.expect("truncate failed");
            transaction.commit().await.expect("commit failed");
        }
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_allocated_size() {
        let (fs, object) = test_filesystem_and_object_with_key(None).await;

        let before = object.get_allocated_size().await.expect("get_allocated_size failed");
        let mut buf = object.allocate_buffer(5);
        buf.as_mut_slice().copy_from_slice(b"hello");
        object.write_or_append(Some(0), buf.as_ref()).await.expect("write failed");
        let after = object.get_allocated_size().await.expect("get_allocated_size failed");
        assert_eq!(after, before + fs.block_size() as u64);

        // Do the same write again and there should be no change.
        object.write_or_append(Some(0), buf.as_ref()).await.expect("write failed");
        assert_eq!(object.get_allocated_size().await.expect("get_allocated_size failed"), after);

        // extend...
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        let offset = 1000 * fs.block_size() as u64;
        let before = after;
        object
            .extend(&mut transaction, offset..offset + fs.block_size() as u64)
            .await
            .expect("extend failed");
        transaction.commit().await.expect("commit failed");
        let after = object.get_allocated_size().await.expect("get_allocated_size failed");
        assert_eq!(after, before + fs.block_size() as u64);

        // truncate...
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        let before = after;
        let size = object.get_size();
        object
            .truncate(&mut transaction, size - fs.block_size() as u64)
            .await
            .expect("extend failed");
        transaction.commit().await.expect("commit failed");
        let after = object.get_allocated_size().await.expect("get_allocated_size failed");
        assert_eq!(after, before - fs.block_size() as u64);

        // preallocate_range...
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        let before = after;
        object
            .preallocate_range(&mut transaction, offset..offset + fs.block_size() as u64)
            .await
            .expect("extend failed");
        transaction.commit().await.expect("commit failed");
        let after = object.get_allocated_size().await.expect("get_allocated_size failed");
        assert_eq!(after, before + fs.block_size() as u64);
        fs.close().await.expect("Close failed");
    }

    #[fasync::run(10, test)]
    async fn test_zero() {
        let (fs, object) = test_filesystem_and_object().await;
        let expected_size = object.get_size();
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object.zero(&mut transaction, 0..fs.block_size() as u64 * 10).await.expect("zero failed");
        transaction.commit().await.expect("commit failed");
        assert_eq!(object.get_size(), expected_size);
        let mut buf = object.allocate_buffer(fs.block_size() as usize * 10);
        assert_eq!(object.read(0, buf.as_mut()).await.expect("read failed") as u64, expected_size);
        assert_eq!(
            &buf.as_slice()[0..expected_size as usize],
            vec![0u8; expected_size as usize].as_slice()
        );
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_properties() {
        let (fs, object) = test_filesystem_and_object().await;
        const CRTIME: Timestamp = Timestamp::from_nanos(1234);
        const MTIME: Timestamp = Timestamp::from_nanos(5678);

        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object
            .write_timestamps(&mut transaction, Some(CRTIME), Some(MTIME))
            .await
            .expect("update_timestamps failed");
        transaction.commit().await.expect("commit failed");

        let properties = object.get_properties().await.expect("get_properties failed");
        assert_matches!(
            properties,
            ObjectProperties {
                refs: 1u64,
                allocated_size: TEST_OBJECT_ALLOCATED_SIZE,
                data_attribute_size: TEST_OBJECT_SIZE,
                creation_time: CRTIME,
                modification_time: MTIME,
                ..
            }
        );
        fs.close().await.expect("Close failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_is_allocated() {
        let (fs, object) = test_filesystem_and_object().await;

        // `test_filesystem_and_object()` wrote the buffer `TEST_DATA` to the device at offset
        // `TEST_DATA_OFFSET` where the length and offset are aligned to the block size.
        let aligned_offset = round_down(TEST_DATA_OFFSET, fs.block_size());
        let aligned_length = round_up(TEST_DATA.len() as u64, fs.block_size()).unwrap();

        // Check for the case where where we have the following extent layout
        //       [ unallocated ][ `TEST_DATA` ]
        // The extents before `aligned_offset` should not be allocated
        let (allocated, count) = object.is_allocated(0).await.expect("is_allocated failed");
        assert_eq!(count, aligned_offset);
        assert_eq!(allocated, false);

        let (allocated, count) =
            object.is_allocated(aligned_offset).await.expect("is_allocated failed");
        assert_eq!(count, aligned_length);
        assert_eq!(allocated, true);

        // Check for the case where where we query out of range
        let end = aligned_offset + aligned_length;
        object
            .is_allocated(end)
            .await
            .expect_err("is_allocated should have returned ERR_OUT_OF_RANGE");

        // Check for the case where where we start querying for allocation starting from
        // an allocated range to the end of the device
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        let size = 50 * fs.block_size() as u64;
        object.truncate(&mut transaction, size).await.expect("extend failed");
        transaction.commit().await.expect("commit failed");

        let (allocated, count) = object.is_allocated(end).await.expect("is_allocated failed");
        assert_eq!(count, size - end);
        assert_eq!(allocated, false);

        // Check for the case where where we have the following extent layout
        //      [ unallocated ][ `buf` ][ `buf` ]
        let buf_length = 5 * fs.block_size();
        let mut buf = object.allocate_buffer(buf_length as usize);
        buf.as_mut_slice().fill(123);
        let new_offset = end + 20 * fs.block_size() as u64;
        object.write_or_append(Some(new_offset), buf.as_ref()).await.expect("write failed");
        object
            .write_or_append(Some(new_offset + buf_length), buf.as_ref())
            .await
            .expect("write failed");

        let (allocated, count) = object.is_allocated(end).await.expect("is_allocated failed");
        assert_eq!(count, new_offset - end);
        assert_eq!(allocated, false);

        let (allocated, count) =
            object.is_allocated(new_offset).await.expect("is_allocated failed");
        assert_eq!(count, 2 * buf_length);
        assert_eq!(allocated, true);

        // Check the case where we query from the middle of an extent
        let (allocated, count) = object
            .is_allocated(new_offset + 4 * fs.block_size())
            .await
            .expect("is_allocated failed");
        assert_eq!(count, 2 * buf_length - 4 * fs.block_size());
        assert_eq!(allocated, true);

        // Now, write buffer to a location already written to.
        // Check for the case when we the following extent layout
        //      [ unallocated ][ `other_buf` ][ (part of) `buf` ][ `buf` ]
        let other_buf_length = 3 * fs.block_size();
        let mut other_buf = object.allocate_buffer(other_buf_length as usize);
        other_buf.as_mut_slice().fill(231);
        object.write_or_append(Some(new_offset), other_buf.as_ref()).await.expect("write failed");

        // We still expect that `is_allocated(..)` will return that  there are 2*`buf_length bytes`
        // allocated from `new_offset`
        let (allocated, count) =
            object.is_allocated(new_offset).await.expect("is_allocated failed");
        assert_eq!(count, 2 * buf_length);
        assert_eq!(allocated, true);

        // Check for the case when we the following extent layout
        //   [ unallocated ][ deleted ][ unallocated ][ deleted ][ allocated ]
        // Mark TEST_DATA as deleted
        let mut transaction = object.new_transaction().await.expect("new_transaction failed");
        object
            .zero(&mut transaction, aligned_offset..aligned_offset + aligned_length)
            .await
            .expect("zero failed");
        // Mark `other_buf` as deleted
        object
            .zero(&mut transaction, new_offset..new_offset + buf_length)
            .await
            .expect("zero failed");
        transaction.commit().await.expect("commit transaction failed");

        let (allocated, count) = object.is_allocated(0).await.expect("is_allocated failed");
        assert_eq!(count, new_offset + buf_length);
        assert_eq!(allocated, false);

        let (allocated, count) =
            object.is_allocated(new_offset + buf_length).await.expect("is_allocated failed");
        assert_eq!(count, buf_length);
        assert_eq!(allocated, true);

        let new_end = new_offset + buf_length + count;

        // Check for the case where there are objects with different keys.
        // Case that we're checking for:
        //      [ unallocated ][ extent (object with different key) ][ unallocated ]
        let store = object.owner();
        let mut transaction = fs
            .clone()
            .new_transaction(&[], Options::default())
            .await
            .expect("new_transaction failed");
        let object2 =
            ObjectStore::create_object(&store, &mut transaction, HandleOptions::default(), None)
                .await
                .expect("create_object failed");
        transaction.commit().await.expect("commit failed");

        object2
            .write_or_append(Some(new_end + fs.block_size()), buf.as_ref())
            .await
            .expect("write failed");

        // Expecting that the extent with a different key is treated like unallocated extent
        let (allocated, count) = object.is_allocated(new_end).await.expect("is_allocated failed");
        assert_eq!(count, size - new_end);
        assert_eq!(allocated, false);

        fs.close().await.expect("close failed");
    }
}
