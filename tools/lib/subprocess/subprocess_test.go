// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package subprocess

import (
	"bytes"
	"context"
	"errors"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"go.fuchsia.dev/fuchsia/tools/lib/clock"
)

func TestRun(t *testing.T) {
	t.Run("Run", func(t *testing.T) {
		t.Run("should execute a command", func(t *testing.T) {
			r := Runner{
				Env: []string{"FOO=bar"}, // Cover env var handling.
			}
			message := "Hello, World!"
			command := []string{"echo", message}

			stdout := new(bytes.Buffer)
			stderr := new(bytes.Buffer)
			if err := r.Run(context.Background(), command, stdout, stderr); err != nil {
				t.Fatal(err)
			}

			stdoutS := strings.TrimSpace(stdout.String())
			if stdoutS != message {
				t.Fatalf("Expected output %q, but got %q", message, stdoutS)
			}

			stderrS := strings.TrimSpace(stderr.String())
			if stderrS != "" {
				t.Fatalf("Expected empty stderr, but got %q", stderrS)
			}
		})

		t.Run("should error if the context completes before the command", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			cancel()

			r := Runner{}
			command := []string{"sleep", "5"}
			err := r.Run(ctx, command, nil, nil)
			if err == nil {
				t.Fatal("Expected sleep command to terminate early but it completed")
			} else if !errors.Is(err, ctx.Err()) {
				t.Fatalf("Expected Run() to return a context error after cancelation but got: %s", err)
			}
		})

		t.Run("should return an error if the command fails", func(t *testing.T) {
			r := Runner{}
			command := []string{"not_a_command_12345"}
			err := r.Run(context.Background(), command, nil, nil)
			if err == nil {
				t.Fatalf("Expected invalid command to fail but it succeeded: %s", err)
			} else if !errors.Is(err, exec.ErrNotFound) {
				t.Fatalf("Expected Run() to return exec.ErrNotFound but got: %s", err)
			}
		})

		t.Run("should wait for command to finish after sending SIGTERM", func(t *testing.T) {
			script := filepath.Join(t.TempDir(), "script")
			// The script below will print `start` to signify that it's ready to handle
			// SIGTERMs and SIGINTs and then run cleanup() when it receives the signal.
			// By checking that `start` is printed before canceling the context and checking
			// that `finished` is printed after, we can assert that the cleanup() function
			// was run before the script exited.
			if err := ioutil.WriteFile(script, []byte(
				`#!/bin/bash
				cleanup() {
					echo "finished"; exit 1
				}
				trap cleanup TERM INT
				echo "start"
				while true;do :; done`,
			), os.ModePerm); err != nil {
				t.Fatalf("Failed to write script: %s", err)
			}
			r := Runner{}
			command := []string{script}
			stdoutReader, stdout := io.Pipe()
			defer stdoutReader.Close()
			defer stdout.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			go func() {
				buf := make([]byte, 20)
				// Wait for script to print `start` before calling cancel() to know
				// that it's ready to handle the SIGTERM.
				if _, err := stdoutReader.Read(buf); err != nil || !bytes.Contains(buf, []byte("start")) {
					t.Errorf("Failed to read `start` from stdout: %s, got: %s", err, string(buf))
				}
				cancel()
				// After sending the SIGTERM, check that the script ran cleanup() and
				// printed `finished`.
				buf = make([]byte, 20)
				if _, err := stdoutReader.Read(buf); err != nil || !bytes.Contains(buf, []byte("finished")) {
					t.Errorf("Failed to read `finished` from stdout: %s, got: %s", err, string(buf))
				}
			}()
			if err := r.Run(ctx, command, stdout, stdout); err == nil {
				t.Errorf("Expected script to terminate early but it completed successfully")
			} else {
				if !errors.Is(err, context.Canceled) {
					t.Errorf("Expected Run() to return context.Canceled but got: %s", err)
				}
			}
		})

		t.Run("should kill command if it doesn't terminate after sending SIGTERM", func(t *testing.T) {
			if runtime.GOOS == "darwin" {
				// Setting the pgid doesn't work on Mac OS, so this test
				// will hang because it can't kill the sleep process.
				// TODO(fxbug.dev/86162): Enable if we can find a way to kill
				// the child processes.
				t.Skip("Skipping on Mac because setting pgid doesn't work")
			}
			script := filepath.Join(t.TempDir(), "script")
			// The script below will print `start` to signify that it's ready to handle
			// SIGTERMs and SIGINTs and then run cleanup() when it receives the signal.
			// However, since it starts a `sleep` process, it actually waits for the process
			// to finish before entering cleanup(). This will test that the process group
			// gets killed if it can't clean up and exit in time. In this test, `finished`
			// should not be in the output because the process would have been killed before
			// it could run cleanup().
			if err := ioutil.WriteFile(script, []byte(
				`#!/bin/bash
                                cleanup() {
					echo "finished"; exit 1
                                }
                                trap cleanup TERM INT
                                echo "start"
				sleep 10000`,
			), os.ModePerm); err != nil {
				t.Fatalf("Failed to write script: %s", err)
			}
			r := Runner{}
			command := []string{script}
			stdoutReader, stdout := io.Pipe()
			defer stdoutReader.Close()
			defer stdout.Close()
			fakeClock := clock.NewFakeClock()
			ctx := clock.NewContext(context.Background(), fakeClock)
			ctx, cancel := context.WithCancel(ctx)
			defer cancel()
			go func() {
				buf := make([]byte, 20)
				if _, err := stdoutReader.Read(buf); err != nil || !bytes.Contains(buf, []byte("start")) {
					t.Errorf("Failed to read `start` from stdout: %s, got: %s", err, string(buf))
				}
				cancel()
				// Wait for After() to be called before advancing the clock.
				<-fakeClock.AfterCalledChan()
				fakeClock.Advance(cleanupGracePeriod + time.Second)
				// The script should be killed before it reaches cleanup(), so `finished`
				// should NOT have been printed to stdout. No need to check the err from
				// Read() because we don't expect the script to print anything more to
				// stdout, so it should block until the deferred stdout.Close() gets
				// executed.
				buf = make([]byte, 20)
				stdoutReader.Read(buf)
				if bytes.Contains(buf, []byte("finished")) {
					t.Errorf("Expected script to be killed without doing cleanup")
				}
			}()
			if err := r.RunWithStdin(ctx, command, stdout, stdout, nil); err == nil {
				t.Errorf("Expected script to terminate early but it completed successfully")
			} else {
				if !errors.Is(err, context.Canceled) {
					t.Errorf("Expected Run() to return context.Canceled but got: %s", err)
				}
			}
		})
	})
}
