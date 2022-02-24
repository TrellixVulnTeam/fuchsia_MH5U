use crate::core;
use crate::core::cmp;
use crate::io;
use crate::io::prelude::*;
use crate::io::{ErrorKind, SeekFrom};

use crate::dir_entry::DirEntryEditor;
use crate::error::FatfsError;
use crate::fs::{FileSystem, ReadWriteSeek};
use crate::time::{Date, DateTime, TimeProvider};

pub const MAX_FILE_SIZE: u32 = core::u32::MAX;

/// A FAT filesystem file object used for reading and writing data.
///
/// This struct is created by the `open_file` or `create_file` methods on `Dir`.
pub struct File<'a, IO: ReadWriteSeek, TP, OCC> {
    // Note first_cluster is None if file is empty
    first_cluster: Option<u32>,
    // Note: if offset points between clusters current_cluster is the previous cluster
    current_cluster: Option<u32>,
    // current position in this file
    offset: u32,
    // file dir entry editor - None for root dir
    entry: Option<DirEntryEditor>,
    // file-system reference
    fs: &'a FileSystem<IO, TP, OCC>,
}

impl<'a, IO: ReadWriteSeek, TP, OCC> File<'a, IO, TP, OCC> {
    pub(crate) fn new(
        first_cluster: Option<u32>,
        entry: Option<DirEntryEditor>,
        fs: &'a FileSystem<IO, TP, OCC>,
    ) -> Self {
        File {
            first_cluster,
            entry,
            fs,
            current_cluster: None, // cluster before first one
            offset: 0,
        }
    }

    /// Truncate file in current position.
    pub fn truncate(&mut self) -> io::Result<()> {
        if let Some(ref mut e) = self.entry {
            e.set_size(self.offset);
            if self.offset == 0 {
                e.set_first_cluster(None, self.fs.fat_type());
            }
        }
        if self.offset > 0 {
            debug_assert!(self.current_cluster.is_some());
            // if offset is not 0 current cluster cannot be empty
            self.fs.truncate_cluster_chain(self.current_cluster.unwrap()) // SAFE
        } else {
            debug_assert!(self.current_cluster.is_none());
            if let Some(n) = self.first_cluster {
                self.fs.free_cluster_chain(n)?;
                self.first_cluster = None;
            }
            Ok(())
        }
    }

    pub(crate) fn abs_pos(&self) -> Result<Option<u64>, FatfsError> {
        // Returns current position relative to filesystem start
        // Note: when between clusters it returns position after previous cluster
        Ok(match self.current_cluster {
            Some(n) => {
                assert!(self.offset > 0); // offset == 0 should mean current_clusters is None.
                let cluster_size = self.fs.cluster_size();
                // We want offset_in_cluster to be in the range [1..cluster_size], and not
                // [0..cluster_size - 1].
                let offset_in_cluster = (self.offset - 1) % cluster_size + 1;
                let offset_in_fs = self.fs.offset_from_cluster(n)? + (offset_in_cluster as u64);
                Some(offset_in_fs)
            }
            None => None,
        })
    }

    pub fn flush_dir_entry(&mut self) -> io::Result<()> {
        if let Some(ref mut e) = self.entry {
            e.flush(self.fs)?;
        }
        Ok(())
    }

    /// Sets date and time of creation for this file.
    ///
    /// Note: it is set to a value from the `TimeProvider` when creating a file.
    /// Deprecated: if needed implement a custom `TimeProvider`.
    #[deprecated]
    pub fn set_created(&mut self, date_time: DateTime) {
        if let Some(ref mut e) = self.entry {
            e.set_created(date_time);
        }
    }

    /// Sets date of last access for this file.
    ///
    /// Note: it is overwritten by a value from the `TimeProvider` on every file read operation.
    /// Deprecated: if needed implement a custom `TimeProvider`.
    #[deprecated]
    pub fn set_accessed(&mut self, date: Date) {
        if let Some(ref mut e) = self.entry {
            e.set_accessed(date);
        }
    }

    /// Sets date and time of last modification for this file.
    ///
    /// Note: it is overwritten by a value from the `TimeProvider` on every file write operation.
    /// Deprecated: if needed implement a custom `TimeProvider`.
    #[deprecated]
    pub fn set_modified(&mut self, date_time: DateTime) {
        if let Some(ref mut e) = self.entry {
            e.set_modified(date_time);
        }
    }

    /// Get the current length of this file.
    pub fn len(&self) -> u32 {
        match self.entry {
            Some(ref e) => e.inner().size().unwrap_or(0),
            None => 0,
        }
    }

    /// Get the access time of this file.
    pub fn accessed(&self) -> Date {
        match self.entry {
            Some(ref e) => e.inner().accessed(),
            None => Date::decode(0),
        }
    }

    /// Get the creation time of this file.
    pub fn created(&self) -> DateTime {
        match self.entry {
            Some(ref e) => e.inner().created(),
            None => DateTime::decode(0, 0, 0),
        }
    }

    /// Get the modification time of this file.
    pub fn modified(&self) -> DateTime {
        match self.entry {
            Some(ref e) => e.inner().modified(),
            None => DateTime::decode(0, 0, 0),
        }
    }

    /// Avoid writing any more updates to the file's dirent. This should be called if the file's
    /// dirent is being deleted but the actual file clusters are being left as-is.
    pub(crate) fn mark_deleted(&mut self) {
        if let Some(ref mut e) = self.entry {
            e.mark_deleted();
        }
    }

    /// Avoid writing any more updates to the file's dirent. This should be called if the file's
    /// dirent is being deleted but the actual file clusters are being left as-is.
    pub(crate) fn mark_deleted_and_purged(&mut self) {
        if let Some(ref mut e) = self.entry {
            e.mark_deleted();
            self.first_cluster.take();
        }
    }

    /// True if mark_deleted() has been called on this file.
    pub(crate) fn is_deleted(&self) -> bool {
        if let Some(ref e) = self.entry {
            e.inner().is_deleted()
        } else {
            false
        }
    }

    /// Delete the file if it has been removed from its parent directory using `Dir::remove_dirent`.
    pub fn purge(mut self) -> io::Result<()> {
        let entry = match self.entry {
            Some(ref e) => e,
            None => {
                return Err(io::Error::new(
                    ErrorKind::InvalidInput,
                    "Can't delete file with no dirent",
                ))
            }
        };

        if !entry.inner().is_deleted() {
            return Err(io::Error::new(ErrorKind::InvalidInput, "Must call remove_dirent() first"));
        }

        if let Some(cluster) = self.first_cluster.take() {
            self.fs.free_cluster_chain(cluster)?;
        }

        Ok(())
    }

    pub(crate) fn editor_mut(&mut self) -> Option<&mut DirEntryEditor> {
        self.entry.as_mut()
    }

    pub(crate) fn editor(&self) -> Option<&DirEntryEditor> {
        self.entry.as_ref()
    }

    fn size(&self) -> Option<u32> {
        match self.entry {
            Some(ref e) => e.inner().size(),
            None => None,
        }
    }

    fn is_dir(&self) -> bool {
        match self.entry {
            Some(ref e) => e.inner().is_dir(),
            None => true, // root directory
        }
    }

    fn bytes_left_in_file(&self) -> Option<usize> {
        // Note: seeking beyond end of file is not allowed so overflow is impossible
        self.size().map(|s| (s - self.offset) as usize)
    }

    fn set_first_cluster(&mut self, cluster: u32) {
        self.first_cluster = Some(cluster);
        if let Some(ref mut e) = self.entry {
            e.set_first_cluster(self.first_cluster, self.fs.fat_type());
        }
    }

    pub(crate) fn first_cluster(&self) -> Option<u32> {
        self.first_cluster
    }

    fn flush(&mut self) -> io::Result<()> {
        self.flush_dir_entry()?;
        let mut disk = self.fs.disk.borrow_mut();
        disk.flush()
    }
}

impl<IO: ReadWriteSeek, TP: TimeProvider, OCC> File<'_, IO, TP, OCC> {
    fn update_dir_entry_after_write(&mut self) {
        let offset = self.offset;
        if let Some(ref mut e) = self.entry {
            let now = self.fs.options.time_provider.get_current_date_time();
            e.set_modified(now);
            if e.inner().size().map_or(false, |s| offset > s) {
                e.set_size(offset);
            }
        }
    }
}

impl<IO: ReadWriteSeek, TP, OCC> Drop for File<'_, IO, TP, OCC> {
    fn drop(&mut self) {
        if let Err(err) = self.flush() {
            error!("flush failed {}", err);
        }

        // If we've been deleted, then mark clusters as free.
        if let Some(ref entry) = self.entry {
            if entry.inner().is_deleted() {
                if let Some(cluster) = self.first_cluster.take() {
                    let _ = self
                        .fs
                        .free_cluster_chain(cluster)
                        .map_err(|e| error!("free_cluster_chain failed {}", e));
                }
            }
        }
    }
}

impl<IO: ReadWriteSeek, TP: TimeProvider, OCC> Read for File<'_, IO, TP, OCC> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let cluster_size = self.fs.cluster_size();
        let current_cluster_opt = if self.offset % cluster_size == 0 {
            // next cluster
            match self.current_cluster {
                None => self.first_cluster,
                Some(n) => {
                    let r = self.fs.cluster_iter(n).next();
                    match r {
                        Some(Err(err)) => return Err(err),
                        Some(Ok(n)) => Some(n),
                        None => None,
                    }
                }
            }
        } else {
            self.current_cluster
        };
        let current_cluster = match current_cluster_opt {
            Some(n) => n,
            None => return Ok(0),
        };
        let offset_in_cluster = self.offset % cluster_size;
        let bytes_left_in_cluster = (cluster_size - offset_in_cluster) as usize;
        let bytes_left_in_file = self.bytes_left_in_file().unwrap_or(bytes_left_in_cluster);
        let read_size = cmp::min(cmp::min(buf.len(), bytes_left_in_cluster), bytes_left_in_file);
        if read_size == 0 {
            return Ok(0);
        }
        trace!("read {} bytes in cluster {}", read_size, current_cluster);
        let offset_in_fs =
            self.fs.offset_from_cluster(current_cluster)? + (offset_in_cluster as u64);
        let read_bytes = {
            let mut disk = self.fs.disk.borrow_mut();
            disk.seek(SeekFrom::Start(offset_in_fs))?;
            disk.read(&mut buf[..read_size])?
        };
        if read_bytes == 0 {
            return Ok(0);
        }
        self.offset += read_bytes as u32;
        self.current_cluster = Some(current_cluster);

        if let Some(ref mut e) = self.entry {
            if self.fs.options.update_accessed_date {
                let now = self.fs.options.time_provider.get_current_date();
                e.set_accessed(now);
            }
        }
        Ok(read_bytes)
    }
}

impl<IO: ReadWriteSeek, TP: TimeProvider, OCC> Write for File<'_, IO, TP, OCC> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let cluster_size = self.fs.cluster_size();
        let offset_in_cluster = self.offset % cluster_size;
        let bytes_left_in_cluster = (cluster_size - offset_in_cluster) as usize;
        let bytes_left_until_max_file_size = (MAX_FILE_SIZE - self.offset) as usize;
        let write_size = cmp::min(buf.len(), bytes_left_in_cluster);
        let write_size = cmp::min(write_size, bytes_left_until_max_file_size);
        // Exit early if we are going to write no data
        if write_size == 0 {
            return Ok(0);
        }
        // Mark the volume 'dirty'
        self.fs.set_dirty_flag(true)?;
        // Get cluster for write possibly allocating new one
        let current_cluster = if self.offset % cluster_size == 0 {
            // next cluster
            let next_cluster = match self.current_cluster {
                None => self.first_cluster,
                Some(n) => {
                    let r = self.fs.cluster_iter(n).next();
                    match r {
                        Some(Err(err)) => return Err(err),
                        Some(Ok(n)) => Some(n),
                        None => None,
                    }
                }
            };
            match next_cluster {
                Some(n) => n,
                None => {
                    // end of chain reached - allocate new cluster
                    let new_cluster = self.fs.alloc_cluster(self.current_cluster, self.is_dir())?;
                    trace!("allocated cluser {}", new_cluster);
                    if self.first_cluster.is_none() {
                        self.set_first_cluster(new_cluster);
                    }
                    new_cluster
                }
            }
        } else {
            // self.current_cluster should be a valid cluster
            match self.current_cluster {
                Some(n) => n,
                None => panic!("Offset inside cluster but no cluster allocated"),
            }
        };
        trace!("write {} bytes in cluster {}", write_size, current_cluster);
        let offset_in_fs =
            self.fs.offset_from_cluster(current_cluster)? + (offset_in_cluster as u64);
        let written_bytes = {
            let mut disk = self.fs.disk.borrow_mut();
            disk.seek(SeekFrom::Start(offset_in_fs))?;
            disk.write(&buf[..write_size])?
        };
        if written_bytes == 0 {
            return Ok(0);
        }
        // some bytes were written - update position and optionally size
        self.offset += written_bytes as u32;
        self.current_cluster = Some(current_cluster);
        self.update_dir_entry_after_write();
        Ok(written_bytes)
    }

    fn flush(&mut self) -> io::Result<()> {
        Self::flush(self)
    }
}

impl<IO: ReadWriteSeek, TP, OCC> Seek for File<'_, IO, TP, OCC> {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let mut new_pos = match pos {
            SeekFrom::Current(x) => self.offset as i64 + x,
            SeekFrom::Start(x) => x as i64,
            SeekFrom::End(x) => {
                let size = self.size().expect("cannot seek from end if size is unknown") as i64;
                size + x
            }
        };
        if new_pos < 0 {
            return Err(io::Error::new(ErrorKind::InvalidInput, "Seek to a negative offset"));
        }
        if let Some(s) = self.size() {
            if new_pos > s as i64 {
                trace!("seek beyond end of file");
                new_pos = s as i64;
            }
        }
        let mut new_pos = new_pos as u32;
        trace!("file seek {} -> {} - entry {:?}", self.offset, new_pos, self.entry);
        if new_pos == self.offset {
            // position is the same - nothing to do
            return Ok(self.offset as u64);
        }
        // get number of clusters to seek (favoring previous cluster in corner case)
        let cluster_count = (self.fs.clusters_from_bytes(new_pos as u64) as i32 - 1) as isize;
        let old_cluster_count =
            (self.fs.clusters_from_bytes(self.offset as u64) as i32 - 1) as isize;
        let new_cluster = if new_pos == 0 {
            None
        } else if cluster_count == old_cluster_count {
            self.current_cluster
        } else {
            match self.first_cluster {
                Some(n) => {
                    let mut cluster = n;
                    let mut iter = self.fs.cluster_iter(n);
                    for i in 0..cluster_count {
                        cluster = match iter.next() {
                            Some(r) => r?,
                            None => {
                                // chain ends before new position - seek to end of last cluster
                                new_pos = self.fs.bytes_from_clusters((i + 1) as u32)? as u32;
                                break;
                            }
                        };
                    }
                    Some(cluster)
                }
                None => {
                    // empty file - always seek to 0
                    new_pos = 0;
                    None
                }
            }
        };
        self.offset = new_pos as u32;
        self.current_cluster = new_cluster;
        Ok(self.offset as u64)
    }
}
