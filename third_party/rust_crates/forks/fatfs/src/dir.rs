use crate::core::{char, cmp, num, str};
#[cfg(feature = "lfn")]
use crate::core::{iter, slice};
use crate::io;
use crate::io::prelude::*;
use crate::io::{ErrorKind, SeekFrom};
#[cfg(all(not(feature = "std"), feature = "alloc", feature = "lfn"))]
use alloc::vec::Vec;

use crate::dir_entry::{
    DirEntry, DirEntryData, DirEntryEditor, DirFileEntryData, DirLfnEntryData, FileAttributes,
    ShortName, DIR_ENTRY_SIZE,
};
#[cfg(feature = "lfn")]
use crate::dir_entry::{LFN_ENTRY_LAST_FLAG, LFN_PART_LEN};
use crate::dir_entry::{SFN_PADDING, SFN_SIZE};
use crate::error::FatfsError;
use crate::file::File;
use crate::fs::{DiskSlice, FileSystem, FsIoAdapter, OemCpConverter, ReadWriteSeek};
use crate::time::{Date, DateTime, TimeProvider};
use std::cell::{RefCell, RefMut};

pub(crate) enum DirRawStream<'fs, IO: ReadWriteSeek, TP, OCC> {
    File(Option<File<'fs, IO, TP, OCC>>),
    Root(DiskSlice<FsIoAdapter<'fs, IO, TP, OCC>>),
}

impl<IO: ReadWriteSeek, TP, OCC> DirRawStream<'_, IO, TP, OCC> {
    fn abs_pos(&self) -> Result<Option<u64>, FatfsError> {
        match self {
            DirRawStream::File(file) => {
                if let Some(ref file) = file {
                    Ok(file.abs_pos()?)
                } else {
                    Ok(None)
                }
            }
            DirRawStream::Root(slice) => Ok(Some(slice.abs_pos())),
        }
    }

    fn first_cluster(&self) -> Option<u32> {
        match self {
            DirRawStream::File(file) => file.as_ref().and_then(|f| f.first_cluster()),
            DirRawStream::Root(_) => None,
        }
    }

    fn entry_mut(&mut self) -> Option<&mut DirEntryEditor> {
        match self {
            DirRawStream::File(file) => file.as_mut().and_then(|f| f.editor_mut()),
            DirRawStream::Root(_) => None,
        }
    }

    fn entry(&self) -> Option<&DirEntryEditor> {
        match self {
            DirRawStream::File(file) => file.as_ref().and_then(|f| f.editor()),
            DirRawStream::Root(_) => None,
        }
    }

    fn is_deleted(&self) -> bool {
        match self {
            DirRawStream::File(file) => file.as_ref().map_or(true, |f| f.is_deleted()),
            DirRawStream::Root(_) => false,
        }
    }
}

impl<IO: ReadWriteSeek, TP: TimeProvider, OCC> Read for DirRawStream<'_, IO, TP, OCC> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            DirRawStream::File(Some(file)) => file.read(buf),
            DirRawStream::File(None) => {
                Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"))
            }
            DirRawStream::Root(raw) => raw.read(buf),
        }
    }
}

impl<IO: ReadWriteSeek, TP: TimeProvider, OCC> Write for DirRawStream<'_, IO, TP, OCC> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            DirRawStream::File(Some(file)) => file.write(buf),
            DirRawStream::File(None) => {
                Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"))
            }
            DirRawStream::Root(raw) => raw.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            DirRawStream::File(Some(file)) => file.flush(),
            DirRawStream::File(None) => {
                Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"))
            }
            DirRawStream::Root(raw) => raw.flush(),
        }
    }
}

impl<IO: ReadWriteSeek, TP, OCC> Seek for DirRawStream<'_, IO, TP, OCC> {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        match self {
            DirRawStream::File(Some(file)) => file.seek(pos),
            DirRawStream::File(None) => {
                Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"))
            }
            DirRawStream::Root(raw) => raw.seek(pos),
        }
    }
}

/// Ensures the filename has no trailing spaces or dots.
pub fn validate_filename<'a>(name: &'a str) -> io::Result<()> {
    if name == "." || name == ".." {
        return Ok(());
    }

    // Disallow trailing dots or spaces.
    // Trailing dots are ignored by Windows and Linux, and trailing spaces are ignored by Windows.
    // We error so that opening a file with a given name will always mean that a file with that
    // name exists.
    // We also disallow empty filenames.
    if name.ends_with(&['.', ' '][..]) || name.is_empty() {
        return Err(io::Error::new(ErrorKind::InvalidInput, "Illegal filename"));
    }
    Ok(())
}

fn split_path<'a>(path: &'a str) -> io::Result<(&'a str, Option<&'a str>)> {
    // remove trailing slash and split into 2 components - top-most parent and rest
    let mut path_split = path.trim_matches('/').splitn(2, '/');
    let comp = path_split.next().unwrap(); // SAFE: splitn always returns at least one element
    validate_filename(comp)?;
    let rest_opt = path_split.next();
    Ok((comp, rest_opt))
}

enum DirEntryOrShortName<'fs, IO: ReadWriteSeek, TP, OCC> {
    DirEntry(DirEntry<'fs, IO, TP, OCC>),
    ShortName([u8; SFN_SIZE]),
}

/// A FAT filesystem directory.
///
/// This struct is created by the `open_dir` or `create_dir` methods on `Dir`.
/// The root directory is returned by the `root_dir` method on `FileSystem`.
pub struct Dir<'fs, IO: ReadWriteSeek, TP, OCC> {
    stream: RefCell<DirRawStream<'fs, IO, TP, OCC>>,
    fs: &'fs FileSystem<IO, TP, OCC>,
    is_root: bool,
}

impl<'fs, IO: ReadWriteSeek, TP, OCC> Dir<'fs, IO, TP, OCC> {
    pub(crate) fn new(
        stream: DirRawStream<'fs, IO, TP, OCC>,
        fs: &'fs FileSystem<IO, TP, OCC>,
        is_root: bool,
    ) -> Self {
        Dir { stream: RefCell::new(stream), fs, is_root }
    }

    /// Creates directory entries iterator.
    pub fn iter<'a>(&'a self) -> DirIter<'a, 'fs, IO, TP, OCC> {
        DirIter::new(&self.stream, self.fs, true)
    }
}

impl<'fs, IO: ReadWriteSeek, TP: TimeProvider, OCC: OemCpConverter> Dir<'fs, IO, TP, OCC> {
    fn find_entry(
        &self,
        name: &str,
        is_dir: Option<bool>,
        mut short_name_gen: Option<&mut ShortNameGenerator>,
    ) -> io::Result<DirEntry<'fs, IO, TP, OCC>> {
        for r in self.iter() {
            let e = r?;
            // compare name ignoring case
            if e.eq_name(name) {
                // check if file or directory is expected
                if is_dir.is_some() && Some(e.is_dir()) != is_dir {
                    let error =
                        if e.is_dir() { FatfsError::IsDirectory } else { FatfsError::NotDirectory };
                    return Err(io::Error::new(ErrorKind::Other, error));
                }
                return Ok(e);
            }
            // update short name generator state
            if let Some(ref mut gen) = short_name_gen {
                gen.add_existing(e.raw_short_name());
            }
        }
        Err(io::Error::new(ErrorKind::NotFound, "No such file or directory"))
    }

    pub(crate) fn find_volume_entry(&self) -> io::Result<Option<DirEntry<'fs, IO, TP, OCC>>> {
        for r in DirIter::new(&self.stream, self.fs, false) {
            let e = r?;
            if e.data.is_volume() {
                return Ok(Some(e));
            }
        }
        Ok(None)
    }

    fn check_for_existence(
        &self,
        name: &str,
        is_dir: Option<bool>,
    ) -> io::Result<DirEntryOrShortName<'fs, IO, TP, OCC>> {
        let mut short_name_gen = ShortNameGenerator::new(name);
        loop {
            let r = self.find_entry(name, is_dir, Some(&mut short_name_gen));
            match r {
                Err(ref err) if err.kind() == ErrorKind::NotFound => {}
                // other error
                Err(err) => return Err(err),
                // directory already exists - return it
                Ok(e) => return Ok(DirEntryOrShortName::DirEntry(e)),
            };
            if let Ok(name) = short_name_gen.generate() {
                return Ok(DirEntryOrShortName::ShortName(name));
            }
            short_name_gen.next_iteration();
        }
    }

    /// Set modification datetime for this directory.
    pub fn set_created(&mut self, date_time: DateTime) {
        match self.stream.borrow_mut().entry_mut() {
            Some(e) => e.set_created(date_time),
            None => {}
        }
    }

    /// Set access date for this directory.
    pub fn set_accessed(&mut self, date: Date) {
        match self.stream.borrow_mut().entry_mut() {
            Some(e) => e.set_accessed(date),
            None => {}
        }
    }

    /// Set modification datetime for this directory.
    pub fn set_modified(&mut self, date_time: DateTime) {
        match self.stream.borrow_mut().entry_mut() {
            Some(e) => e.set_modified(date_time),
            None => {}
        }
    }

    /// Get the access time of this directory.
    pub fn accessed(&self) -> Date {
        match self.stream.borrow().entry() {
            Some(ref e) => e.inner().accessed(),
            None => Date::decode(0),
        }
    }

    /// Get the creation time of this directory.
    pub fn created(&self) -> DateTime {
        match self.stream.borrow().entry() {
            Some(ref e) => e.inner().created(),
            None => DateTime::decode(0, 0, 0),
        }
    }

    /// Get the modification time of this directory.
    pub fn modified(&self) -> DateTime {
        match self.stream.borrow().entry() {
            Some(ref e) => e.inner().modified(),
            None => DateTime::decode(0, 0, 0),
        }
    }

    /// Opens existing subdirectory.
    ///
    /// `path` is a '/' separated directory path relative to self directory.
    pub fn open_dir(&self, path: &str) -> io::Result<Self> {
        trace!("open_dir {}", path);
        let (name, rest_opt) = split_path(path)?;
        let e = self.find_entry(name, Some(true), None)?;
        match rest_opt {
            Some(rest) => e.to_dir().open_dir(rest),
            None => Ok(e.to_dir()),
        }
    }

    /// Opens existing file.
    ///
    /// `path` is a '/' separated file path relative to self directory.
    pub fn open_file(&self, path: &str) -> io::Result<File<'fs, IO, TP, OCC>> {
        trace!("open_file {}", path);
        // traverse path
        let (name, rest_opt) = split_path(path)?;
        if let Some(rest) = rest_opt {
            let e = self.find_entry(name, Some(true), None)?;
            return e.to_dir().open_file(rest);
        }
        // convert entry to a file
        let e = self.find_entry(name, Some(false), None)?;
        Ok(e.to_file())
    }

    /// Creates new or opens existing file=.
    ///
    /// `path` is a '/' separated file path relative to self directory.
    /// File is never truncated when opening. It can be achieved by calling `File::truncate` method after opening.
    pub fn create_file(&self, path: &str) -> io::Result<File<'fs, IO, TP, OCC>> {
        trace!("create_file {}", path);
        // traverse path
        let (name, rest_opt) = split_path(path)?;
        if let Some(rest) = rest_opt {
            return self.find_entry(name, Some(true), None)?.to_dir().create_file(rest);
        }
        if self.stream.borrow().is_deleted() {
            return Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"));
        }
        // this is final filename in the path
        let r = self.check_for_existence(name, Some(false))?;
        match r {
            // file does not exist - create it
            DirEntryOrShortName::ShortName(short_name) => {
                let sfn_entry =
                    self.create_sfn_entry(short_name, FileAttributes::from_bits_truncate(0), None);
                Ok(self.write_entry(name, sfn_entry)?.to_file())
            }
            // file already exists - return it
            DirEntryOrShortName::DirEntry(e) => Ok(e.to_file()),
        }
    }

    /// Creates new directory or opens existing.
    ///
    /// `path` is a '/' separated path relative to self directory.
    pub fn create_dir(&self, path: &str) -> io::Result<Self> {
        trace!("create_dir {}", path);
        // traverse path
        let (name, rest_opt) = split_path(path)?;
        if let Some(rest) = rest_opt {
            return self.find_entry(name, Some(true), None)?.to_dir().create_dir(rest);
        }
        if self.stream.borrow().is_deleted() {
            return Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"));
        }
        // this is final filename in the path
        let r = self.check_for_existence(name, Some(true))?;
        match r {
            // directory does not exist - create it
            DirEntryOrShortName::ShortName(short_name) => {
                let transaction = self.fs.begin_transaction().unwrap();
                // alloc cluster for directory data
                let cluster = self.fs.alloc_cluster(None, true)?;
                // create entry in parent directory
                let sfn_entry =
                    self.create_sfn_entry(short_name, FileAttributes::DIRECTORY, Some(cluster));
                let entry = self.write_entry(name, sfn_entry)?;
                let dir = entry.to_dir();
                // create special entries "." and ".."
                let dot_sfn = ShortNameGenerator::generate_dot();
                let sfn_entry = self.create_sfn_entry(
                    dot_sfn,
                    FileAttributes::DIRECTORY,
                    entry.first_cluster(),
                );
                dir.write_entry(".", sfn_entry)?;
                let dotdot_sfn = ShortNameGenerator::generate_dotdot();
                let sfn_entry = self.create_sfn_entry(
                    dotdot_sfn,
                    FileAttributes::DIRECTORY,
                    self.cluster_for_child_dotdot_entry(),
                );
                dir.write_entry("..", sfn_entry)?;
                self.fs.commit(transaction)?;
                Ok(dir)
            }
            // directory already exists - return it
            DirEntryOrShortName::DirEntry(e) => Ok(e.to_dir()),
        }
    }

    pub fn is_empty(&self) -> io::Result<bool> {
        trace!("is_empty");
        // check if directory contains no files
        for r in self.iter() {
            let e = r?;
            let name = e.short_file_name_as_bytes();
            // ignore special entries "." and ".."
            if name != b"." && name != b".." {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Removes existing file or directory.
    ///
    /// `path` is a '/' separated file path relative to self directory.
    /// Make sure there is no reference to this file (no File instance) or filesystem corruption
    /// can happen.
    pub fn remove(&self, path: &str) -> io::Result<()> {
        trace!("remove {}", path);
        // traverse path
        let (name, rest_opt) = split_path(path)?;
        if let Some(rest) = rest_opt {
            let e = self.find_entry(name, Some(true), None)?;
            return e.to_dir().remove(rest);
        }
        // in case of directory check if it is empty
        let e = self.find_entry(name, None, None)?;
        if e.is_dir() && !e.to_dir().is_empty()? {
            return Err(io::Error::new(ErrorKind::Other, FatfsError::DirectoryNotEmpty));
        }
        // free data
        if let Some(n) = e.first_cluster() {
            self.fs.free_cluster_chain(n)?;
        }
        // free long and short name entries
        let mut stream = self.stream.borrow_mut();
        stream.seek(SeekFrom::Start(e.offset_range.0 as u64))?;
        let num = (e.offset_range.1 - e.offset_range.0) as usize / DIR_ENTRY_SIZE as usize;
        for _ in 0..num {
            let mut data = DirEntryData::deserialize(&mut *stream)?;
            trace!("removing dir entry {:?}", data);
            data.set_deleted();
            stream.seek(SeekFrom::Current(-(DIR_ENTRY_SIZE as i64)))?;
            data.serialize(&mut *stream)?;
        }
        Ok(())
    }

    fn unlink_file_on_disk(&self, offset_range: (u64, u64)) -> io::Result<()> {
        let mut stream = self.stream.borrow_mut();
        stream.seek(io::SeekFrom::Start(offset_range.0))?;
        let num = (offset_range.1 - offset_range.0) as usize / DIR_ENTRY_SIZE as usize;
        for _ in 0..num {
            let mut data = DirEntryData::deserialize(&mut *stream)?;
            trace!("removing dir entry {:?}", data);
            data.set_deleted();
            stream.seek(SeekFrom::Current(-(DIR_ENTRY_SIZE as i64)))?;
            data.serialize(&mut *stream)?;
        }
        Ok(())
    }

    /// Unlink the given File from this directory.
    ///
    /// Will cause filesystem corruption if the file is not actually in this directory,
    /// but continuing to use the file is legal. It will be completely removed from the filesystem
    /// once it is dropped.
    pub fn unlink_file(&self, file: &mut File<IO, TP, OCC>) -> io::Result<()> {
        let entry = file
            .editor()
            .ok_or(io::Error::new(ErrorKind::InvalidInput, "Can't delete file with no dirent"))?;
        self.unlink_file_on_disk(entry.offset_range)?;
        file.mark_deleted();
        Ok(())
    }

    /// Unlink the given Dir from this directory. Returns an error if the Dir is not empty.
    ///
    /// Will cause filesystem corruption if the file is not actually in this directory.
    /// The directory's clusters will be removed from the disk.
    pub fn unlink_dir(&self, dir: &mut Dir<IO, TP, OCC>) -> io::Result<()> {
        if !dir.is_empty()? {
            return Err(io::Error::new(ErrorKind::Other, FatfsError::DirectoryNotEmpty));
        }
        match &mut *dir.stream.borrow_mut() {
            DirRawStream::File(ref mut option) => {
                if let Some(mut file) = option.take() {
                    self.unlink_file(&mut file)?;
                    file.purge()
                } else {
                    // Directory has already been deleted.
                    return Err(io::Error::new(ErrorKind::NotFound, "Not found"));
                }
            }
            DirRawStream::Root(_) => Err(io::Error::new(
                ErrorKind::InvalidInput,
                "Root directory is not child of any directory",
            )),
        }
    }

    /// Renames or moves existing file or directory.
    ///
    /// `src_path` is a '/' separated source file path relative to self directory.
    /// `dst_path` is a '/' separated destination file path relative to `dst_dir`.
    /// `dst_dir` can be set to self directory if rename operation without moving is needed.
    /// Make sure there is no reference to this file (no File instance) or filesystem corruption
    /// can happen.
    pub fn rename(
        &self,
        src_path: &str,
        dst_dir: &Dir<IO, TP, OCC>,
        dst_path: &str,
    ) -> io::Result<()> {
        trace!("rename {} {}", src_path, dst_path);
        // traverse source path
        let (name, rest_opt) = split_path(src_path)?;
        if let Some(rest) = rest_opt {
            let e = self.find_entry(name, Some(true), None)?;
            return e.to_dir().rename(rest, dst_dir, dst_path);
        }
        // traverse destination path
        let (name, rest_opt) = split_path(dst_path)?;
        if let Some(rest) = rest_opt {
            let e = dst_dir.find_entry(name, Some(true), None)?;
            return self.rename(src_path, &e.to_dir(), rest);
        }

        // move/rename file
        self.rename_internal(src_path, dst_dir, dst_path)
    }

    /// Same as rename, but supports renaming over an existing file.
    pub fn rename_over_file(
        &self,
        src_path: &str,
        dst_dir: &Dir<IO, TP, OCC>,
        dst_path: &str,
        file: &mut File<IO, TP, OCC>,
    ) -> io::Result<()> {
        let transaction = self.fs.begin_transaction().unwrap();
        let entry = file
            .editor()
            .ok_or(io::Error::new(ErrorKind::InvalidInput, "Can't delete file with no dirent"))?;
        dst_dir.unlink_file_on_disk(entry.offset_range)?;
        self.rename(src_path, dst_dir, dst_path)?;
        self.fs.commit(transaction)?;
        file.mark_deleted();
        Ok(())
    }

    /// Same as rename but supports renaming over an existing directory (which must be empty).
    pub fn rename_over_dir(
        &self,
        src_path: &str,
        dst_dir: &Dir<IO, TP, OCC>,
        dst_path: &str,
        dir: &mut Dir<IO, TP, OCC>,
    ) -> io::Result<()> {
        let transaction = self.fs.begin_transaction().unwrap();
        if !dir.is_empty()? {
            return Err(io::Error::new(ErrorKind::Other, FatfsError::DirectoryNotEmpty));
        }
        let file;
        let mut stream = dir.stream.borrow_mut();
        match &mut *stream {
            DirRawStream::File(ref mut option) => {
                file = option.as_mut().ok_or(io::Error::new(ErrorKind::NotFound, "Not found"))?;
                let entry = file.editor().ok_or(io::Error::new(
                    ErrorKind::InvalidInput,
                    "Can't delete file with no dirent",
                ))?;
                dst_dir.unlink_file_on_disk(entry.offset_range)?;
                if let Some(cluster) = file.first_cluster() {
                    self.fs.free_cluster_chain(cluster)?;
                }
            }
            DirRawStream::Root(_) => {
                return Err(io::Error::new(
                    ErrorKind::InvalidInput,
                    "Root directory is not child of any directory",
                ))
            }
        };
        self.rename(src_path, dst_dir, dst_path)?;
        self.fs.commit(transaction)?;
        file.mark_deleted_and_purged();
        Ok(())
    }

    fn rename_internal(
        &self,
        src_name: &str,
        dst_dir: &Dir<IO, TP, OCC>,
        dst_name: &str,
    ) -> io::Result<()> {
        trace!("rename_internal {} {}", src_name, dst_name);
        if dst_dir.stream.borrow().is_deleted() {
            return Err(io::Error::new(
                ErrorKind::NotFound,
                "Destination directory has been deleted",
            ));
        }
        // find existing file
        let e = self.find_entry(src_name, None, None)?;
        // check if destionation filename is unused
        let r = dst_dir.check_for_existence(dst_name, None)?;
        let short_name = match r {
            DirEntryOrShortName::DirEntry(ref dst_e) => {
                // check if source and destination entry is the same
                if !e.is_same_entry(dst_e) {
                    return Err(io::Error::new(
                        ErrorKind::AlreadyExists,
                        "Destination file already exists",
                    ));
                }
                // if names are exactly the same, we don't need to do anything.
                if src_name == dst_name {
                    return Ok(());
                }
                // otherwise, we'll rewrite the LFN below.
                dst_e.raw_short_name().clone()
            }
            DirEntryOrShortName::ShortName(short_name) => short_name,
        };
        // free long and short name entries
        {
            let mut stream = self.stream.borrow_mut();
            stream.seek(SeekFrom::Start(e.offset_range.0 as u64))?;
            let num = (e.offset_range.1 - e.offset_range.0) as usize / DIR_ENTRY_SIZE as usize;
            for _ in 0..num {
                let mut data = DirEntryData::deserialize(&mut *stream)?;
                trace!("removing LFN entry {:?}", data);
                data.set_deleted();
                stream.seek(SeekFrom::Current(-(DIR_ENTRY_SIZE as i64)))?;
                data.serialize(&mut *stream)?;
            }
        }
        // save new directory entry
        let sfn_entry = e.data.renamed(short_name);
        let dir_entry = dst_dir.write_entry(dst_name, sfn_entry)?;
        // if renaming a directory, then we need to update the '..' entry
        if dir_entry.is_dir()
            && self.cluster_for_child_dotdot_entry() != dst_dir.cluster_for_child_dotdot_entry()
        {
            let dir = dir_entry.to_dir();
            let mut iter = dir.iter();
            let error = || io::Error::new(ErrorKind::Other, "Missing expected .. entry");
            // The dotdot entry should be the second entry.
            let _ = iter.next().ok_or_else(error)??;
            let entry = iter.next().ok_or_else(error)??;
            if entry.short_file_name_as_bytes() != b".." {
                return Err(error());
            }
            let mut editor = entry.editor();
            editor.set_first_cluster(dst_dir.cluster_for_child_dotdot_entry(), self.fs.fat_type());
            editor.flush(self.fs)?;
        }
        Ok(())
    }

    fn find_free_entries(
        &self,
        num_entries: usize,
    ) -> io::Result<RefMut<'_, DirRawStream<'fs, IO, TP, OCC>>> {
        let mut stream = self.stream.borrow_mut();
        stream.seek(io::SeekFrom::Start(0))?;
        let mut first_free = 0;
        let mut num_free = 0;
        let mut i = 0;
        loop {
            let raw_entry = DirEntryData::deserialize(&mut *stream)?;
            if raw_entry.is_end() {
                // first unused entry - all remaining space can be used
                if num_free == 0 {
                    first_free = i;
                }
                stream.seek(io::SeekFrom::Start(first_free as u64 * DIR_ENTRY_SIZE))?;
                return Ok(stream);
            } else if raw_entry.is_deleted() {
                // free entry - calculate number of free entries in a row
                if num_free == 0 {
                    first_free = i;
                }
                num_free += 1;
                if num_free == num_entries {
                    // enough space for new file
                    stream.seek(io::SeekFrom::Start(first_free as u64 * DIR_ENTRY_SIZE))?;
                    return Ok(stream);
                }
            } else {
                // used entry - start counting from 0
                num_free = 0;
            }
            i += 1;
        }
    }

    fn create_sfn_entry(
        &self,
        short_name: [u8; SFN_SIZE],
        attrs: FileAttributes,
        first_cluster: Option<u32>,
    ) -> DirFileEntryData {
        let mut raw_entry = DirFileEntryData::new(short_name, attrs);
        raw_entry.set_first_cluster(first_cluster, self.fs.fat_type());
        let now = self.fs.options.time_provider.get_current_date_time();
        raw_entry.set_created(now);
        raw_entry.set_accessed(now.date);
        raw_entry.set_modified(now);
        raw_entry
    }

    #[cfg(feature = "lfn")]
    fn encode_lfn_utf16(name: &str) -> LfnBuffer {
        LfnBuffer::from_ucs2_units(name.encode_utf16())
    }
    #[cfg(not(feature = "lfn"))]
    fn encode_lfn_utf16(_name: &str) -> LfnBuffer {
        LfnBuffer {}
    }

    fn alloc_and_write_lfn_entries(
        &self,
        lfn_utf16: &LfnBuffer,
        short_name: &[u8; SFN_SIZE],
    ) -> io::Result<(RefMut<'_, DirRawStream<'fs, IO, TP, OCC>>, u64)> {
        // get short name checksum
        let lfn_chsum = lfn_checksum(short_name);
        // create LFN entries generator
        let lfn_iter = LfnEntriesGenerator::new(lfn_utf16.as_ucs2_units(), lfn_chsum);
        // find space for new entries (multiple LFN entries and 1 SFN entry)
        let num_entries = lfn_iter.len() + 1;
        let mut stream = self.find_free_entries(num_entries)?;
        let start_pos = stream.seek(io::SeekFrom::Current(0))?;
        // write LFN entries before SFN entry
        for lfn_entry in lfn_iter {
            lfn_entry.serialize(&mut *stream)?;
        }
        Ok((stream, start_pos))
    }

    fn write_entry(
        &self,
        name: &str,
        raw_entry: DirFileEntryData,
    ) -> io::Result<DirEntry<'fs, IO, TP, OCC>> {
        trace!("write_entry {}", name);
        // check if name doesn't contain unsupported characters
        let long_name_required = validate_long_name(name, raw_entry.name())?;
        // start a transaction so that if things go wrong (e.g. we run out of space), we can easily
        // revert and leave the file system in a good state.
        let transaction = self.fs.begin_transaction();
        let (mut stream, start_pos, lfn_utf16) = if long_name_required {
            // convert long name to UTF-16
            let lfn_utf16 = Self::encode_lfn_utf16(name);
            // write LFN entries
            let (stream, start_pos) =
                self.alloc_and_write_lfn_entries(&lfn_utf16, raw_entry.name())?;
            (stream, start_pos, lfn_utf16)
        } else {
            let mut stream = self.find_free_entries(1)?;
            let start_pos = stream.seek(io::SeekFrom::Current(0))?;
            (stream, start_pos, LfnBuffer::new())
        };
        // write short name entry
        raw_entry.serialize(&mut *stream)?;
        if let Some(transaction) = transaction {
            self.fs.commit(transaction)?;
        }
        let end_pos = stream.seek(io::SeekFrom::Current(0))?;
        let abs_pos = stream.abs_pos()?.map(|p| p - DIR_ENTRY_SIZE);
        // return new logical entry descriptor
        let short_name = ShortName::new(raw_entry.name());
        Ok(DirEntry {
            data: raw_entry,
            short_name,
            #[cfg(feature = "lfn")]
            lfn_utf16,
            fs: self.fs,
            entry_pos: abs_pos.unwrap(), // SAFE: abs_pos is absent only for empty file
            offset_range: (start_pos, end_pos),
        })
    }

    fn cluster_for_child_dotdot_entry(&self) -> Option<u32> {
        if self.is_root {
            None
        } else {
            self.stream.borrow().first_cluster()
        }
    }

    /// Flushes the directory entry in the parent if not the root.
    pub fn flush_dir_entry(&mut self) -> std::io::Result<()> {
        match &mut *self.stream.borrow_mut() {
            DirRawStream::File(Some(file)) => file.flush_dir_entry(),
            DirRawStream::File(None) => {
                Err(io::Error::new(ErrorKind::NotFound, "Directory has been deleted"))
            }
            DirRawStream::Root(_) => {
                Err(io::Error::new(ErrorKind::Other, "Cannot flush root directroy entry"))
            }
        }
    }
}

/// An iterator over the directory entries.
///
/// This struct is created by the `iter` method on `Dir`.
pub struct DirIter<'a, 'fs, IO: ReadWriteSeek, TP, OCC> {
    stream: &'a RefCell<DirRawStream<'fs, IO, TP, OCC>>,
    fs: &'fs FileSystem<IO, TP, OCC>,
    skip_volume: bool,
    err: bool,
}

impl<'a, 'fs, IO: ReadWriteSeek, TP, OCC> DirIter<'a, 'fs, IO, TP, OCC> {
    fn new(
        stream: &'a RefCell<DirRawStream<'fs, IO, TP, OCC>>,
        fs: &'fs FileSystem<IO, TP, OCC>,
        skip_volume: bool,
    ) -> Self {
        stream.borrow_mut().seek(SeekFrom::Start(0)).unwrap();
        DirIter { stream, fs, skip_volume, err: false }
    }
}

impl<'fs, IO: ReadWriteSeek, TP: TimeProvider, OCC> DirIter<'_, 'fs, IO, TP, OCC> {
    fn should_skip_entry(&self, raw_entry: &DirEntryData) -> bool {
        if raw_entry.is_deleted() {
            return true;
        }
        match raw_entry {
            DirEntryData::File(sfn_entry) => self.skip_volume && sfn_entry.is_volume(),
            _ => false,
        }
    }

    fn read_dir_entry(&mut self) -> io::Result<Option<DirEntry<'fs, IO, TP, OCC>>> {
        trace!("read_dir_entry");
        let mut lfn_builder = LongNameBuilder::new();
        let mut stream = self.stream.borrow_mut();
        let mut offset = stream.seek(SeekFrom::Current(0))?;
        let mut begin_offset = offset;
        loop {
            let raw_entry = DirEntryData::deserialize(&mut *stream)?;
            offset += DIR_ENTRY_SIZE;
            // Check if this is end of dir
            if raw_entry.is_end() {
                return Ok(None);
            }
            // Check if this is deleted or volume ID entry
            if self.should_skip_entry(&raw_entry) {
                trace!("skip entry");
                lfn_builder.clear();
                begin_offset = offset;
                continue;
            }
            match raw_entry {
                DirEntryData::File(data) => {
                    // Get entry position on volume
                    let abs_pos = stream.abs_pos()?.map(|p| p - DIR_ENTRY_SIZE);
                    // Check if LFN checksum is valid
                    lfn_builder.validate_chksum(data.name());
                    // Return directory entry
                    let short_name = ShortName::new(data.name());
                    trace!("file entry {:?}", data.name());
                    return Ok(Some(DirEntry {
                        data,
                        short_name,
                        #[cfg(feature = "lfn")]
                        lfn_utf16: lfn_builder.into_buf(),
                        fs: self.fs,
                        entry_pos: abs_pos.unwrap(), // SAFE: abs_pos is empty only for empty file
                        offset_range: (begin_offset, offset),
                    }));
                }
                DirEntryData::Lfn(data) => {
                    // Append to LFN buffer
                    trace!("lfn entry");
                    lfn_builder.process(&data);
                }
            }
        }
    }
}

impl<'b, IO: ReadWriteSeek, TP: TimeProvider, OCC> Iterator for DirIter<'_, 'b, IO, TP, OCC> {
    type Item = io::Result<DirEntry<'b, IO, TP, OCC>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.err {
            return None;
        }
        let r = self.read_dir_entry();
        match r {
            Ok(Some(e)) => Some(Ok(e)),
            Ok(None) => None,
            Err(err) => {
                self.err = true;
                Some(Err(err))
            }
        }
    }
}

fn validate_long_name(name: &str, short_name: &[u8; SFN_SIZE]) -> io::Result<bool> {
    // check if length is valid
    if name.is_empty() {
        return Err(io::Error::new(ErrorKind::Other, FatfsError::FileNameEmpty));
    }
    if name.len() > 255 {
        return Err(io::Error::new(ErrorKind::Other, FatfsError::FileNameTooLong));
    }
    let mut long_name_required = name.len() > 12;
    let mut short_name_index: usize = 0;
    // check if there are only valid characters
    for c in name.chars() {
        match c {
            'A'..='Z' | '0'..='9' => {}
            '$' | '%' | '\'' | '-' | '_' | '@' | '~' | '`' | '!' | '(' | ')' | '{' | '}' | '^'
            | '#' | '&' | '.' => {}
            'a'..='z' | '\u{80}'..='\u{10FFFF}' | ' ' | '+' | ',' | ';' | '=' | '[' | ']' => {
                long_name_required = true
            }
            _ => return Err(io::Error::new(ErrorKind::Other, FatfsError::FileNameBadCharacter)),
        }
        if !long_name_required {
            long_name_required = match short_name_index {
                0..=7 => {
                    if c as u8 == short_name[short_name_index] {
                        short_name_index += 1;
                        false
                    } else if c == '.' {
                        short_name_index = 9;
                        false
                    } else {
                        true
                    }
                }
                8 => {
                    short_name_index = 9;
                    c != '.'
                }
                9..=11 => {
                    short_name_index += 1;
                    c as u8 != short_name[short_name_index - 2]
                }
                _ => true,
            };
        }
    }
    Ok(long_name_required)
}

fn lfn_checksum(short_name: &[u8; SFN_SIZE]) -> u8 {
    let mut chksum = num::Wrapping(0u8);
    for b in short_name {
        chksum = (chksum << 7) + (chksum >> 1) + num::Wrapping(*b);
    }
    chksum.0
}

#[cfg(all(feature = "lfn", feature = "alloc"))]
#[derive(Clone)]
pub(crate) struct LfnBuffer {
    ucs2_units: Vec<u16>,
}

#[cfg(not(feature = "alloc"))]
const MAX_LFN_LEN: usize = 256;

#[cfg(all(feature = "lfn", not(feature = "alloc")))]
#[derive(Clone)]
pub(crate) struct LfnBuffer {
    ucs2_units: [u16; MAX_LFN_LEN],
    len: usize,
}

#[cfg(all(feature = "lfn", feature = "alloc"))]
impl LfnBuffer {
    fn new() -> Self {
        LfnBuffer { ucs2_units: Vec::<u16>::new() }
    }

    fn from_ucs2_units<I>(usc2_units: I) -> Self
    where
        I: Iterator<Item = u16>,
    {
        LfnBuffer { ucs2_units: usc2_units.collect() }
    }

    fn clear(&mut self) {
        self.ucs2_units.clear();
    }

    pub(crate) fn len(&self) -> usize {
        self.ucs2_units.len()
    }

    fn set_len(&mut self, len: usize) {
        self.ucs2_units.resize(len, 0u16);
    }

    pub(crate) fn as_ucs2_units(&self) -> &[u16] {
        &self.ucs2_units
    }
}

#[cfg(all(feature = "lfn", not(feature = "alloc")))]
impl LfnBuffer {
    fn new() -> Self {
        LfnBuffer { ucs2_units: [0u16; MAX_LFN_LEN], len: 0 }
    }

    fn from_ucs2_units<I>(usc2_units: I) -> Self
    where
        I: Iterator<Item = u16>,
    {
        let mut lfn = LfnBuffer { ucs2_units: [0u16; MAX_LFN_LEN], len: 0 };
        for (i, usc2_unit) in usc2_units.enumerate() {
            lfn.ucs2_units[i] = usc2_unit;
        }
        lfn
    }

    fn clear(&mut self) {
        self.ucs2_units = [0u16; MAX_LFN_LEN];
        self.len = 0;
    }

    pub(crate) fn len(&self) -> usize {
        self.len
    }

    fn set_len(&mut self, len: usize) {
        self.len = len;
    }

    pub(crate) fn as_ucs2_units(&self) -> &[u16] {
        &self.ucs2_units
    }
}

#[cfg(not(feature = "lfn"))]
#[derive(Clone)]
pub(crate) struct LfnBuffer {}

#[cfg(not(feature = "lfn"))]
impl LfnBuffer {
    fn new() -> Self {
        LfnBuffer {}
    }

    pub(crate) fn as_ucs2_units(&self) -> &[u16] {
        &[]
    }
}

#[cfg(feature = "lfn")]
struct LongNameBuilder {
    buf: LfnBuffer,
    chksum: u8,
    index: u8,
}

#[cfg(feature = "lfn")]
impl LongNameBuilder {
    fn new() -> Self {
        LongNameBuilder { buf: LfnBuffer::new(), chksum: 0, index: 0 }
    }

    fn clear(&mut self) {
        self.buf.clear();
        self.index = 0;
    }

    fn into_buf(mut self) -> LfnBuffer {
        // Check if last processed entry had index 1
        if self.index == 1 {
            self.truncate();
        } else if !self.is_empty() {
            warn!("unfinished LFN sequence {}", self.index);
            self.clear();
        }
        self.buf
    }

    fn truncate(&mut self) {
        // Truncate 0 and 0xFFFF characters from LFN buffer
        let mut lfn_len = self.buf.len();
        while lfn_len > 0 {
            match self.buf.ucs2_units[lfn_len - 1] {
                0xFFFF | 0 => lfn_len -= 1,
                _ => break,
            }
        }
        self.buf.set_len(lfn_len);
    }

    fn is_empty(&self) -> bool {
        // Check if any LFN entry has been processed
        // Note: index 0 is not a valid index in LFN and can be seen only after struct initialization
        self.index == 0
    }

    fn process(&mut self, data: &DirLfnEntryData) {
        let is_last = (data.order() & LFN_ENTRY_LAST_FLAG) != 0;
        let index = data.order() & 0x1F;
        if index == 0 {
            // Corrupted entry
            warn!("corrupted lfn entry! {:x}", data.order());
            self.clear();
            return;
        }
        if is_last {
            // last entry is actually first entry in stream
            self.index = index;
            self.chksum = data.checksum();
            self.buf.set_len(index as usize * LFN_PART_LEN);
        } else if self.index == 0 || index != self.index - 1 || data.checksum() != self.chksum {
            // Corrupted entry
            warn!(
                "corrupted lfn entry! {:x} {:x} {:x} {:x}",
                data.order(),
                self.index,
                data.checksum(),
                self.chksum
            );
            self.clear();
            return;
        } else {
            // Decrement LFN index only for non-last entries
            self.index -= 1;
        }
        let pos = LFN_PART_LEN * (index - 1) as usize;
        // copy name parts into LFN buffer
        data.copy_name_to_slice(&mut self.buf.ucs2_units[pos..pos + 13]);
    }

    fn validate_chksum(&mut self, short_name: &[u8; SFN_SIZE]) {
        if self.is_empty() {
            // Nothing to validate - no LFN entries has been processed
            return;
        }
        let chksum = lfn_checksum(short_name);
        if chksum != self.chksum {
            warn!("checksum mismatch {:x} {:x} {:?}", chksum, self.chksum, short_name);
            self.clear();
        }
    }
}

// Dummy implementation for non-alloc build
#[cfg(not(feature = "lfn"))]
struct LongNameBuilder {}
#[cfg(not(feature = "lfn"))]
impl LongNameBuilder {
    fn new() -> Self {
        LongNameBuilder {}
    }
    fn clear(&mut self) {}
    fn into_vec(self) {}
    fn truncate(&mut self) {}
    fn process(&mut self, _data: &DirLfnEntryData) {}
    fn validate_chksum(&mut self, _short_name: &[u8; SFN_SIZE]) {}
}

#[cfg(feature = "lfn")]
struct LfnEntriesGenerator<'a> {
    name_parts_iter: iter::Rev<slice::Chunks<'a, u16>>,
    checksum: u8,
    index: usize,
    num: usize,
    ended: bool,
}

#[cfg(feature = "lfn")]
impl<'a> LfnEntriesGenerator<'a> {
    fn new(name_utf16: &'a [u16], checksum: u8) -> Self {
        let num_entries = (name_utf16.len() + LFN_PART_LEN - 1) / LFN_PART_LEN;
        // create generator using reverse iterator over chunks - first chunk can be shorter
        LfnEntriesGenerator {
            checksum,
            name_parts_iter: name_utf16.chunks(LFN_PART_LEN).rev(),
            index: 0,
            num: num_entries,
            ended: false,
        }
    }
}

#[cfg(feature = "lfn")]
impl Iterator for LfnEntriesGenerator<'_> {
    type Item = DirLfnEntryData;

    fn next(&mut self) -> Option<Self::Item> {
        if self.ended {
            return None;
        }

        // get next part from reverse iterator
        match self.name_parts_iter.next() {
            Some(ref name_part) => {
                let lfn_index = self.num - self.index;
                let mut order = lfn_index as u8;
                if self.index == 0 {
                    // this is last name part (written as first)
                    order |= LFN_ENTRY_LAST_FLAG;
                }
                debug_assert!(order > 0);
                // name is padded with ' '
                const LFN_PADDING: u16 = 0xFFFF;
                let mut lfn_part = [LFN_PADDING; LFN_PART_LEN];
                lfn_part[..name_part.len()].copy_from_slice(&name_part);
                if name_part.len() < LFN_PART_LEN {
                    // name is only zero-terminated if its length is not multiplicity of LFN_PART_LEN
                    lfn_part[name_part.len()] = 0;
                }
                // create and return new LFN entry
                let mut lfn_entry = DirLfnEntryData::new(order, self.checksum);
                lfn_entry.copy_name_from_slice(&lfn_part);
                self.index += 1;
                Some(lfn_entry)
            }
            None => {
                // end of name
                self.ended = true;
                None
            }
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.name_parts_iter.size_hint()
    }
}

// name_parts_iter is ExactSizeIterator so size_hint returns one limit
#[cfg(feature = "lfn")]
impl ExactSizeIterator for LfnEntriesGenerator<'_> {}

// Dummy implementation for non-alloc build
#[cfg(not(feature = "lfn"))]
struct LfnEntriesGenerator {}
#[cfg(not(feature = "lfn"))]
impl LfnEntriesGenerator {
    fn new(_name_utf16: &[u16], _checksum: u8) -> Self {
        LfnEntriesGenerator {}
    }
}
#[cfg(not(feature = "lfn"))]
impl Iterator for LfnEntriesGenerator {
    type Item = DirLfnEntryData;

    fn next(&mut self) -> Option<Self::Item> {
        None
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (0, Some(0))
    }
}
#[cfg(not(feature = "lfn"))]
impl ExactSizeIterator for LfnEntriesGenerator {}

#[derive(Default, Debug, Clone)]
struct ShortNameGenerator {
    chksum: u16,
    long_prefix_bitmap: u16,
    prefix_chksum_bitmap: u16,
    name_fits: bool,
    lossy_conv: bool,
    exact_match: bool,
    basename_len: u8,
    short_name: [u8; SFN_SIZE],
}

impl ShortNameGenerator {
    fn new(name: &str) -> Self {
        // padded by ' '
        let mut short_name = [SFN_PADDING; SFN_SIZE];
        // find extension after last dot
        // Note: short file name cannot start with the extension
        let (basename_len, name_fits, lossy_conv) = match name.rfind('.') {
            Some(0) | None => {
                // file starts with a '.', or it has no extension - copy name and leave extension
                // empty.
                let (basename_len, basename_fits, basename_lossy) =
                    Self::copy_short_name_part(&mut short_name[0..8], &name);
                (basename_len, basename_fits, basename_lossy)
            }
            Some(dot_index) => {
                // extension found - copy parts before and after dot
                let (basename_len, basename_fits, basename_lossy) =
                    Self::copy_short_name_part(&mut short_name[0..8], &name[..dot_index]);
                // This is safe, because '.' is always exactly one byte - we don't risk
                // accidentally indexing mid-character.
                let (_, ext_fits, ext_lossy) =
                    Self::copy_short_name_part(&mut short_name[8..11], &name[dot_index + 1..]);
                (basename_len, basename_fits && ext_fits, basename_lossy || ext_lossy)
            }
        };
        let chksum = Self::checksum(&name);
        Self {
            short_name,
            chksum,
            name_fits,
            lossy_conv,
            basename_len: basename_len as u8,
            ..Default::default()
        }
    }

    fn generate_dot() -> [u8; SFN_SIZE] {
        let mut short_name = [SFN_PADDING; SFN_SIZE];
        short_name[0] = b'.';
        short_name
    }

    fn generate_dotdot() -> [u8; SFN_SIZE] {
        let mut short_name = [SFN_PADDING; SFN_SIZE];
        short_name[0] = b'.';
        short_name[1] = b'.';
        short_name
    }

    fn copy_short_name_part(dst: &mut [u8], src: &str) -> (usize, bool, bool) {
        let mut dst_pos = 0;
        let mut lossy_conv = false;
        for c in src.chars() {
            if dst_pos == dst.len() {
                // result buffer is full
                return (dst_pos, false, lossy_conv);
            }
            // Make sure character is allowed in 8.3 name
            let fixed_c = match c {
                // strip spaces and dots
                ' ' | '.' => {
                    lossy_conv = true;
                    continue;
                }
                // copy allowed characters
                'A'..='Z' | 'a'..='z' | '0'..='9' => c,
                '!' | '#' | '$' | '%' | '&' | '\'' | '(' | ')' | '-' | '@' | '^' | '_' | '`'
                | '{' | '}' | '~' => c,
                // replace disallowed characters by underscore
                _ => '_',
            };
            // Update 'lossy conversion' flag
            lossy_conv = lossy_conv || (fixed_c != c);
            // short name is always uppercase
            let upper = fixed_c.to_ascii_uppercase();
            dst[dst_pos] = upper as u8; // SAFE: upper is in range 0x20-0x7F
            dst_pos += 1;
        }
        (dst_pos, true, lossy_conv)
    }

    fn add_existing(&mut self, short_name: &[u8; SFN_SIZE]) {
        // check for exact match collision
        if short_name == &self.short_name {
            self.exact_match = true;
        }
        // check for long prefix form collision (TEXTFI~1.TXT)
        let long_prefix_len = cmp::min(self.basename_len, 6) as usize;
        let num_suffix = if short_name[long_prefix_len] == b'~' {
            (short_name[long_prefix_len + 1] as char).to_digit(10)
        } else {
            None
        };
        let long_prefix_matches =
            short_name[..long_prefix_len] == self.short_name[..long_prefix_len];
        let ext_matches = short_name[8..] == self.short_name[8..];
        if long_prefix_matches && num_suffix.is_some() && ext_matches {
            let num = num_suffix.unwrap(); // SAFE: checked in if condition
            self.long_prefix_bitmap |= 1 << num;
        }

        // check for short prefix + checksum form collision (TE021F~1.TXT)
        let short_prefix_len = cmp::min(self.basename_len, 2) as usize;
        let num_suffix = if short_name[short_prefix_len + 4] == b'~' {
            (short_name[short_prefix_len + 4 + 1] as char).to_digit(10)
        } else {
            None
        };
        let short_prefix_matches =
            short_name[..short_prefix_len] == self.short_name[..short_prefix_len];
        if short_prefix_matches && num_suffix.is_some() && ext_matches {
            let chksum_res = str::from_utf8(&short_name[short_prefix_len..short_prefix_len + 4])
                .map(|s| u16::from_str_radix(s, 16));
            if chksum_res == Ok(Ok(self.chksum)) {
                let num = num_suffix.unwrap(); // SAFE: checked in if condition
                self.prefix_chksum_bitmap |= 1 << num;
            }
        }
    }

    fn checksum(name: &str) -> u16 {
        // BSD checksum algorithm
        let mut chksum = num::Wrapping(0u16);
        for c in name.chars() {
            chksum = (chksum >> 1) + (chksum << 15) + num::Wrapping(c as u16);
        }
        chksum.0
    }

    fn generate(&self) -> io::Result<[u8; SFN_SIZE]> {
        if !self.lossy_conv && self.name_fits && !self.exact_match {
            // If there was no lossy conversion and name fits into
            // 8.3 convention and there is no collision return it as is
            return Ok(self.short_name);
        }
        // Try using long 6-characters prefix
        for i in 1..5 {
            if self.long_prefix_bitmap & (1 << i) == 0 {
                return Ok(self.build_prefixed_name(i, false));
            }
        }
        // Try prefix with checksum
        for i in 1..10 {
            if self.prefix_chksum_bitmap & (1 << i) == 0 {
                return Ok(self.build_prefixed_name(i, true));
            }
        }
        // Too many collisions - fail
        Err(io::Error::new(ErrorKind::AlreadyExists, "short name already exists"))
    }

    fn next_iteration(&mut self) {
        // Try different checksum in next iteration
        self.chksum = (num::Wrapping(self.chksum) + num::Wrapping(1)).0;
        // Zero bitmaps
        self.long_prefix_bitmap = 0;
        self.prefix_chksum_bitmap = 0;
    }

    fn build_prefixed_name(&self, num: u32, with_chksum: bool) -> [u8; SFN_SIZE] {
        let mut buf = [SFN_PADDING; SFN_SIZE];
        let prefix_len = if with_chksum {
            let prefix_len = cmp::min(self.basename_len as usize, 2);
            buf[..prefix_len].copy_from_slice(&self.short_name[..prefix_len]);
            buf[prefix_len..prefix_len + 4].copy_from_slice(&Self::u16_to_u8_array(self.chksum));
            prefix_len + 4
        } else {
            let prefix_len = cmp::min(self.basename_len as usize, 6);
            buf[..prefix_len].copy_from_slice(&self.short_name[..prefix_len]);
            prefix_len
        };
        buf[prefix_len] = b'~';
        buf[prefix_len + 1] = char::from_digit(num, 10).unwrap() as u8; // SAFE
        buf[8..].copy_from_slice(&self.short_name[8..]);
        buf
    }

    fn u16_to_u8_array(x: u16) -> [u8; 4] {
        let c1 = char::from_digit((x as u32 >> 12) & 0xF, 16).unwrap().to_ascii_uppercase() as u8;
        let c2 = char::from_digit((x as u32 >> 8) & 0xF, 16).unwrap().to_ascii_uppercase() as u8;
        let c3 = char::from_digit((x as u32 >> 4) & 0xF, 16).unwrap().to_ascii_uppercase() as u8;
        let c4 = char::from_digit((x as u32 >> 0) & 0xF, 16).unwrap().to_ascii_uppercase() as u8;
        [c1, c2, c3, c4]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_split_path() {
        assert_eq!(split_path("aaa/bbb/ccc"), Ok(("aaa", Some("bbb/ccc"))));
        assert_eq!(split_path("aaa/bbb"), Ok(("aaa", Some("bbb"))));
        assert_eq!(split_path("aaa"), Ok(("aaa", None)));
    }

    #[test]
    fn test_generate_short_name() {
        assert_eq!(&ShortNameGenerator::new("Foo").generate().unwrap(), b"FOO        ");
        assert_eq!(&ShortNameGenerator::new("Foo.b").generate().unwrap(), b"FOO     B  ");
        assert_eq!(&ShortNameGenerator::new("Foo.baR").generate().unwrap(), b"FOO     BAR");
        assert_eq!(&ShortNameGenerator::new("Foo+1.baR").generate().unwrap(), b"FOO_1~1 BAR");
        assert_eq!(&ShortNameGenerator::new("ver +1.2.text").generate().unwrap(), b"VER_12~1TEX");
        assert_eq!(&ShortNameGenerator::new(".bashrc.swp").generate().unwrap(), b"BASHRC~1SWP");
        assert_eq!(&ShortNameGenerator::new(".foo").generate().unwrap(), b"FOO~1      ");
    }

    #[test]
    fn test_short_name_checksum_overflow() {
        ShortNameGenerator::checksum("\u{FF5A}\u{FF5A}\u{FF5A}\u{FF5A}");
    }

    #[test]
    fn test_lfn_checksum_overflow() {
        lfn_checksum(&[
            0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8, 0xFFu8,
        ]);
    }

    #[test]
    fn test_generate_short_name_collisions_long() {
        let mut buf: [u8; SFN_SIZE];
        let mut gen = ShortNameGenerator::new("TextFile.Mine.txt");
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TEXTFI~1TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TEXTFI~2TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TEXTFI~3TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TEXTFI~4TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TE527D~1TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TE527D~2TXT");
        for i in 3..10 {
            gen.add_existing(&buf);
            buf = gen.generate().unwrap();
            assert_eq!(&buf, format!("TE527D~{}TXT", i).as_bytes());
        }
        gen.add_existing(&buf);
        assert!(gen.generate().is_err());
        gen.next_iteration();
        for _i in 0..4 {
            buf = gen.generate().unwrap();
            gen.add_existing(&buf);
        }
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"TE527E~1TXT");
    }

    #[test]
    fn test_generate_short_name_collisions_short() {
        let mut buf: [u8; SFN_SIZE];
        let mut gen = ShortNameGenerator::new("x.txt");
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X       TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X~1     TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X~2     TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X~3     TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X~4     TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X40DA~1 TXT");
        gen.add_existing(&buf);
        buf = gen.generate().unwrap();
        assert_eq!(&buf, b"X40DA~2 TXT");
    }

    #[test]
    fn test_long_name_requires_long_name() {
        assert_eq!(
            validate_long_name("01234567.1234", b"01234567123").expect("should be valid"),
            true
        );
    }

    #[test]
    fn test_dot_and_dotdot_does_not_require_long_name() {
        assert_eq!(validate_long_name(".", b".          ").expect("should be valid"), false);
        assert_eq!(validate_long_name("..", b"..         ").expect("should be valid"), false);
    }

    #[test]
    fn test_lowercase_and_special_requires_long_name() {
        assert_eq!(validate_long_name("abc", b"abc        ").expect("should be valid"), true);
        assert_eq!(validate_long_name(",", b",          ").expect("should be valid"), true);
        assert_eq!(
            validate_long_name("FOO\u{100}", b"FOO        ").expect("should be valid"),
            true
        );
    }

    #[test]
    fn test_name_with_extension_does_not_require_long_name() {
        assert_eq!(validate_long_name("NAME.EXT", b"NAME    EXT").expect("should be valid"), false);
    }

    #[test]
    fn test_name_with_long_extension_does_require_long_name() {
        assert_eq!(validate_long_name("NAME.EXT1", b"NAME    EXT").expect("should be valid"), true);
    }
}
