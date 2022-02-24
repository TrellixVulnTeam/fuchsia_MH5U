// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

// Package pkgfs hosts a filesystem for interacting with packages that are
// stored on a host. It presents a tree of packages that are locally available
// and a tree that enables a user to add new packages and/or package content to
// the host.
package pkgfs

import (
	"fmt"
	"io"
	"log"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"sync"
	"syscall/zx"
	"syscall/zx/fdio"
	"time"

	"go.fuchsia.dev/fuchsia/src/lib/thinfs/fs"
	"go.fuchsia.dev/fuchsia/src/lib/thinfs/zircon/rpc"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pkgfs/allowlist"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pkgfs/blobfs"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pkgfs/index"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/pkg"
)

// Filesystem is the top level container for a pkgfs server
type Filesystem struct {
	root                                    *rootDirectory
	static                                  *index.StaticIndex
	index                                   *index.DynamicIndex
	blobfs                                  *blobfs.Manager
	mountInfo                               mountInfo
	mountTime                               time.Time
	allowedNonStaticPackages                *allowlist.Allowlist
	enforceNonBaseExecutabilityRestrictions bool
}

// New initializes a new pkgfs filesystem server
func New(blobDir *fdio.Directory, enforcePkgfsPackagesNonStaticAllowlist bool, enforceNonBaseExecutabilityRestrictions bool) (*Filesystem, error) {
	bm, err := blobfs.New(blobDir)
	if err != nil {
		return nil, fmt.Errorf("pkgfs: open blobfs: %s", err)
	}

	static := index.NewStatic()
	f := &Filesystem{
		static: static,
		index:  index.NewDynamic(static),
		blobfs: bm,
		mountInfo: mountInfo{
			parentFd: -1,
		},
		enforceNonBaseExecutabilityRestrictions: enforceNonBaseExecutabilityRestrictions,
	}

	f.root = &rootDirectory{
		unsupportedDirectory: unsupportedDirectory("/"),
		fs:                   f,

		dirs: map[string]fs.Directory{
			"ctl": &ctlDirectory{
				unsupportedDirectory: unsupportedDirectory("/ctl"),
				fs:                   f,
				dirs: map[string]fs.Directory{
					"do-not-use-this-garbage": unsupportedDirectory("/ctl/do-not-use-this-garbage"),
					"validation": &validationDir{
						unsupportedDirectory: unsupportedDirectory("/ctl/validation"),
						fs:                   f,
					},
				},
			},
			"install": &installDir{
				unsupportedDirectory: unsupportedDirectory("/install"),
				fs:                   f,
			},
			"needs": &needsRoot{
				unsupportedDirectory: unsupportedDirectory("/needs"),
				fs:                   f,
			},
			"packages": &packagesRoot{
				unsupportedDirectory:      unsupportedDirectory("/packages"),
				fs:                        f,
				enforceNonStaticAllowlist: enforcePkgfsPackagesNonStaticAllowlist,
			},
			"versions": &versionsDirectory{
				unsupportedDirectory: unsupportedDirectory("/versions"),
				fs:                   f,
			},
			"system": unsupportedDirectory("/system"),
		},
	}

	return f, nil
}

// staticIndexPath is the path inside the system package directory that contains the static packages for that system version.
const staticIndexPath = "data/static_packages"
const cacheIndexPath = "data/cache_packages"
const disableExecutabilityEnforcementPath = "data/pkgfs_disable_executability_restrictions"
const packagesAllowlistPath = "data/pkgfs_packages_non_static_packages_allowlist.txt"

// loadStaticIndex loads the blob specified by root from blobfs. A non-nil
// *StaticIndex is always returned. If an error is returned that indicates a
// problem reading the index content from disk and therefore the StaticIndex
// returned may be empty.
func loadStaticIndex(static *index.StaticIndex, blobfs *blobfs.Manager, root string, systemImage pkg.Package, systemImageMerkleroot string) error {
	indexFile, err := blobfs.Open(root)
	if err != nil {
		return fmt.Errorf("pkgfs: could not load static index from blob %s: %s", root, err)
	}
	defer indexFile.Close()

	return static.LoadFrom(indexFile, systemImage, systemImageMerkleroot)
}

func (f *Filesystem) loadCacheIndex(root string) error {
	log.Println("pkgfs: loading cache index")
	start := time.Now()
	indexFile, err := f.blobfs.Open(root)
	if err != nil {
		return fmt.Errorf("pkgfs: could not load cache index from blob %s: %s", root, err)
	}
	defer indexFile.Close()

	entries, err := index.ParseIndexFile(indexFile)
	if err != nil {
		return fmt.Errorf("pkgfs: error parsing cache index: %v", err)
	}

	var foundCount int
	for _, entry := range entries {
		meta, err := f.blobfs.Open(entry.Merkle)
		if err != nil {
			// Package meta.far is missing, skip it.
			continue
		}
		meta.Close()
		if err := importPackage(f, entry.Merkle); err != nil && err != fs.ErrAlreadyExists {
			// This probably shouldn't happen if the meta far is present already.
			log.Printf("pkgfs: surprising error loading optional pkg %q: %v", entry.Key, err)
		}
		if f.index.IsInstalling(entry.Merkle) {
			// Some content blobs are missing.
			// Mark failed so we don't list the package in /pkgfs/needs/packages
			f.index.InstallingFailedForPackage(entry.Merkle)
		}
		if _, found := f.index.GetRoot(entry.Merkle); found {
			foundCount++
		}
	}
	log.Printf("pkgfs: cache index loaded in %.3fs; found %d/%d packages",
		time.Since(start).Seconds(), foundCount, len(entries))
	return nil
}

// Read the /pkgfs/packages allowlist for which non-static packages it should return,
// and set the allowlist in the packages directory.
func (f *Filesystem) retrievePackagesAllowlist(pd *packageDir) {
	log.Println("pkgfs: loading /pkgfs/packages non static allowlist")
	blob, ok := pd.getBlobFor(packagesAllowlistPath)
	if !ok {
		log.Printf("pkgfs: couldn't get a blob for /pkgfs/packages allowlist path %v", packagesAllowlistPath)
		return
	}

	if err := f.loadAllowedNonStaticPackages(blob); err != nil {
		log.Printf("pkgfs: could not load pkgfs/packages allowlist: %v", err)
		return
	}
}

func (f *Filesystem) loadAllowedNonStaticPackages(blob string) error {
	allowListFile, err := f.blobfs.Open(blob)
	if err != nil {
		return fmt.Errorf("could not load non_static_pkgs allowlist from blob %s: %v", blob, err)
	}
	defer allowListFile.Close()

	allowList, err := allowlist.LoadFrom(allowListFile)
	if err != nil {
		return err
	}

	log.Printf("pkgfs: parsed non-static pkgs allowlist: %v", allowList)
	f.allowedNonStaticPackages = allowList
	return nil
}

// SetSystemRoot sets/updates the merkleroot (and static index) that backs the /system partition and static package index.
func (f *Filesystem) SetSystemRoot(merkleroot string) error {
	pd, err := newPackageDirFromBlob(merkleroot, f, true)
	if err != nil {
		return err
	}
	f.root.setDir("system", pd)

	blob, ok := pd.getBlobFor(staticIndexPath)
	if !ok {
		return fmt.Errorf("pkgfs: new system root set, but new static index %q not found in %q", staticIndexPath, merkleroot)
	}

	err = loadStaticIndex(f.static, f.blobfs, blob, pkg.Package{
		Name:    "system_image",
		Version: "0",
	}, merkleroot)
	if err != nil {
		return err
	}

	blob, ok = pd.getBlobFor(cacheIndexPath)
	if ok {
		err := f.loadCacheIndex(blob)
		if err != nil {
			return err
		}
	}

	// Using our root package, retrieve the packages allowlist, if it exists,
	// and pass it off to the packages root directory.
	f.retrievePackagesAllowlist(pd)

	// If the marker blob is known (even if it doesn't exist in blobfs),
	// silently disable enforcement of executability restrictions.
	if _, ok := pd.getBlobFor(disableExecutabilityEnforcementPath); ok {
		f.enforceNonBaseExecutabilityRestrictions = false
	}

	return nil
}

// shouldAllowExecutableOpenByPackageName determines if a package should be
// allowed to open blobs as executable. Contents of a package can be executed
// if it is in the static index (without any runtime changes) or if it is in
// the allowlist.
func (f *Filesystem) shouldAllowExecutableOpenByPackageName(packageName string) bool {
	inStaticIndex := f.static != nil && f.static.HasStaticName(packageName)
	inAllowList := f.allowedNonStaticPackages != nil && f.allowedNonStaticPackages.Contains(packageName)

	return inStaticIndex || inAllowList
}

// shouldAllowExecutableOpenByPackageRoot determines if a package should be
// allowed to open blobs as executable. Contents of a package can be executed
// if it is in the static index (without any runtime changes) or if it is in
// the allowlist.
func (f *Filesystem) shouldAllowExecutableOpenByPackageRoot(packageRoot string, packageName string) bool {
	inStaticIndex := f.static != nil && f.static.HasStaticRoot(packageRoot)
	inAllowList := f.allowedNonStaticPackages != nil && f.allowedNonStaticPackages.Contains(packageName)

	return inStaticIndex || inAllowList
}

func (f *Filesystem) Blockcount() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobfs?
	return 0
}

func (f *Filesystem) Blocksize() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobfs?
	return 0
}

func (f *Filesystem) Size() int64 {
	// TODO(raggi): delegate to blobfs?
	return 0
}

func (f *Filesystem) Close() error {
	return nil
}

func (f *Filesystem) RootDirectory() fs.Directory {
	return f.root
}

func (f *Filesystem) Type() string {
	return "pkgfs"
}

func (f *Filesystem) FreeSize() int64 {
	return 0
}

func (f *Filesystem) DevicePath() string {
	return ""
}

// Serve starts a Directory protocol RPC server on the given channel.
func (f *Filesystem) Serve(c zx.Channel) error {
	// rpc.NewServer takes ownership of the Handle and will close it on error.
	_, err := rpc.NewServer(f, c)
	if err != nil {
		return fmt.Errorf("vfs server creation: %s", err)
	}
	f.mountInfo.serveChannel = c

	return nil
}

var _ fs.FileSystem = (*Filesystem)(nil)

// clean canonicalizes a path and returns a path that is relative to an assumed root.
// as a result of this cleaning operation, an open of '/' or '.' or '' all return ''.
// TODO(raggi): speed this up/reduce allocation overhead.
func clean(path string) string {
	return filepath.Clean("/" + path)[1:]
}

type mountInfo struct {
	unmountOnce  sync.Once
	serveChannel zx.Channel
	parentFd     int
}

func goErrToFSErr(err error) error {
	switch err {
	case nil:
		return nil
	// Explicitly catch and pass through any error coming from the fs package.
	case fs.ErrInvalidArgs, fs.ErrNotFound, fs.ErrAlreadyExists,
		fs.ErrPermission, fs.ErrReadOnly, fs.ErrNoSpace, fs.ErrNoSpace,
		fs.ErrFailedPrecondition, fs.ErrNotEmpty, fs.ErrNotOpen, fs.ErrNotAFile,
		fs.ErrNotADir, fs.ErrIsActive, fs.ErrUnmounted, fs.ErrEOF,
		fs.ErrNotSupported:
		return err
	case os.ErrInvalid:
		return fs.ErrInvalidArgs
	case os.ErrPermission:
		return fs.ErrPermission
	case os.ErrExist:
		return fs.ErrAlreadyExists
	case os.ErrNotExist:
		return fs.ErrNotFound
	case os.ErrClosed, io.ErrClosedPipe:
		return fs.ErrNotOpen
	case io.EOF, io.ErrUnexpectedEOF:
		return fs.ErrEOF
	}

	switch e := err.(type) {
	case *os.PathError:
		return goErrToFSErr(e.Err)
	case *zx.Error:
		return e
	}

	log.Printf("unmapped fs error type: %#v", err)
	return &zx.Error{Status: zx.ErrInternal, Text: err.Error()}
}

// GC examines the static and dynamic indexes, collects all the blobs that
// belong to packages in these indexes. It then reads blobfs for its entire
// list of blobs. Anything in blobfs that does not appear in the indexes is
// removed.
func (fs *Filesystem) GC() error {
	log.Println("GC: start")
	start := time.Now()
	defer func() {
		// this process produces a lot of garbage, so try to free that up (removes
		// ~1.5mb of heap from a common (small) build target).
		runtime.GC()
		log.Printf("GC: completed in %.3fs", time.Since(start).Seconds())
	}()

	// read the list of installed blobs first, as there may be installations in
	// progress, we want to not create orphans later, this list must be equal to or
	// a subset of the set we intersect.
	installedBlobNames, err := fs.blobfs.Blobs()
	if err != nil {
		return fmt.Errorf("GC: unable to list blobfs: %s", err)
	}
	installedBlobs := make(map[string]struct{}, len(installedBlobNames))
	for _, name := range installedBlobNames {
		installedBlobs[name] = struct{}{}
	}
	log.Printf("GC: %d blobs in blobfs", len(installedBlobs))

	allPackageBlobs := fs.index.AllPackageBlobs()
	// access the meta FAR blob of the system package
	if pd, ok := fs.root.dir("system").(*packageDir); ok {
		allPackageBlobs = append(allPackageBlobs, pd.contents["meta"].blobId)
	} else {
		return fmt.Errorf("GC: gc aborted, system directory is of unknown type")
	}

	// Walk the list of all packages and collate all involved blobs, both the fars
	// themselves, and the contents on which they depend.
	allBlobs := make(map[string]struct{})
	for _, pkgRoot := range allPackageBlobs {
		allBlobs[pkgRoot] = struct{}{}

		pDir, err := newPackageDirFromBlob(pkgRoot, fs, false)
		if err != nil {
			log.Printf("GC: failed getting package from blob %s: %s", pkgRoot, err)
			continue
		}

		for _, m := range pDir.Blobs() {
			allBlobs[m] = struct{}{}
		}
		pDir.Close()
	}

	log.Printf("GC: %d blobs referenced by %d packages", len(allBlobs), len(allPackageBlobs))

	for m := range allBlobs {
		delete(installedBlobs, m)
	}

	// remove all the blobs we no longer need
	log.Printf("GC: removing %d blobs from blobfs", len(installedBlobs))

	i := 0
	for m := range installedBlobs {
		i += 1
		e := os.Remove(path.Join("/blob", m))
		if e != nil {
			log.Printf("GC: error removing %s from blobfs: %s", m, e)
		}
		if i%100 == 0 {
			log.Printf("GC: deleted %d of %d blobs in %.3fs", i, len(installedBlobs), time.Since(start).Seconds())
		}
	}
	return nil
}

// ValidateStaticIndex compares the contents of the static index against what
// blobs are available in blobfs. It returns the ids of the blobs that are
// present and those that are missing or any error encountered trying to do the
// validation.
func (fs *Filesystem) ValidateStaticIndex() (map[string]struct{}, map[string]struct{}, error) {
	installedBlobNames, err := fs.blobfs.Blobs()
	if err != nil {
		return nil, nil, fmt.Errorf("pmd_validate: unable to list blobfs: %s", err)
	}
	installedBlobs := make(map[string]struct{}, len(installedBlobNames))
	for _, name := range installedBlobNames {
		installedBlobs[name] = struct{}{}
	}

	present := make(map[string]struct{})
	missing := make(map[string]struct{})
	staticPkgs := fs.static.StaticPackageBlobs()
	if pd, ok := fs.root.dir("system").(*packageDir); ok {
		staticPkgs = append(staticPkgs, pd.contents["meta"].blobId)
	}

	for _, pkgRoot := range staticPkgs {
		if _, ok := installedBlobs[pkgRoot]; ok {
			present[pkgRoot] = struct{}{}
		} else {
			log.Printf("pmd_validate: %q root is missing", pkgRoot)
			missing[pkgRoot] = struct{}{}
		}

		pDir, err := newPackageDirFromBlob(pkgRoot, fs, false)
		if err != nil {
			log.Printf("pmd_validate: failed getting package from blob %s: %s", pkgRoot, err)
			pDir.Close()
			return nil, nil, err
		}

		for _, m := range pDir.Blobs() {
			if _, ok := installedBlobs[m]; ok {
				present[m] = struct{}{}
			} else {
				log.Printf("pmd_validate: %q is missing from %q", m, pkgRoot)
				missing[m] = struct{}{}
			}
		}
		pDir.Close()
	}
	return present, missing, nil
}
