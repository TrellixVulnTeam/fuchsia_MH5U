// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fuzz

import (
	"fmt"
	"io"
)

type mockLauncher struct {
	running           bool
	shouldFailToStart bool
	shouldExitEarly   bool

	// Failure modes that will be passed on to the Connector
	shouldFailToConnectCount uint
	shouldFailToExecuteCount uint
}

func (l *mockLauncher) Prepare() error {
	return nil
}

func (l *mockLauncher) Start() (Connector, error) {
	if l.running {
		return nil, fmt.Errorf("Start called on already-running Launcher")
	}

	if l.shouldFailToStart {
		return nil, fmt.Errorf("Intentionally broken Launcher")
	}

	l.Prepare()
	l.running = !l.shouldExitEarly

	return &mockConnector{shouldFailToConnectCount: l.shouldFailToConnectCount,
		shouldFailToExecuteCount: l.shouldFailToExecuteCount}, nil
}

func (l *mockLauncher) IsRunning() (bool, error) {
	return l.running, nil
}

func (l *mockLauncher) Kill() error {
	if !l.running {
		return fmt.Errorf("Kill called on stopped Launcher")
	}
	l.running = false
	return nil
}

func (l *mockLauncher) GetLogs(out io.Writer) error {
	io.WriteString(out, "system log\n")
	return nil
}
