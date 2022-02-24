// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/ed25519"
	"crypto/md5"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"hash"
	"io"
	"io/ioutil"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"cloud.google.com/go/storage"
	"github.com/google/subcommands"
	"google.golang.org/api/googleapi"

	"go.fuchsia.dev/fuchsia/tools/artifactory"
	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/lib/gcsutil"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

const (
	// The exit code emitted by the `up` command when it fails due to a
	// transient GCS error.
	exitTransientError = 3

	// The size in bytes at which files will be read and written to GCS.
	chunkSize = 100 * 1024 * 1024

	// Relative path within the build directory to the repo produced by a build.
	repoSubpath = "amber-files"
	// Names of the repository metadata, key, blob, and target directories within a repo.
	metadataDirName = "repository"
	keyDirName      = "keys"
	blobDirName     = "blobs"
	targetDirName   = "targets"

	// Names of directories to be uploaded to in GCS.
	assemblyInputArchivesDirName = "assembly"
	buildsDirName                = "builds"
	buildAPIDirName              = "build_api"
	buildidDirName               = "buildid"
	debugDirName                 = "debug"
	hostTestDirName              = "host_tests"
	imageDirName                 = "images"
	packageDirName               = "packages"
	sdkArchivesDirName           = "sdk"
	toolDirName                  = "tools"

	// A record of all of the fuchsia debug symbols processed.
	// This is eventually consumed by crash reporting infrastructure.
	// TODO(fxbug.dev/75356): Have the crash reporting infrastructure
	// consume build-ids.json instead.
	buildIDsTxt = "build-ids.txt"

	// A mapping of build ids to binary labels.
	buildIDsToLabelsManifestName = "build-ids.json"

	// The blobs manifest. TODO(fxbug.dev/60322) remove this.
	blobManifestName = "blobs.json"

	// A list of all Public Platform Surface Areas.
	ctsPlasaReportName = "test_coverage_report.plasa.json"

	// A list of the objects that need their TTL refreshed.
	objsToRefreshTTLTxt = "objs_to_refresh_ttl.txt"

	// The number of days since the CustomTime of an object after which the
	// TTL should be refreshed. This is an arbitrary value chosen to avoid
	// refreshing the TTL of frequently deduplicated objects repeatedly over
	// a short time period.
	daysSinceCustomTime = 10

	// The ELF sizes manifest.
	elfSizesManifestName = "elf_sizes.json"

	// A mapping of fidl mangled names to api functions.
	fidlMangledToApiMappingManifestName = "fidl_mangled_to_api_mapping.json"

	// Timeout for every file upload.
	perFileUploadTimeout = 8 * time.Minute

	// Metadata keys.
	googleReservedFileMtime = "goog-reserved-file-mtime"
)

type upCommand struct {
	// GCS bucket to which build artifacts will be uploaded.
	gcsBucket string
	// Unique namespace under which to index artifacts.
	namespace string
	// The maximum number of concurrent uploading routines.
	j int
	// An ED25519 private key encoded in the PKCS8 PEM format.
	privateKeyPath string
	// Whether or not to upload host tests.
	uploadHostTests bool
}

func (upCommand) Name() string { return "up" }

func (upCommand) Synopsis() string { return "upload artifacts from a build to Google Cloud Storage" }

func (upCommand) Usage() string {
	return `
artifactory up -bucket $GCS_BUCKET -namespace $NAMESPACE <build directory>

Uploads artifacts from a build to $GCS_BUCKET with the following structure:

├── $GCS_BUCKET
│   │   ├── assembly
│   │   │   └── <assembly input archives>
│   │   ├── blobs
│   │   │   └── <blob names>
│   │   ├── debug
│   │   │   └── <debug binaries in zxdb format>
│   │   ├── buildid
│   │   │   └── <debug binaries in debuginfod format>
│   │   ├── builds
│   │   │   ├── $NAMESPACE
│   │   │   │   ├── build-ids.json
│   │   │   │   ├── build-ids.txt
│   │   │   │   ├── jiri.snapshot
│   │   │   │   ├── objs_to_refresh_ttl.txt
│   │   │   │   ├── publickey.pem
│   │   │   │   ├── images
│   │   │   │   │   └── <images>
│   │   │   │   ├── packages
│   │   │   │   │   ├── all_blobs.json
│   │   │   │   │   ├── blobs.json
│   │   │   │   │   ├── elf_sizes.json
│   │   │   │   │   ├── repository
│   │   │   │   │   │   ├── targets
│   │   │   │   │   │   │   └── <package repo target files>
│   │   │   │   │   │   └── <package repo metadata files>
│   │   │   │   │   └── keys
│   │   │   │   │       └── <package repo keys>
│   │   │   │   ├── sdk
│   │   │   │   │   ├── <host-independent SDK archives>
│   │   │   │   │   └── <OS-CPU>
│   │   │   │   │       └── <host-specific SDK archives>
│   │   │   │   ├── build_api
│   │   │   │   │   └── <build API module JSON>
|   |   |   |   ├── host_tests
│   │   │   │   │   └── <host tests and deps, same hierarchy as build dir>
│   │   │   │   ├── tools
│   │   │   │   │   └── <OS>-<CPU>
│   │   │   │   │       └── <tool names>

flags:

`
}

func (cmd *upCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&cmd.gcsBucket, "bucket", "", "GCS bucket to which artifacts will be uploaded")
	f.StringVar(&cmd.namespace, "namespace", "", "Namespace under which to index uploaded artifacts")
	f.IntVar(&cmd.j, "j", 32, "maximum number of concurrent uploading processes")
	f.StringVar(&cmd.privateKeyPath, "pkey", "", "The path to an ED25519 private key encoded in the PKCS8 PEM format.\n"+
		"This can, for example, be generated by \"openssl genpkey -algorithm ed25519\".\n"+
		"If set, all images and build APIs will be signed and uploaded with their signatures\n"+
		"in GCS metadata, and the corresponding public key will be uploaded as well.")
	f.BoolVar(&cmd.uploadHostTests, "upload-host-tests", false, "whether or not to upload host tests and their runtime deps.")
}

func isTransientError(err error) bool {
	_, transient := err.(gcsutil.TransientError)
	var apiErr *googleapi.Error
	return transient || (errors.As(err, &apiErr) && apiErr.Code >= 500)
}

func (cmd upCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	args := f.Args()
	if len(args) != 1 {
		logger.Errorf(ctx, "exactly one positional argument expected: the build directory root")
		return subcommands.ExitFailure
	}

	if err := cmd.execute(ctx, args[0]); err != nil {
		logger.Errorf(ctx, "%v", err)
		// Use a different exit code if the failure is a (likely transient)
		// server error so the infrastructure knows to consider it as an infra
		// failure.
		if isTransientError(err) || errors.Is(err, context.DeadlineExceeded) {
			return exitTransientError
		}
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}

func (cmd upCommand) execute(ctx context.Context, buildDir string) error {
	if cmd.gcsBucket == "" {
		return fmt.Errorf("-bucket is required")
	}
	if cmd.namespace == "" {
		return fmt.Errorf("-namespace is required")
	}

	m, err := build.NewModules(buildDir)
	if err != nil {
		return err
	}

	sink, err := newCloudSink(ctx, cmd.gcsBucket)
	if err != nil {
		return err
	}
	defer sink.client.Close()

	repo := path.Join(buildDir, repoSubpath)
	metadataDir := path.Join(repo, metadataDirName)
	keyDir := path.Join(repo, keyDirName)
	blobDir := path.Join(metadataDir, blobDirName)
	targetDir := path.Join(metadataDir, targetDirName)
	buildsNamespaceDir := path.Join(buildsDirName, cmd.namespace)
	packageNamespaceDir := path.Join(buildsNamespaceDir, packageDirName)
	imageNamespaceDir := path.Join(buildsNamespaceDir, imageDirName)

	dirs := []artifactory.Upload{
		{
			Source:      blobDir,
			Destination: blobDirName,
			Deduplicate: true,
		},
		{
			Source:      metadataDir,
			Destination: path.Join(packageNamespaceDir, metadataDirName),
			Deduplicate: false,
		},
		{
			Source:      keyDir,
			Destination: path.Join(packageNamespaceDir, keyDirName),
			Deduplicate: false,
		},
		{
			Source:      targetDir,
			Destination: path.Join(packageNamespaceDir, metadataDirName, targetDirName),
			Deduplicate: false,
			Recursive:   true,
		},
	}

	files := []artifactory.Upload{
		{
			Source:      path.Join(buildDir, blobManifestName),
			Destination: path.Join(packageNamespaceDir, blobManifestName),
		},
		{
			Source:      path.Join(buildDir, elfSizesManifestName),
			Destination: path.Join(packageNamespaceDir, elfSizesManifestName),
		},
		// Used for CTS test coverage.
		{
			Source:      path.Join(buildDir, fidlMangledToApiMappingManifestName),
			Destination: path.Join(buildsNamespaceDir, fidlMangledToApiMappingManifestName),
		},
		{
			Source:      path.Join(buildDir, ctsPlasaReportName),
			Destination: path.Join(buildsNamespaceDir, ctsPlasaReportName),
		},
	}

	allBlobsUpload, err := artifactory.BlobsUpload(m, path.Join(packageNamespaceDir, "all_blobs.json"))
	if err != nil {
		return fmt.Errorf("failed to obtain blobs upload: %w", err)
	}
	files = append(files, allBlobsUpload)

	pkey, err := artifactory.PrivateKey(cmd.privateKeyPath)
	if err != nil {
		return fmt.Errorf("failed to get private key: %w", err)
	}

	// Sign the images for release builds.
	images, err := artifactory.ImageUploads(m, imageNamespaceDir)
	if err != nil {
		return err
	}
	if pkey != nil {
		images, err = artifactory.Sign(images, pkey)
		if err != nil {
			return err
		}
	}
	files = append(files, images...)

	productBundle, err := artifactory.ProductBundleUploads(m, packageNamespaceDir, blobDirName, imageNamespaceDir)
	if err != nil {
		return err
	}
	// Check that an upload isn't nil as product bundle doesn't exist for "bringup" and SDK builds.
	if productBundle != nil {
		files = append(files, *productBundle)
	}

	// Sign the build_info.json for release builds.
	buildAPIs := artifactory.BuildAPIModuleUploads(m, path.Join(buildsNamespaceDir, buildAPIDirName))
	if pkey != nil {
		buildAPIs, err = artifactory.Sign(buildAPIs, pkey)
		if err != nil {
			return err
		}
	}
	files = append(files, buildAPIs...)

	assemblyInputArchives := artifactory.AssemblyInputArchiveUploads(m, path.Join(buildsNamespaceDir, assemblyInputArchivesDirName))
	files = append(files, assemblyInputArchives...)

	sdkArchives := artifactory.SDKArchiveUploads(m, path.Join(buildsNamespaceDir, sdkArchivesDirName))
	files = append(files, sdkArchives...)

	// Sign the tools for release builds.
	tools := artifactory.ToolUploads(m, path.Join(buildsNamespaceDir, toolDirName))
	if pkey != nil {
		tools, err = artifactory.Sign(tools, pkey)
		if err != nil {
			return err
		}
	}
	files = append(files, tools...)

	debugBinaries, buildIDsToLabels, buildIDs, err := artifactory.DebugBinaryUploads(ctx, m, debugDirName, buildidDirName)
	if err != nil {
		return err
	}
	files = append(files, debugBinaries...)
	buildIDManifest, err := createBuildIDManifest(buildIDs)
	if err != nil {
		return err
	}
	defer os.Remove(buildIDManifest)
	files = append(files, artifactory.Upload{
		Source:      buildIDManifest,
		Destination: path.Join(buildsNamespaceDir, buildIDsTxt),
	})
	buildIDsToLabelsJSON, err := json.MarshalIndent(buildIDsToLabels, "", "  ")
	if err != nil {
		return err
	}
	files = append(files, artifactory.Upload{
		Contents:    buildIDsToLabelsJSON,
		Destination: path.Join(buildsNamespaceDir, buildIDsToLabelsManifestName),
	})

	snapshot, err := artifactory.JiriSnapshotUpload(m, buildsNamespaceDir)
	if err != nil {
		return err
	}
	files = append(files, *snapshot)

	if pkey != nil {
		publicKey, err := artifactory.PublicKeyUpload(buildsNamespaceDir, pkey.Public().(ed25519.PublicKey))
		if err != nil {
			return err
		}
		files = append(files, *publicKey)
	}

	if cmd.uploadHostTests {
		hostTests, err := artifactory.HostTestUploads(m.TestSpecs(), m.BuildDir(), path.Join(buildsNamespaceDir, hostTestDirName))
		if err != nil {
			return fmt.Errorf("failed to get host test files: %v", err)
		}
		files = append(files, hostTests...)
	}

	for _, dir := range dirs {
		contents, err := dirToFiles(ctx, dir)
		if err != nil {
			return err
		}
		files = append(files, contents...)
	}
	return uploadFiles(ctx, files, sink, cmd.j, buildsNamespaceDir)
}

func createBuildIDManifest(buildIDs []string) (string, error) {
	manifest, err := ioutil.TempFile("", buildIDsTxt)
	if err != nil {
		return "", err
	}
	defer manifest.Close()
	_, err = io.WriteString(manifest, strings.Join(buildIDs, "\n"))
	return manifest.Name(), err
}

// DataSink is an abstract data sink, providing a mockable interface to
// cloudSink, the GCS-backed implementation below.
type dataSink interface {

	// ObjectExistsAt returns whether an object of that name exists within the sink.
	objectExistsAt(ctx context.Context, name string) (bool, *storage.ObjectAttrs, error)

	// Write writes the content of a reader to a sink object at the given name.
	// If an object at that name does not exists, it will be created; else it
	// will be overwritten.
	write(ctx context.Context, upload *artifactory.Upload) error
}

// CloudSink is a GCS-backed data sink.
type cloudSink struct {
	client *storage.Client
	bucket *storage.BucketHandle
}

func newCloudSink(ctx context.Context, bucket string) (*cloudSink, error) {
	client, err := storage.NewClient(ctx)
	if err != nil {
		return nil, err
	}
	return &cloudSink{
		client: client,
		bucket: client.Bucket(bucket),
	}, nil
}

func (s *cloudSink) objectExistsAt(ctx context.Context, name string) (bool, *storage.ObjectAttrs, error) {
	attrs, err := gcsutil.ObjectAttrs(ctx, s.bucket.Object(name))
	if err != nil {
		if errors.Is(err, storage.ErrObjectNotExist) {
			return false, nil, nil
		}
		return false, nil, err
	}
	// Check if MD5 is not set, mark this as a miss, then write() function will
	// handle the race.
	return len(attrs.MD5) != 0, attrs, nil
}

// hasher is a io.Writer that calculates the MD5.
type hasher struct {
	h hash.Hash
	w io.Writer
}

func (h *hasher) Write(p []byte) (int, error) {
	n, err := h.w.Write(p)
	_, _ = h.h.Write(p[:n])
	return n, err
}

func (s *cloudSink) write(ctx context.Context, upload *artifactory.Upload) error {
	var reader io.Reader
	if upload.Source != "" {
		f, err := os.Open(upload.Source)
		if err != nil {
			return err
		}
		defer f.Close()
		reader = f
	} else {
		reader = bytes.NewBuffer(upload.Contents)
	}
	obj := s.bucket.Object(upload.Destination)
	// Set timeouts to fail fast on unresponsive connections.
	tctx, cancel := context.WithTimeout(ctx, perFileUploadTimeout)
	defer cancel()
	sw := obj.If(storage.Conditions{DoesNotExist: true}).NewWriter(tctx)
	sw.ChunkSize = chunkSize
	sw.ContentType = "application/octet-stream"
	if upload.Compress {
		sw.ContentEncoding = "gzip"
	}
	if upload.Metadata != nil {
		sw.Metadata = upload.Metadata
	}
	// The CustomTime needs to be set to work with the lifecycle condition
	// set on the GCS bucket.
	sw.CustomTime = time.Now()

	// We optionally compress on the fly, and calculate the MD5 on the
	// compressed data.
	// Writes happen asynchronously, and so a nil may be returned while the write
	// goes on to fail. It is recommended in
	// https://godoc.org/cloud.google.com/go/storage#Writer.Write
	// to return the value of Close() to detect the success of the write.
	// Note that a gzip compressor would need to be closed before the storage
	// writer that it wraps is.
	h := &hasher{md5.New(), sw}
	var writeErr, tarErr, zipErr error
	if upload.Compress {
		gzw := gzip.NewWriter(h)
		if upload.TarHeader != nil {
			tw := tar.NewWriter(gzw)
			writeErr = tw.WriteHeader(upload.TarHeader)
			if writeErr == nil {
				_, writeErr = io.Copy(tw, reader)
			}
			tarErr = tw.Close()
		} else {
			_, writeErr = io.Copy(gzw, reader)
		}
		zipErr = gzw.Close()
	} else {
		_, writeErr = io.Copy(h, reader)
	}
	closeErr := sw.Close()

	// Keep the first error we encountered - and vet it for 'permissable' GCS
	// error states.
	// Note: consider an errorsmisc.FirstNonNil() helper if see this logic again.
	err := writeErr
	if err == nil {
		err = tarErr
	}
	if err == nil {
		err = zipErr
	}
	if err == nil {
		err = closeErr
	}
	if err = checkGCSErr(ctx, err, upload.Destination); err != nil {
		return err
	}

	// Now confirm that the MD5 matches upstream, just in case. If the file was
	// uploaded by another client (a race condition), loop until the MD5 is set.
	// This guarantees that the file is properly uploaded before this function
	// quits.
	d := h.h.Sum(nil)
	t := time.Second
	const max = 30 * time.Second
	for {
		attrs, err := gcsutil.ObjectAttrs(ctx, obj)
		if err != nil {
			return fmt.Errorf("failed to confirm MD5 for %s due to: %w", upload.Destination, err)
		}
		if len(attrs.MD5) == 0 {
			time.Sleep(t)
			if t += t / 2; t > max {
				t = max
			}
			logger.Debugf(ctx, "waiting for MD5 for %s", upload.Destination)
			continue
		}
		if !bytes.Equal(attrs.MD5, d) {
			return fmt.Errorf("MD5 mismatch for %s; local: %x, remote: %x", upload.Destination, d, attrs.MD5)
		}
		logger.Infof(ctx, "Uploaded: %s", upload.Destination)
		break
	}
	return nil
}

// checkGCSErr validates the error for a GCS upload.
//
// If the precondition of the object not existing is not met on write (i.e.,
// at the time of the write the object is there), then the server will
// respond with a 412. (See
// https://cloud.google.com/storage/docs/json_api/v1/status-codes and
// https://tools.ietf.org/html/rfc7232#section-4.2.)
// We do not report this as an error, however, as the associated object might
// have been created after having checked its non-existence - and we wish to
// be resilient in the event of such a race.
func checkGCSErr(ctx context.Context, err error, name string) error {
	if err == nil || err == io.EOF {
		return nil
	}
	if strings.Contains(err.Error(), "Error 412") {
		logger.Infof(ctx, "object %q: created after its non-existence check", name)
		return nil
	}
	return err
}

// dirToFiles returns a list of the top-level files in the dir if dir.Recursive
// is false, else it returns all files in the dir.
func dirToFiles(ctx context.Context, dir artifactory.Upload) ([]artifactory.Upload, error) {
	var files []artifactory.Upload
	var err error
	var paths []string
	if dir.Recursive {
		err = filepath.Walk(dir.Source, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if !info.IsDir() {
				relPath, err := filepath.Rel(dir.Source, path)
				if err != nil {
					return err
				}
				paths = append(paths, relPath)
			}
			return nil
		})
	} else {
		var entries []os.FileInfo
		entries, err = ioutil.ReadDir(dir.Source)
		if err == nil {
			for _, fi := range entries {
				if fi.IsDir() {
					continue
				}
				paths = append(paths, fi.Name())
			}
		}
	}
	if os.IsNotExist(err) {
		logger.Debugf(ctx, "%s does not exist; skipping upload", dir.Source)
		return nil, nil
	} else if err != nil {
		return nil, err
	}
	for _, path := range paths {
		files = append(files, artifactory.Upload{
			Source:      filepath.Join(dir.Source, path),
			Destination: filepath.Join(dir.Destination, path),
			Deduplicate: dir.Deduplicate,
		})
	}
	return files, nil
}

func uploadFiles(ctx context.Context, files []artifactory.Upload, dest dataSink, j int, buildsNamespaceDir string) error {
	if j <= 0 {
		return fmt.Errorf("Concurrency factor j must be a positive number")
	}

	uploads := make(chan artifactory.Upload, j)
	errs := make(chan error, j)

	queueUploads := func() {
		defer close(uploads)
		for _, f := range files {
			if len(f.Source) != 0 {
				fileInfo, err := os.Stat(f.Source)
				if err != nil {
					// The associated artifacts might not actually have been created, which is valid.
					if os.IsNotExist(err) {
						logger.Infof(ctx, "%s does not exist; skipping upload", f.Source)
						continue
					}
					errs <- err
					return
				}
				mtime := strconv.FormatInt(fileInfo.ModTime().Unix(), 10)
				if f.Metadata == nil {
					f.Metadata = map[string]string{}
				}
				f.Metadata[googleReservedFileMtime] = mtime
			}
			uploads <- f
		}
	}

	objsToRefreshTTL := make(chan string)
	var wg sync.WaitGroup
	wg.Add(j)
	upload := func() {
		defer wg.Done()
		for upload := range uploads {
			exists, attrs, err := dest.objectExistsAt(ctx, upload.Destination)
			if err != nil {
				errs <- err
				return
			}
			if exists {
				logger.Debugf(ctx, "object %q: already exists remotely", upload.Destination)
				if !upload.Deduplicate {
					errs <- fmt.Errorf("object %q: collided", upload.Destination)
					return
				}
				// Add objects to update timestamps for that are older than daysSinceCustomTime.
				if attrs != nil && time.Now().AddDate(0, 0, -daysSinceCustomTime).After(attrs.CustomTime) {
					objsToRefreshTTL <- upload.Destination
				}
				continue
			}

			if err := uploadFile(ctx, upload, dest); err != nil {
				errs <- err
				return
			}
		}
	}

	go queueUploads()
	for i := 0; i < j; i++ {
		go upload()
	}
	go func() {
		wg.Wait()
		close(errs)
		close(objsToRefreshTTL)
	}()
	var objs []string
	for o := range objsToRefreshTTL {
		objs = append(objs, o)
	}

	if err := <-errs; err != nil {
		return err
	}
	// Upload a file listing all the deduplicated files that already existed in the
	// upload destination. A post-processor will use this file to update the CustomTime
	// of the objects and extend their TTL.
	if len(objs) > 0 {
		objsToRefreshTTLUpload := artifactory.Upload{
			Contents:    []byte(strings.Join(objs, "\n")),
			Destination: path.Join(buildsNamespaceDir, objsToRefreshTTLTxt),
		}
		return uploadFile(ctx, objsToRefreshTTLUpload, dest)
	}
	return nil
}

func uploadFile(ctx context.Context, upload artifactory.Upload, dest dataSink) error {
	logger.Debugf(ctx, "object %q: attempting creation", upload.Destination)
	if err := gcsutil.Retry(ctx, func() error {
		err := dest.write(ctx, &upload)
		if err != nil {
			logger.Warningf(ctx, "error uploading %q: %s", upload.Destination, err)
		}
		return err
	}); err != nil {
		return fmt.Errorf("%s: %w", upload.Destination, err)
	}
	logger.Debugf(ctx, "object %q: created", upload.Destination)
	return nil
}
