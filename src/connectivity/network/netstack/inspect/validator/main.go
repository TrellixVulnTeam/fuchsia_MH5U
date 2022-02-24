// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain
// +build !build_with_native_toolchain

package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"syscall/zx"
	"syscall/zx/fidl"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/inspect"
	"go.fuchsia.dev/fuchsia/src/lib/component"

	fidlinspect "fidl/fuchsia/inspect"
	"fidl/test/inspect/validate"
)

type vmoReader struct {
	vmo    zx.VMO
	offset int64
}

func (r *vmoReader) Read(b []byte) (int, error) {
	n, err := r.ReadAt(b, r.offset)
	if err == nil {
		r.offset += int64(n)
	}
	return n, err
}

func (r *vmoReader) ReadAt(b []byte, offset int64) (int, error) {
	if err := r.vmo.Read(b, uint64(offset)); err != nil {
		return 0, err
	}
	return len(b), nil
}

func (r *vmoReader) Seek(offset int64, whence int) (int64, error) {
	var abs int64
	switch whence {
	case io.SeekStart:
		abs = offset
	case io.SeekCurrent:
		abs = r.offset + offset
	case io.SeekEnd:
		size, err := r.vmo.Size()
		if err != nil {
			return r.offset, err
		}
		abs = int64(size) + offset
	default:
		return 0, fmt.Errorf("%T.Seek: invalid whence: %d", r, whence)
	}
	if abs < 0 {
		return 0, fmt.Errorf("%T.Seek: negative position: %d", r, abs)
	}
	r.offset = abs
	return abs, nil
}

type vmoWriter struct {
	vmo    zx.VMO
	offset uint64
}

func (w *vmoWriter) Write(b []byte) (int, error) {
	if err := w.vmo.Write(b, w.offset); err != nil {
		return 0, err
	}
	w.offset += uint64(len(b))
	return len(b), nil
}

type impl struct {
	vmo       zx.VMO
	writer    *inspect.Writer
	nodes     map[uint32]uint32
	published bool
}

const inspectName = "root.inspect"

var _ component.Directory = (*impl)(nil)

func (i *impl) Get(name string) (component.Node, bool) {
	if i.published && name == inspectName {
		return &component.FileWrapper{File: i}, true
	}
	return nil, false
}

func (i *impl) ForEach(fn func(string, component.Node)) {
	if i.published {
		fn(inspectName, &component.FileWrapper{File: i})
	}
}

var _ component.File = (*impl)(nil)

func (i *impl) GetReader() (component.Reader, uint64) {
	h, err := i.vmo.Handle().Duplicate(zx.RightSameRights)
	if err != nil {
		panic(err)
	}
	vmo := zx.VMO(h)
	size, err := vmo.Size()
	if err != nil {
		panic(err)
	}
	return &vmoReader{
		vmo: vmo,
	}, size
}

func (i *impl) GetVMO() zx.VMO {
	return i.vmo
}

func (i *impl) Initialize(_ fidl.Context, params validate.InitializationParams) (zx.Handle, validate.TestResult, error) {
	if !params.HasVmoSize() {
		return zx.HandleInvalid, validate.TestResultIllegal, nil
	}
	size := params.GetVmoSize()
	{
		vmo, err := zx.NewVMO(size, 0)
		if err != nil {
			panic(err)
		}
		i.vmo = vmo
	}
	{
		w, err := inspect.NewWriter(&vmoWriter{
			vmo: i.vmo,
		})
		if err != nil {
			panic(err)
		}
		i.writer = w
	}

	h, err := i.vmo.Handle().Duplicate(zx.RightSameRights)
	if err != nil {
		panic(err)
	}

	return h, validate.TestResultOk, nil
}

func (*impl) InitializeTree(fidl.Context, validate.InitializationParams) (fidlinspect.TreeWithCtxInterface, validate.TestResult, error) {
	return fidlinspect.TreeWithCtxInterface{}, validate.TestResultUnimplemented, nil
}

func (i *impl) Publish(fidl.Context) (validate.TestResult, error) {
	i.published = true
	return validate.TestResultOk, nil
}

func (i *impl) Unpublish(fidl.Context) (validate.TestResult, error) {
	i.published = false
	return validate.TestResultOk, nil
}

func (i *impl) Act(ctx fidl.Context, action validate.Action) (validate.TestResult, error) {
	switch action.Which() {
	case validate.ActionCreateNode:
		action := action.CreateNode
		index, err := i.writer.WriteNodeValueBlock(i.nodes[action.Parent], action.Name)
		if err != nil {
			panic(err)
		}
		i.nodes[action.Id] = index
		return validate.TestResultOk, nil
	case validate.ActionDeleteNode:
	case validate.ActionCreateNumericProperty:
	case validate.ActionCreateBytesProperty:
		action := action.CreateBytesProperty
		var r bytes.Reader
		r.Reset(action.Value)
		if err := i.writer.WriteBinary(i.nodes[action.Parent], action.Name, uint32(r.Len()), &r); err != nil {
			panic(err)
		}
		// WriteBinary might write lots of nodes, which index do we store? We'll
		// need to figure it out when/if we implement deletion.
		i.nodes[action.Id] = 0
		return validate.TestResultOk, nil
	case validate.ActionCreateStringProperty:
	case validate.ActionDeleteProperty:
	case validate.ActionSetNumber:
	case validate.ActionSetString:
	case validate.ActionSetBytes:
	case validate.ActionAddNumber:
	case validate.ActionSubtractNumber:
	case validate.ActionCreateArrayProperty:
	case validate.ActionArraySet:
	case validate.ActionArrayAdd:
	case validate.ActionArraySubtract:
	case validate.ActionCreateLinearHistogram:
	case validate.ActionCreateExponentialHistogram:
	case validate.ActionInsert:
	case validate.ActionInsertMultiple:
	case validate.ActionCreateBoolProperty:
	case validate.ActionSetBool:
	}
	return validate.TestResultUnimplemented, nil
}

func (*impl) ActLazy(fidl.Context, validate.LazyAction) (validate.TestResult, error) {
	return validate.TestResultUnimplemented, nil
}

func main() {
	appCtx := component.NewContextFromStartupInfo()

	i := impl{
		nodes: make(map[uint32]uint32),
	}
	appCtx.OutgoingService.AddDiagnostics("root", &component.DirectoryWrapper{
		Directory: &i,
	})
	stub := validate.ValidateWithCtxStub{Impl: &i}
	appCtx.OutgoingService.AddService(
		validate.ValidateName,
		func(ctx context.Context, c zx.Channel) error {
			go component.Serve(ctx, &stub, c, component.ServeOptions{
				OnError: func(err error) {
					panic(err)
				},
			})
			return nil
		},
	)

	appCtx.BindStartupHandle(context.Background())
}
