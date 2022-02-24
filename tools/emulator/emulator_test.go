// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package emulator

import (
	"bufio"
	"fmt"
	"strings"
	"testing"
)

func TestCheckForLogMessage(t *testing.T) {
	logLines := []string{"Some message", "Another message", "First message we're looking for", "Another message", "Second message we're looking for"}
	testStr := strings.Join(logLines, "\n")
	// attach a newline, because the reader expects this
	testStr = fmt.Sprintf("%s\n", testStr)
	fakeReader := bufio.NewReader(strings.NewReader(testStr))
	i := Instance{}
	if err := i.checkForLogMessage(fakeReader, logLines[2]); err != nil {
		t.Fatal(err)
	}

	fakeReader = bufio.NewReader(strings.NewReader(testStr))

	i = Instance{stdout: fakeReader}
	if err := i.WaitForLogMessage(logLines[2]); err != nil {
		t.Fatal(err)
	}

	fakeReader = bufio.NewReader(strings.NewReader(testStr))

	i = Instance{stdout: fakeReader}
	if err := i.WaitForLogMessages([]string{logLines[2]}); err != nil {
		t.Fatal(err)
	}

	fakeReader = bufio.NewReader(strings.NewReader(testStr))

	i = Instance{stdout: fakeReader}
	if err := i.WaitForLogMessages([]string{logLines[2], logLines[4]}); err != nil {
		t.Fatal(err)
	}

	fakeReader = bufio.NewReader(strings.NewReader(testStr))

	i = Instance{stdout: fakeReader}
	if err := i.WaitForLogMessages([]string{logLines[4], logLines[2]}); err != nil {
		t.Fatal(err)
	}
}
