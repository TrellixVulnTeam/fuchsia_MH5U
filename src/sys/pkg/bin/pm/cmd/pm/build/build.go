// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package build contains the `pm build` command
package build

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/build"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/cmd/pm/seal"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/cmd/pm/update"
)

const usage = `Usage: %s build
perform update and seal in order
`

func Run(cfg *build.Config, args []string) error {
	fs := flag.NewFlagSet("build", flag.ExitOnError)

	var depfile = fs.Bool("depfile", true, "Produce a depfile")
	var pkgManifestPath = fs.String("output-package-manifest", "", "If set, produce a package manifest at the given path")
	var blobsfile = fs.Bool("blobsfile", false, "Produce blobs.json file")
	var blobsmani = fs.Bool("blobs-manifest", false, "Produce blobs.manifest file")

	fs.Usage = func() {
		fmt.Fprintf(os.Stderr, usage, filepath.Base(os.Args[0]))
		fmt.Fprintln(os.Stderr)
		fs.PrintDefaults()
	}

	if err := fs.Parse(args); err != nil {
		return err
	}

	if len(fs.Args()) != 0 {
		fmt.Fprintf(os.Stderr, "WARNING: unused arguments: %s\n", fs.Args())
	}

	if err := update.Run(cfg, []string{}); err != nil {
		return fmt.Errorf("failed to update the merkle roots: %s", err)
	}

	if err := seal.Run(cfg, []string{}); err != nil {
		return fmt.Errorf("failed to seal the package: %s", err)
	}

	if *depfile {
		if cfg.ManifestPath == "" {
			return fmt.Errorf("the -depfile option requires the use of the -m manifest option")
		}

		content, err := buildDepfile(cfg)
		if err != nil {
			return fmt.Errorf("failed to build dep file: %s", err)
		}
		if err := ioutil.WriteFile(cfg.MetaFAR()+".d", content, 0644); err != nil {
			return err
		}
	}

	if cfg.ManifestPath == "" {
		return fmt.Errorf("the -blobsfile option requires the use of the -m manifest option")
	}

	blobs, err := cfg.BlobInfo()
	if err != nil {
		return err
	}

	if *blobsfile {
		content, err := json.MarshalIndent(blobs, "", "    ")
		if err != nil {
			return err
		}
		if err := ioutil.WriteFile(filepath.Join(cfg.OutputDir, "blobs.json"), content, 0644); err != nil {
			return err
		}
	}

	if *blobsmani {
		var buf bytes.Buffer
		for _, blob := range blobs {
			fmt.Fprintf(&buf, "%s=%s\n", blob.Merkle.String(), blob.SourcePath)
		}
		if err := ioutil.WriteFile(filepath.Join(cfg.OutputDir, "blobs.manifest"), buf.Bytes(), 0644); err != nil {
			return err
		}
	}

	if *pkgManifestPath != "" {
		pkgManifest, err := cfg.OutputManifest()
		if err != nil {
			return err
		}
		content, err := json.MarshalIndent(pkgManifest, "", "    ")
		if err != nil {
			return err
		}
		if err := ioutil.WriteFile(*pkgManifestPath, content, 0644); err != nil {
			return err
		}
	}

	return nil
}

// computedOutputs are files that are produced by the `build` composite command
// that must be excluded from the depfile
var computedOutputs = map[string]struct{}{
	"meta/contents": {},
}

// buildDepfile computes and returns the contents of a ninja compatible depfile
// for meta.far for the composite `build` action.
func buildDepfile(cfg *build.Config) ([]byte, error) {
	manifest, err := cfg.Manifest()
	if err != nil {
		return nil, err
	}

	var buf bytes.Buffer

	if _, err := io.WriteString(&buf, cfg.MetaFAR()+":"); err != nil {
		return nil, err
	}

	for dst, src := range manifest.Paths {
		// see computedOutputs
		if _, ok := computedOutputs[dst]; ok {
			continue
		}

		// Spaces are separators, so spaces in filenames must be
		// escaped.
		src := strings.ReplaceAll(src, " ", "\\ ")

		if _, err := io.WriteString(&buf, " "+src); err != nil {
			return nil, err
		}
	}

	if _, err := io.WriteString(&buf, " "+cfg.ManifestPath); err != nil {
		return nil, err
	}

	return buf.Bytes(), nil
}
