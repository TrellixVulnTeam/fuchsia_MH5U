// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

package util

import "gen/config"

var gidlFiles []config.GidlFile

func Register(gf config.GidlFile) {
	gidlFiles = append(gidlFiles, gf)
}

func AllGidlFiles() []config.GidlFile {
	return gidlFiles
}
