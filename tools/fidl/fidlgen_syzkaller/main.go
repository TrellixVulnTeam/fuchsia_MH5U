// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"path"

	"go.fuchsia.dev/fuchsia/tools/fidl/fidlgen_syzkaller/codegen"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

type flagsDef struct {
	jsonPath   *string
	outputPath *string
}

var flags = flagsDef{
	jsonPath: flag.String("json", "",
		"relative path to the FIDL intermediate representation."),
	outputPath: flag.String("output-syz", "",
		"output path for the generated syz.txt file."),
}

// valid returns true if the parsed flags are valid.
func (f flagsDef) valid() bool {
	return *f.jsonPath != "" && *f.outputPath != ""
}

func printUsage() {
	program := path.Base(os.Args[0])
	message := `Usage: ` + program + ` [flags]

Syzkaller FIDL backend, used to generate Syzkaller bindings from JSON IR input
(the intermediate representation of a FIDL library).

Flags:
`
	fmt.Fprint(flag.CommandLine.Output(), message)
	flag.PrintDefaults()
}

func main() {
	flag.Usage = printUsage
	flag.Parse()
	if !flag.Parsed() || !flags.valid() {
		printUsage()
		os.Exit(1)
	}

	root, err := fidlgen.ReadJSONIr(*flags.jsonPath)
	if err != nil {
		log.Fatalf("Failed to read JSON: %v", err)
	}

	generator := codegen.NewGenerator()
	err = generator.GenerateSyscallDescription(*flags.outputPath, root)
	if err != nil {
		log.Fatalf("Failed to compile syzkaller description: %v", err)
	}
}
