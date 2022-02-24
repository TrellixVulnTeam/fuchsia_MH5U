// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// whereiscl is a command-line utility that answers "Where is my CL?".
// Given a Gerrit review URL, it will answer
//   - Whether the CL was merged (or abandoned)
//   - Whether the CL passed Global Integration (if merged)
package main

import (
	"flag"
	"fmt"
	"log"
	"os"

	"go.fuchsia.dev/fuchsia/tools/whereiscl"
)

var gitRevision = flag.String("rev", "", "git revision (anything that 'git rev-parse' can parse)")

func init() {
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), `Usage: whereiscl [<review URL>|--rev <git revision>]

Answers whether a CL is merged (or abandoned) and whether it passed Global Integration.
Review URL can be various review URL forms as below. It can also be a raw CL number, a Change-Id, or a git commit hash.

Examples:
  $ whereiscl https://fuchsia-review.googlesource.com/c/fuchsia/+/123456789
  $ whereiscl fuchsia-review.googlesource.com/c/fuchsia/+/123456789/some/file
  $ whereiscl https://fuchsia-review.googlesource.com/c/fuchsia/+/Ie8dddbce1eeb01a561f3b36e1685f4136fb61378
  $ whereiscl http://fxr/123456789
  $ whereiscl http://fxr/Ie8dddbce1eeb01a561f3b36e1685f4136fb61378
  $ whereiscl fxr/123456789/some/file
  $ whereiscl 123456789
  $ whereiscl Ie8dddbce1eeb01a561f3b36e1685f4136fb61378
  $ whereiscl cd83861

`)
		flag.PrintDefaults()
	}
}

func validateArgs() {
	if *gitRevision == "" && flag.NArg() == 0 {
		// TODO: Consider alternatives. E.g., show all outstanding CLs
		// of the current user, or show all CLs that are pending in
		// Global Integration.
		flag.Usage()
		os.Exit(1)
	}
	if *gitRevision != "" && flag.NArg() > 0 {
		log.Fatal("Either --rev or a raw argument must be provided, but not both")
	}
}

func main() {
	flag.Parse()
	validateArgs()

	var queryInfo *whereiscl.QueryInfo
	var err error
	if *gitRevision == "" {
		queryInfo, err = whereiscl.ParseReviewURL(flag.Arg(0))
		if err != nil {
			log.Fatalf("Error parsing query: %v", err)
		}
	} else {
		queryInfo, err = whereiscl.ParseGitRevision(*gitRevision)
		if err != nil {
			log.Fatalf("Error parsing git revision: %v", err)
		}
	}

	ci, err := whereiscl.GetChangeInfo(queryInfo)
	if err != nil {
		log.Fatalf("Error getting change info: %v", err)
	}
	fmt.Printf("https://fuchsia-review.googlesource.com/c/%v/+/%v\n\n", ci.Project, ci.Number)
	fmt.Printf("CL status: %v\n", ci.Status)

	if ci.Status != whereiscl.CLStatusMerged {
		return
	}

	gs, err := whereiscl.GetGIStatus(ci)
	if err != nil {
		log.Fatalf("Error getting GI status: %v", err)
	}
	fmt.Printf("GI status: %v\n", gs)
}
