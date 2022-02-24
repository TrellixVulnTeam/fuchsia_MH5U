// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package repo

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"testing"
	"time"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/lib/merkle"
)

type fakeTimeProvider struct {
	time int
}

func (f *fakeTimeProvider) UnixTimestamp() int {
	return f.time
}

type targetsFile struct {
	Signed struct {
		Targets map[string]struct {
			Custom struct {
				Merkle string `json:"merkle"`
			} `json:"custom"`
		} `json:"targets"`
	} `json:"signed"`
}

var roleJsons = []string{"root.json", "timestamp.json", "targets.json", "snapshot.json"}
var merklePat = regexp.MustCompile("^[0-9a-f]{64}$")

func TestInitRepoWithCreate_no_directory(t *testing.T) {
	repoDir := t.TempDir()
	// Remove the repository directory that tempdir just created, as we want to
	// check that a new one is created.
	if err := os.RemoveAll(repoDir); err != nil {
		t.Fatal(err)
	}
	r, err := New(repoDir, t.TempDir())
	if err != nil {
		t.Fatalf("Repo new returned error %v", err)
	}
	if err := r.OptionallyInitAtLocation(true); err != nil {
		t.Fatal(err)
	}

	for _, rolejson := range roleJsons {
		path := filepath.Join(repoDir, "keys", rolejson)
		if _, err := os.Stat(path); err != nil {
			t.Fatal(err)
		}
	}
}
func TestInitRepoWithCreate_existing_directory(t *testing.T) {
	repoDir := t.TempDir()
	// The tempdir is not removed, so the repo directory already exists but is
	// empty.
	r, err := New(repoDir, t.TempDir())
	if err != nil {
		t.Fatalf("Repo new returned error %v", err)
	}
	if err := r.OptionallyInitAtLocation(true); err != nil {
		t.Fatal(err)
	}

	for _, rolejson := range roleJsons {
		path := filepath.Join(repoDir, "keys", rolejson)
		if _, err := os.Stat(path); err != nil {
			t.Fatal(err)
		}
	}
}
func TestInitRepoWithCreate_file_at_target_path(t *testing.T) {
	repoPath := filepath.Join(t.TempDir(), "publish-test-repo")
	if err := ioutil.WriteFile(repoPath, []byte("foo"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := New(repoPath, t.TempDir()); err == nil {
		t.Fatal("expected error, did not get one")
	} else if !errors.Is(err, syscall.ENOTDIR) {
		t.Fatalf("unexpected error: %#v", err)
	}
}

func TestInitRepoNoCreate(t *testing.T) {
	repoDir := t.TempDir()
	r, err := New(repoDir, t.TempDir())
	if err != nil {
		t.Fatalf("Repo init returned error %v", err)
	}

	// With the false param, we _don't_ want to create this repository if
	// it doesn't already exist (which it doesn't, because there isn't a root.json).
	// Make sure we get the correct error.
	err = r.OptionallyInitAtLocation(false)
	if err == nil {
		// We actually want an error here.
		t.Fatal("repo did not exist but was possibly created")
	}

	if err != NotCreatingNonExistentRepoError {
		t.Fatalf("other init error: %v", err)
	}
}

func TestAddPackage(t *testing.T) {
	repoDir := t.TempDir()
	r, err := New(repoDir, t.TempDir())
	if err != nil {
		t.Fatalf("Repo init returned error %v", err)
	}
	if err := r.Init(); err != nil {
		t.Fatal(err)
	}
	for _, rolejson := range roleJsons {
		path := filepath.Join(repoDir, "keys", rolejson)
		if _, err := os.Stat(path); err != nil {
			t.Fatal(err)
		}
	}

	targetName := "test-test"
	err = r.AddPackage("test-test", io.LimitReader(rand.Reader, 8193), "")

	if err != nil {
		t.Fatalf("Problem adding repo file %v", err)
	}
	testVersion := 1
	timeProvider := fakeTimeProvider{testVersion}
	r.timeProvider = &timeProvider
	if err = r.CommitUpdates(true); err != nil {
		t.Fatalf("Failure commiting update %s", err)
	}

	// Check for rolejsons and consistent snapshots:
	for _, rolejson := range roleJsons {
		_, err := ioutil.ReadFile(filepath.Join(repoDir, "repository", rolejson))
		if err != nil {
			t.Fatal(err)
		}
		// timestamp doesn't get a consistent snapshot, as it is the entrypoint
		if rolejson == "timestamp.json" {
			continue
		}

		if rolejson == "root.json" {
			// Root version is 1 since we call GenKeys once.
			if _, err := os.Stat(filepath.Join(repoDir, "repository", "1.root.json")); err != nil {
				t.Fatal(err)
			}
		} else {
			if _, err := os.Stat(filepath.Join(repoDir, "repository", fmt.Sprintf("%d.%s", testVersion, rolejson))); err != nil {
				t.Fatal(err)
			}
		}
	}

	contents, err := os.Open(filepath.Join(repoDir, "repository", "targets"))
	if err != nil {
		t.Fatalf("Unable to read targets directory %v", err)
	}

	found := false
	contentList, err := contents.Readdir(0)
	if err != nil {
		t.Fatalf("Couldn't read targets directory")
	}

	for _, info := range contentList {
		n := info.Name()
		if strings.Contains(n, targetName) &&
			strings.LastIndex(n, targetName) == len(n)-len(targetName) {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("Didn't find expected file")
	}

	// do a basic sanity check that a merkle element is included in the
	// targets field
	targs, err := os.Open(filepath.Join(repoDir, "repository", "targets.json"))
	if err != nil {
		t.Fatalf("Couldn't open targets metadata %v", err)
	}
	defer targs.Close()

	var targets targetsFile
	decoder := json.NewDecoder(targs)
	err = decoder.Decode(&targets)
	if err != nil {
		t.Fatalf("Couldn't decode targets metadata %v", err)
	}

	if len(targets.Signed.Targets) == 0 {
		t.Fatalf("Targets file contains no targets")
	}

	for _, target := range targets.Signed.Targets {
		if !merklePat.MatchString(target.Custom.Merkle) {
			t.Fatalf("Targets JSON contains invalid merkle entry: %v", target.Custom.Merkle)
		}
	}
}

func TestAddBlob(t *testing.T) {
	repoDir := t.TempDir()
	blobsDir := t.TempDir()
	repo, err := New(repoDir, blobsDir)
	if err != nil {
		t.Fatalf("Repo init returned error %v", err)
	}

	res, n, err := repo.AddBlob("", io.LimitReader(rand.Reader, 8193))
	if err != nil {
		t.Fatal(err)
	}
	if want := int64(8193); n != want {
		t.Fatalf("got %d, want %d", n, want)
	}
	blobs, err := os.Open(blobsDir)
	if err != nil {
		t.Fatalf("Couldn't open blobs directory for reading %v", err)
	}
	files, err := blobs.Readdir(0)
	blobs.Close()
	if err != nil {
		t.Fatalf("Error reading blobs directory %v", err)
	}
	if len(files) != 1 {
		t.Fatalf("Unexpected number of blobs in blobs directory")
	}
	if res != files[0].Name() {
		t.Fatalf("computed merkle: %s, filename: %s", res, files[0].Name())
	}

	blobPath := filepath.Join(blobsDir, res)
	b, err := ioutil.ReadFile(blobPath)
	if err != nil {
		t.Fatal(err)
	}
	var tree merkle.Tree
	if _, err := tree.ReadFrom(bytes.NewReader(b)); err != nil {
		t.Fatal(err)
	}
	mr := hex.EncodeToString(tree.Root())
	if res != mr {
		t.Fatalf("got %s, want %s", res, mr)
	}

	if err := os.Remove(blobPath); err != nil {
		t.Fatal(err)
	}

	// Test adding a blob with a pre-computed name
	_, n, err = repo.AddBlob(mr, bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}

	if want := int64(len(b)); n != want {
		t.Fatalf("got %d, want %d", n, want)
	}

	if _, err := os.Stat(blobPath); err != nil {
		t.Fatal(err)
	}
}

func TestLinkOrCopy(t *testing.T) {
	tmpDir := t.TempDir()
	srcPath := filepath.Join(tmpDir, "source-file")

	srcContent := "I am a file"

	t.Run("hardlinking", func(t *testing.T) {
		dstPath := filepath.Join(t.TempDir(), "dest-file")
		if err := ioutil.WriteFile(srcPath, []byte(srcContent), 0o600); err != nil {
			t.Fatal(err)
		}

		if err := linkOrCopy(srcPath, dstPath); err != nil {
			t.Fatal(err)
		}

		dstBytes, err := ioutil.ReadFile(dstPath)
		if err != nil {
			t.Fatal(err)
		}

		if got, want := string(dstBytes), srcContent; got != want {
			t.Fatalf("post link content: got %q, want %q", got, want)
		}
	})

	t.Run("copying", func(t *testing.T) {
		dstPath := filepath.Join(t.TempDir(), "dest-file")
		var oldLink func(s, d string) error
		oldLink, link = link, func(s, d string) error { return &os.LinkError{"", "", "", fmt.Errorf("stub error")} }
		defer func() { link = oldLink }()

		if err := ioutil.WriteFile(srcPath, []byte(srcContent), 0o600); err != nil {
			t.Fatal(err)
		}

		if err := linkOrCopy(srcPath, dstPath); err != nil {
			t.Fatal(err)
		}

		dstBytes, err := ioutil.ReadFile(dstPath)
		if err != nil {
			t.Fatal(err)
		}

		if got, want := string(dstBytes), srcContent; got != want {
			t.Fatalf("post copy content: got %q, want %q", got, want)
		}
	})

}

func copyFile(src string, dest string) error {
	b, err := ioutil.ReadFile(src)
	if err != nil {
		return fmt.Errorf("ReadFile: failed to read file %s, err: %w", src, err)
	}
	if err := ioutil.WriteFile(dest, b, 0o600); err != nil {
		return fmt.Errorf("WriteFile: failed to write file %s, err: %w", dest, err)
	}
	return nil
}

func TestLoadExistingRepo(t *testing.T) {
	repoDir := t.TempDir()
	// Create a test repo.
	r, err := New(repoDir, t.TempDir())
	if err != nil {
		t.Fatalf("New: Repo init returned error: %v", err)
	}
	if err := r.Init(); err != nil {
		t.Fatalf("Init: failed to init repo: %v", err)
	}

	if err := r.AddTargets([]string{}, json.RawMessage{}); err != nil {
		t.Fatalf("AddTargets, failed to add empty target: %v", err)
	}

	testVersion := 1
	timeProvider := fakeTimeProvider{testVersion}
	r.timeProvider = &timeProvider
	if err = r.CommitUpdates(true); err != nil {
		t.Fatalf("CommitUpdates: failed to commit updates: %v", err)
	}

	newRepoDir := t.TempDir()
	if err := os.Mkdir(filepath.Join(newRepoDir, "repository"), 0o700); err != nil {
		t.Fatalf("Couldn't create test repo directory.")
	}
	if err := os.Mkdir(filepath.Join(newRepoDir, "keys"), 0o700); err != nil {
		t.Fatalf("Couldn't create test keys directory.")
	}
	defer os.RemoveAll(newRepoDir)
	// Copy the metadata and keys to a new test folder.
	for _, rolejson := range roleJsons {
		if err := copyFile(filepath.Join(repoDir, "repository", rolejson), filepath.Join(newRepoDir, "repository", rolejson)); err != nil {
			t.Fatalf("copyFile: failed to copy file: %v", err)
		}

		if err := copyFile(filepath.Join(repoDir, "keys", rolejson), filepath.Join(newRepoDir, "keys", rolejson)); err != nil {
			t.Fatalf("copyFile: failed to copy file: %v", err)
		}
	}

	if err := copyFile(filepath.Join(repoDir, "repository", "1.root.json"), filepath.Join(newRepoDir, "repository", "1.root.json")); err != nil {
		t.Fatalf("copyFile: failed to copy file: %v", err)
	}

	// Initiate a new repo using the folder containing existing metadata and keys.
	r, err = New(newRepoDir, t.TempDir())
	if err != nil {
		t.Fatalf("New: failed to init new repo using existing metadata files: %v", err)
	}
	if err := r.Init(); err != os.ErrExist {
		t.Fatalf("Init: expect to return os.ErrExist when the repo already exists, got %v", err)
	}
	if err := r.AddTargets([]string{}, json.RawMessage{}); err != nil {
		t.Fatalf("AddTargets, failed to add empty target: %v", err)
	}
	testVersion = 2
	timeProvider = fakeTimeProvider{testVersion}
	r.timeProvider = &timeProvider
	if err = r.CommitUpdates(true); err != nil {
		t.Fatalf("CommitUpdates: failed to commit updates: %v", err)
	}

	// Check for rolejsons and consistent snapshots:
	for _, rolejson := range roleJsons {
		bytes, err := ioutil.ReadFile(filepath.Join(newRepoDir, "repository", rolejson))
		if err != nil {
			t.Fatal(err)
		}

		// Check metadata has a UTC timestamp, and no nanoseconds.
		var meta struct {
			Signed struct {
				Expires time.Time `json:"expires"`
			} `json:"signed"`
		}
		if err := json.Unmarshal(bytes, &meta); err != nil {
			t.Fatal(err)
		}
		zone, offset := meta.Signed.Expires.Zone()
		if zone != "UTC" || offset != 0 {
			t.Fatalf("%s expires field is not UTC: %s", rolejson, meta.Signed.Expires)
		}
		if meta.Signed.Expires.Nanosecond() != 0 {
			t.Fatalf("%s expires should not have nanoseconds: %s", rolejson, meta.Signed.Expires)
		}

		// timestamp doesn't get a consistent snapshot, as it is the entrypoint
		if rolejson == "timestamp.json" {
			continue
		}

		if rolejson == "root.json" {
			// Root version is 1 since we call GenKeys once.
			if _, err := os.Stat(filepath.Join(newRepoDir, "repository", "1.root.json")); err != nil {
				t.Fatal(err)
			}
		} else {
			if _, err := os.Stat(filepath.Join(newRepoDir, "repository", fmt.Sprintf("%d.%s", testVersion, rolejson))); err != nil {
				t.Fatal(err)
			}
		}
	}
}
