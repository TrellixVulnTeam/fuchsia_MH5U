// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package testlib a test library for testing implementations of the
// block.Device interface.
package blocktest

import (
	"bytes"
	"math/rand"
	"testing"

	"go.fuchsia.dev/fuchsia/src/lib/thinfs/block"
)

const (
	numIterations = 100
)

// ReadAt tests the block.Device.ReadAt implementation. buf must be a []byte
// with the same contents as dev.
func ReadAt(t *testing.T, dev block.Device, r *rand.Rand, buf []byte) {
	blockSize := dev.BlockSize()
	numBlocks := dev.Size() / blockSize

	if int64(len(buf)) != dev.Size() {
		t.Fatalf("len(buf) = %v; want %v\n", len(buf), dev.Size())
	}

	// Read a random number of blocks from a random offset.
	for i := int64(0); i < numIterations; i++ {
		block := r.Int63n(numBlocks)
		off := block * dev.BlockSize()
		count := r.Int63n(numBlocks-block) * dev.BlockSize()
		if count == 0 {
			count = dev.BlockSize()
		}

		expected := buf[off : off+count]
		actual := make([]byte, count)

		if _, err := dev.ReadAt(actual, off); err != nil {
			t.Errorf("Error reading %v bytes from offset %v: %v\n", count, off, err)
			continue
		}

		if !bytes.Equal(actual, expected) {
			t.Errorf("Mismatched byte slices for %v byte read from offset %v\n", count, off)
		}
	}
}

// WriteAt tests the block.Device.WriteAt implementation. buf must be a []byte
// with the same contents as dev.
func WriteAt(t *testing.T, dev block.Device, r *rand.Rand, buf []byte) {
	blockSize := dev.BlockSize()
	numBlocks := dev.Size() / blockSize

	if int64(len(buf)) != dev.Size() {
		t.Fatalf("len(buf) = %v; want %v\n", len(buf), dev.Size())
	}

	// Write a random number of blocks to a random offset.
	for i := int64(0); i < numIterations; i++ {
		block := r.Int63n(numBlocks)
		off := block * blockSize
		count := r.Int63n(numBlocks-block) * blockSize
		if count == 0 {
			count = blockSize
		}

		expected := make([]byte, count)
		r.Read(expected)

		if _, err := dev.WriteAt(expected, off); err != nil {
			t.Errorf("Error writing %v bytes from offset %v: %v\n", count, off, err)
			continue
		}

		copy(buf[off:], expected)
	}

	actual := make([]byte, dev.Size())
	if _, err := dev.ReadAt(actual, 0); err != nil {
		t.Error("Error reading contents of device: ", err)
	}
	if !bytes.Equal(actual, buf) {
		t.Error("Device contents differ from expected contents")
	}
}

// ErrorPaths tests that block.Device implementations return errors when clients
// attempt to perform any operations with invalid arguments.
func ErrorPaths(t *testing.T, dev block.Device) {
	blockSize := dev.BlockSize()

	// Write a random number of blocks to a random offset.
	// Offset is not aligned.
	off := blockSize + 1
	if _, err := dev.ReadAt([]byte{}, off); err == nil {
		t.Error("dev.ReadAt returned a nil error for an unaligned offset")
	}
	if _, err := dev.WriteAt([]byte{}, off); err == nil {
		t.Error("dev.WriteAt returned a nil error for an unaligned offset")
	}

	// len(p) is not aligned.
	p := make([]byte, blockSize-1)
	off = blockSize
	if _, err := dev.ReadAt(p, off); err == nil {
		t.Error("dev.ReadAt returned a nil error for an unaligned len(p)")
	}
	if _, err := dev.WriteAt(p, off); err == nil {
		t.Error("dev.WriteAt returned a nil error for an unaligned len(p)")
	}

	// Range is out of bounds.
	off = dev.Size() - blockSize
	p = make([]byte, 2*blockSize)
	if _, err := dev.ReadAt(p, off); err == nil {
		t.Error("dev.ReadAt returned a nil error for an out of bounds range")
	}
	if _, err := dev.WriteAt(p, off); err == nil {
		t.Error("dev.WriteAt returned a nil error for an out of bounds range")
	}
}
