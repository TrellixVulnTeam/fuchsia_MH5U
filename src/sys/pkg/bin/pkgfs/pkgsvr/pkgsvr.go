// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain
// +build !build_with_native_toolchain

package pkgsvr

import (
	"context"
	"flag"
	"log"
	"syscall"
	"syscall/zx"
	"syscall/zx/fdio"

	"go.fuchsia.dev/fuchsia/src/lib/component"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pkgfs/pkgfs"
)

// Main starts a package server program
func Main() {
	var (
		blob                                   = flag.String("blob", "/blob", "Path at which to store blobs")
		enforcePkgfsPackagesNonStaticAllowlist = flag.Bool("enforcePkgfsPackagesNonStaticAllowlist",
			true,
			"Whether to enforce the allowlist of non-static packages allowed to appear in /pkgfs/packages")
		enforceNonBaseExecutabilityRestrictions = flag.Bool("enforceNonBaseExecutabilityRestrictions", true,
			"Whether to enforce the restrictions to executability of files in packages to just packages in base or the allowlist")
	)

	log.SetPrefix("pkgsvr: ")
	log.SetFlags(0) // no time required
	flag.Parse()

	sysPkg := flag.Arg(0)

	blobDir, err := syscall.OpenPath(*blob, syscall.O_RDWR|syscall.O_DIRECTORY, 0777)
	if err != nil {
		log.Fatalf("pkgfs: failed to open %q: %s", *blob, err)
	}

	log.Printf("pkgfs: enforce pkgfs/packages non-static allowlist: %v", *enforcePkgfsPackagesNonStaticAllowlist)
	log.Printf("pkgfs: enforce executability restrictions: %v", *enforceNonBaseExecutabilityRestrictions)
	fs, err := pkgfs.New(blobDir.(*fdio.Directory), *enforcePkgfsPackagesNonStaticAllowlist, *enforceNonBaseExecutabilityRestrictions)
	if err != nil {
		log.Fatalf("pkgfs: initialization failed: %s", err)
	}

	h := component.GetStartupHandle(component.HandleInfo{Type: component.HandleUser0, Arg: 0})
	if h == zx.HandleInvalid {
		log.Fatalf("pkgfs: mount failed, no serving handle supplied in startup arguments")
	}

	if sysPkg != "" {
		if err := fs.SetSystemRoot(sysPkg); err != nil {
			log.Printf("system: failed to set system root from blob %q: %s", sysPkg, err)
		}
		log.Printf("system: will be served from %s", sysPkg)
	} else {
		log.Printf("system: no system package blob supplied")
	}

	log.Printf("pkgfs serving blobfs %s", *blob)
	if err := fs.Serve(zx.Channel(h)); err != nil {
		log.Fatalf("pkgfs: serve failed on startup handle: %s", err)
	}
	component.NewContextFromStartupInfo().BindStartupHandle(context.Background())
}
