// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

package main

import (
	"context"
	lib "fidl/fidl/test/protocolmethodremove"
	"syscall/zx/fidl"
)

// [START contents]
type client struct {
	removeMethod *lib.ExampleWithCtxInterface
}

func (c client) test() {
	c.removeMethod.ExistingMethod(context.Background())
	c.removeMethod.OldMethod(context.Background())
}

type server struct{}

// Assert that server implements the Example interface
var _ lib.ExampleWithCtx = &server{}

func (*server) ExistingMethod(fidl.Context) error {
	return nil
}

func (*server) OldMethod(fidl.Context) error {
	return nil
}

// [END contents]

func main() {}
