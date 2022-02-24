// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Defines multiple types of inspect containers: Mapped VMO for production and byte arrays
//! for testing.

use mapped_vmo::Mapping;
use std::{cmp::min, ptr, sync::Arc};

/// Trait implemented by an Inspect container that can be read from.
pub trait ReadableBlockContainer {
    /// Writes the container at the given `offset` into the given `bytes`.
    fn read_bytes(&self, offset: usize, bytes: &mut [u8]) -> usize;
}

/// Trait implemented by an Inspect container that can be written to.
pub trait WritableBlockContainer {
    /// Writes the given `bytes` at the given `offset` in the container.
    fn write_bytes(&self, offset: usize, bytes: &[u8]) -> usize;
}

/// Trait implemented to compare two inspect containers for equality.
pub trait BlockContainerEq<RHS = Self> {
    /// Returns true if the other container is the same.
    fn ptr_eq(&self, other: &RHS) -> bool;
}

impl ReadableBlockContainer for Arc<Mapping> {
    fn read_bytes(&self, offset: usize, bytes: &mut [u8]) -> usize {
        self.read_at(offset, bytes) as usize
    }
}

impl ReadableBlockContainer for &[u8] {
    fn read_bytes(&self, offset: usize, bytes: &mut [u8]) -> usize {
        if offset >= self.len() {
            return 0;
        }
        let upper_bound = min(self.len(), bytes.len() + offset);
        let bytes_read = upper_bound - offset;
        bytes[..bytes_read].clone_from_slice(&self[offset..upper_bound]);
        bytes_read
    }
}

impl BlockContainerEq for Arc<Mapping> {
    fn ptr_eq(&self, other: &Arc<Mapping>) -> bool {
        Arc::ptr_eq(&self, &other)
    }
}

impl BlockContainerEq for &[u8] {
    fn ptr_eq(&self, other: &&[u8]) -> bool {
        ptr::eq(*self, *other)
    }
}

impl WritableBlockContainer for Arc<Mapping> {
    fn write_bytes(&self, offset: usize, bytes: &[u8]) -> usize {
        self.write_at(offset, bytes) as usize
    }
}

#[cfg(test)]
impl WritableBlockContainer for &[u8] {
    fn write_bytes(&self, offset: usize, bytes: &[u8]) -> usize {
        if offset >= self.len() {
            return 0;
        }
        let bytes_written = min(self.len() - offset, bytes.len());
        let base = (self.as_ptr() as usize).checked_add(offset).unwrap() as *mut u8;
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), base, bytes_written) };
        bytes_written
    }
}
