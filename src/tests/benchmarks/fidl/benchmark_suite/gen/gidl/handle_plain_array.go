// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

package gidl

import (
	"fmt"
	"gen/config"
	"gen/gidl/util"
)

func init() {
	util.Register(config.GidlFile{
		Filename: "handle_array_plain.gen.gidl",
		Gen:      gidlGenHandleArrayPlain,
		Benchmarks: []config.Benchmark{
			{
				Name:    "HandleArray/Plain/1",
				Comment: `1 plain handle array in a struct`,
				Config: config.Config{
					"size": 1,
				},
				// The FIDL type of the handle is a plain handle, but the handle value still needs
				// a type, chosen here to be 'event' for better comparison with the
				// event_handle_array benchmark.
				HandleDefs: util.RepeatHandleDef(config.HandleDef{Subtype: config.Event}, 1),
				Denylist:   []config.Binding{config.Go},
			},
			{
				Name:    "HandleArray/Plain/16",
				Comment: `16 plain handle array in a struct`,
				Config: config.Config{
					"size": 16,
				},
				// The FIDL type of the handle is a plain handle, but the handle value still needs
				// a type, chosen here to be 'event' for better comparison with the
				// event_handle_array benchmark.
				HandleDefs: util.RepeatHandleDef(config.HandleDef{Subtype: config.Event}, 16),
				Denylist:   []config.Binding{config.Go},
			},
			{
				Name:    "HandleArray/Plain/64",
				Comment: `64 plain handle array in a struct`,
				Config: config.Config{
					"size": 64,
				},
				// The FIDL type of the handle is a plain handle, but the handle value still needs
				// a type, chosen here to be 'event' for better comparison with the
				// event_handle_array benchmark.
				HandleDefs: util.RepeatHandleDef(config.HandleDef{Subtype: config.Event}, 64),
				Denylist:   []config.Binding{config.Go},
			},
		},
	})
}

func gidlGenHandleArrayPlain(conf config.Config) (string, error) {
	size := conf.GetInt("size")
	handleValues := ""
	for i := 0; i < size; i++ {
		handleValues += fmt.Sprintf("#%d,\n", i)
	}
	return fmt.Sprintf(`
HandleArrayPlain%[1]d{
	handles: [
%[2]s
	]
}`, size, handleValues), nil
}
