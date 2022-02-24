// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"testing"

	bootservertest "go.fuchsia.dev/fuchsia/tools/bootserver_old/tests"
)

func TestWriteFirmwareNoType(t *testing.T) {
	instance := bootservertest.StartQemu(t, []string{"netsvc.all-features=true", "netsvc.netboot=true"}, "full")

	logPattern := []bootservertest.LogMatch{
		{Pattern: "Received request from ", ShouldMatch: true},
		{Pattern: "Proceeding with nodename ", ShouldMatch: true},
		{Pattern: "Transfer starts", ShouldMatch: true},
		{Pattern: "Transfer ends successfully", ShouldMatch: true},
		{Pattern: "Issued reboot command to", ShouldMatch: true},
	}

	bootservertest.CmdSearchLog(
		t, logPattern,
		bootservertest.ToolPath(t, "bootserver"),
		"-n", bootservertest.DefaultNodename,
		"--firmware", bootservertest.ToolPath(t, "fake_firmware"),
		"-1",
		"--fail-fast")

	instance.WaitForLogMessage("netsvc: Running FIRMWARE Paver (firmware type '')")
}
