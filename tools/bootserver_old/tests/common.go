// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package bootservertest includes common code to write smoke tests for
// bootserver_old.
package bootservertest

import (
	"bufio"
	"context"
	"flag"
	"io"
	"io/ioutil"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"
	"time"

	"go.fuchsia.dev/fuchsia/tools/emulator"
	"go.fuchsia.dev/fuchsia/tools/emulator/emulatortest"
	fvdpb "go.fuchsia.dev/fuchsia/tools/virtual_device/proto"
)

var hostDir = map[string]string{"arm64": "host_arm64", "amd64": "host_x64"}[runtime.GOARCH]

func testDataDir() string {
	base := filepath.Join("..", "..", "..", "..")
	c, err := ioutil.ReadFile(filepath.Join(base, ".fx-build-dir"))
	if err != nil {
		return ""
	}
	return filepath.Join(base, strings.TrimSpace(string(c)), hostDir, "test_data")
}

// TestDataDir is the location to test data files.
//
//  - In "go test" mode, it's relative to this directory.
//  - In "fx test" mode, it's relative to the build directory (out/default).
var TestDataDir = flag.String("test_data_dir", testDataDir(), "Path to test_data/; only used in GN build")

// DefaultNodename is the default nodename given to an target with the default
// QEMU MAC address.
const DefaultNodename = "swarm-donut-petri-acre"

// ToolPath returns the full path to a tool.
func ToolPath(t *testing.T, name string) string {
	p, err := filepath.Abs(filepath.Join(*TestDataDir, "bootserver_tools", name))
	if err != nil {
		t.Fatal(err)
	}
	return p
}

// CmdWithOutput returns a command and returns stdout.
func CmdWithOutput(t *testing.T, name string, arg ...string) []byte {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, name, arg...)
	out, err := cmd.Output()
	if err != nil {
		t.Errorf("%s failed %s, err=%s", name, out, err)
		return nil
	}
	return out
}

// LogMatch is one pattern to search for.
type LogMatch struct {
	Pattern     string
	ShouldMatch bool
}

func matchPattern(t *testing.T, pattern string, reader *bufio.Reader) bool {
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				t.Logf("matchPattern(%q): EOF", pattern)
				return false
			}
			t.Fatal(err)
		}
		if strings.Contains(line, pattern) {
			t.Logf("matchPattern(%q): true", pattern)
			return true
		}
		t.Logf("matchPattern(%q): ignored %q", pattern, line)
	}
}

// CmdSearchLog searches for a patterns in stderr.
func CmdSearchLog(t *testing.T, logPatterns []LogMatch, name string, arg ...string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, name, arg...)
	cmderr, err := cmd.StderrPipe()
	if err != nil {
		t.Errorf("Failed to stderr %s", err)
	}
	readerErr := bufio.NewReader(cmderr)
	if err := cmd.Start(); err != nil {
		t.Errorf("Failed to start %s", err)
	}

	found := false
	for _, logPattern := range logPatterns {
		match := matchPattern(t, logPattern.Pattern, readerErr)
		if match != logPattern.ShouldMatch {
			found = false
			t.Errorf("Log pattern \"%s\" mismatch. Expected - %t, actual - %t",
				logPattern.Pattern, logPattern.ShouldMatch, match)
			break
		}
		found = true
	}

	if err := cmd.Wait(); err != nil {
		t.Logf("Failed to wait on task %s", err)
	}

	if ctx.Err() == context.DeadlineExceeded {
		t.Errorf("%s timed out err=%s", name, ctx.Err())
	} else if !found {
		t.Errorf("%s failed to match logs", name)
	} else {
		t.Logf("%s worked as expected", name)
	}
}

// AttemptPaveNoBind attempts to initiate a pave.
func AttemptPaveNoBind(t *testing.T, shouldWork bool) {
	// Get the node ipv6 address.
	out := CmdWithOutput(t, ToolPath(t, "netls"))
	// Extract the ipv6 from the netls output
	regexString := DefaultNodename + ` \((?P<ipv6>.*)\)`
	match := regexp.MustCompile(regexString).FindStringSubmatch(string(out))
	if len(match) != 2 {
		t.Errorf("Node %s not found in netls output - %s", DefaultNodename, out)
		return
	}

	var logPattern []LogMatch
	if shouldWork {
		paveWorksPattern := []LogMatch{
			{"Sending request to ", true},
			{"Received request from ", true},
			{"Proceeding with nodename ", true},
			{"Transfer starts", true},
		}
		logPattern = paveWorksPattern
	} else {
		paveFailsPattern := []LogMatch{
			{"Sending request to ", true},
			{"Received request from ", false},
			{"Proceeding with nodename ", false},
			{"Transfer starts", false},
		}
		logPattern = paveFailsPattern
	}

	CmdSearchLog(
		t, logPattern,
		ToolPath(t, "bootserver"), "--fvm", "\"dummy.blk\"",
		"--no-bind", "-a", match[1], "-1", "--fail-fast")
}

// StartQemu starts a QEMU instance with the given kernel commandline args.
func StartQemu(t *testing.T, appendCmdline []string, modeString string) *emulatortest.Instance {
	if testing.Short() {
		t.Skip("skipping test in short mode.")
	}
	distro := emulatortest.UnpackFrom(t, *TestDataDir, emulator.DistributionParams{
		Emulator: emulator.Qemu,
	})
	arch := distro.TargetCPU()
	device := emulator.DefaultVirtualDevice(string(arch))
	device.KernelArgs = append(device.KernelArgs, appendCmdline...)
	device.KernelArgs = append(device.KernelArgs, "devmgr.log-to-debuglog=true", "zircon.nodename="+DefaultNodename)

	// To run locally on linux, you need to configure a tuntap interface:
	// $ sudo ip add mode tap qemu user `whoami`
	// $ sudo ip link set qemu up
	device.Hw.NetworkDevices = append(device.Hw.NetworkDevices, &fvdpb.Netdev{
		Id:     "qemu",
		Kind:   "tap",
		Device: &fvdpb.Device{Model: "virtio-net-pci"},
	})
	instance := distro.Create(device)
	instance.Start()

	// Make sure netsvc in expected mode.
	instance.WaitForLogMessage("netsvc: running in " + modeString + " mode")

	// Make sure netsvc is booted.
	instance.WaitForLogMessage("netsvc: start")
	return instance
}
