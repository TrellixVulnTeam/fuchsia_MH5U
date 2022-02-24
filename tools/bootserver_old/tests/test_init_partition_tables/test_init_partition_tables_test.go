// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"testing"

	bootservertest "go.fuchsia.dev/fuchsia/tools/bootserver_old/tests"
)

func TestInitPartitionTables(t *testing.T) {
	_ = bootservertest.StartQemu(t, []string{"netsvc.all-features=true", "netsvc.netboot=true"}, "full")

	logPattern := []bootservertest.LogMatch{
		{Pattern: "Received request from ", ShouldMatch: true},
		{Pattern: "Proceeding with nodename ", ShouldMatch: true},
		{Pattern: "Transfer starts", ShouldMatch: true},
		{Pattern: "Transfer ends successfully", ShouldMatch: true},
		{Pattern: "Issued reboot command to", ShouldMatch: false},
	}

	bootservertest.CmdSearchLog(
		t, logPattern,
		bootservertest.ToolPath(t, "bootserver"),
		"-n", bootservertest.DefaultNodename,
		"--init-partition-tables", "/dev/class/block/000",
		"-1",
		"--fail-fast")
}
