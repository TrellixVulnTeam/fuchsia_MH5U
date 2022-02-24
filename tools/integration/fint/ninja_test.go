// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fint

import (
	"context"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
	"google.golang.org/protobuf/testing/protocmp"

	"go.fuchsia.dev/fuchsia/tools/build"
	fintpb "go.fuchsia.dev/fuchsia/tools/integration/fint/proto"
	"go.fuchsia.dev/fuchsia/tools/lib/streams"
)

func TestRunNinja(t *testing.T) {
	ctx := context.Background()
	// Don't emit the mock ninja logs to stdio while running tests, because
	// they're noisy.
	ctx = streams.ContextWithStdout(ctx, io.Discard)
	ctx = streams.ContextWithStderr(ctx, io.Discard)

	testCases := []struct {
		name string
		// Whether to mock a non-zero ninja retcode, in which case we should get
		// an error.
		fail bool
		// Mock Ninja stdout.
		stdout                 string
		expectedActionData     *fintpb.NinjaActionMetrics
		expectedFailureMessage string
	}{
		{
			name: "success",
			stdout: `
                [1/2] ACTION a.o
                [2/2] ACTION b.o
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 2,
				FinalActions:   2,
				ActionsByType: map[string]int32{
					"ACTION": 2,
				},
			},
		},
		{
			name: "success with regenerating ninja files",
			stdout: `
                [0/1] Regenerating ninja files
                [1/3] ACTION a.o
                [2/3] ACTION b.o
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 3,
				FinalActions:   3,
				ActionsByType: map[string]int32{
					"ACTION": 2,
				},
			},
		},
		{
			name: "multiple action types",
			stdout: `
                [1/3] CXX a.o
                [2/3] RUST crab.rlib
                [3/3] RUST lobster.rlib
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 3,
				FinalActions:   3,
				ActionsByType: map[string]int32{
					"CXX":  1,
					"RUST": 2,
				},
			},
		},
		{
			name: "restat decreasing action counts",
			stdout: `
                [1/300] CXX a.o
                [2/200] RUST crab.rlib
                [3/100] RUST lobster.rlib
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 300,
				FinalActions:   100,
				ActionsByType: map[string]int32{
					"CXX":  1,
					"RUST": 2,
				},
			},
		},
		{
			name: "single failed target",
			fail: true,
			stdout: `
                [35792/53672] CXX a.o b.o
                [35793/53672] CXX c.o d.o
                FAILED: c.o d.o
                output line 1
                output line 2
                [35794/53672] CXX successful/e.o
                [35795/53672] CXX f.o
            `,
			expectedFailureMessage: `
                [35793/53672] CXX c.o d.o
                FAILED: c.o d.o
                output line 1
                output line 2
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 53672,
				FinalActions:   53672,
				ActionsByType: map[string]int32{
					"CXX": 4,
				},
			},
		},
		{
			name: "single failed target decreasing action counts",
			fail: true,
			stdout: `
                [35792/53672] CXX a.o b.o
                [35793/53672] CXX c.o d.o
                FAILED: c.o d.o
                output line 1
                output line 2
                [35794/45678] CXX successful/e.o
                [35795/45678] CXX f.o
            `,
			expectedFailureMessage: `
                [35793/53672] CXX c.o d.o
                FAILED: c.o d.o
                output line 1
                output line 2
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 53672,
				FinalActions:   45678,
				ActionsByType: map[string]int32{
					"CXX": 4,
				},
			},
		},
		{
			name: "preserves indentation",
			fail: true,
			stdout: `
                [35793/53672] CXX a.o b.o
                FAILED: a.o b.o
                    output line 1
                        output line 2
                            output line 3
                [35794/53672] CXX successful/c.o
            `,
			expectedFailureMessage: `
                [35793/53672] CXX a.o b.o
                FAILED: a.o b.o
                    output line 1
                        output line 2
                            output line 3
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 53672,
				FinalActions:   53672,
				ActionsByType: map[string]int32{
					"CXX": 2,
				},
			},
		},
		{
			name: "multiple failed targets",
			fail: true,
			stdout: `
                [35790/53672] CXX foo
                [35791/53672] CXX a.o b.o
                FAILED: a.o b.o
                output line 1
                output line 2
                [35792/53672] CXX c.o d.o
                [35793/53672] CXX e.o
                FAILED: e.o
                output line 3
                output line 4
                [35794/53672] CXX f.o
            `,
			expectedFailureMessage: `
                [35791/53672] CXX a.o b.o
                FAILED: a.o b.o
                output line 1
                output line 2
                [35793/53672] CXX e.o
                FAILED: e.o
                output line 3
                output line 4
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 53672,
				FinalActions:   53672,
				ActionsByType: map[string]int32{
					"CXX": 5,
				},
			},
		},
		{
			name: "last target fails",
			fail: true,
			stdout: `
                [35790/53672] CXX foo
                [35791/53672] CXX a.o b.o
                FAILED: a.o b.o
                output line 1
                output line 2
                ninja: build stopped: subcommand failed.
            `,
			expectedFailureMessage: `
                [35791/53672] CXX a.o b.o
                FAILED: a.o b.o
                output line 1
                output line 2
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 53672,
				FinalActions:   53672,
				ActionsByType: map[string]int32{
					"CXX": 2,
				},
			},
		},
		{
			name: "gn gen fails",
			fail: true,
			stdout: `
				ninja: Entering directory '/usr/me/fuchsia/out/default'
				[0/1] Regenerating ninja files
				ERROR at //src/foo/BUILD.gn:41:5: Can't load input file.
					"//src/bar:tests",
					^----------------
				Unable to load:
				/usr/me/fuchsia/src/bar/BUILD.gn
				FAILED: build.ninja
				../../prebuilt/third_party/gn/linux-x64/gn --root=../.. gen .
				ninja: error: rebuilding 'build.ninja': subcommand failed
			`,
			expectedFailureMessage: `
				[0/1] Regenerating ninja files
				ERROR at //src/foo/BUILD.gn:41:5: Can't load input file.
					"//src/bar:tests",
					^----------------
				Unable to load:
				/usr/me/fuchsia/src/bar/BUILD.gn
				FAILED: build.ninja
				../../prebuilt/third_party/gn/linux-x64/gn --root=../.. gen .
				ninja: error: rebuilding 'build.ninja': subcommand failed
            `,
		},
		{
			name: "graph error",
			fail: true,
			stdout: `
				ninja: Entering directory /foo
				ninja: error: bar.ninja: multiple rules generate baz
            `,
			expectedFailureMessage: `
				ninja: error: bar.ninja: multiple rules generate baz
            `,
		},
		{
			name: "fatal error",
			fail: true,
			stdout: `
				ninja: Entering directory /foo
				[1/1] ACTION //foo
				ninja: fatal: cannot create file foo
            `,
			expectedFailureMessage: `
				ninja: fatal: cannot create file foo
            `,
			expectedActionData: &fintpb.NinjaActionMetrics{
				InitialActions: 1,
				FinalActions:   1,
				ActionsByType: map[string]int32{
					"ACTION": 1,
				},
			},
		},
		{
			name: "unrecognized failure",
			fail: true,
			stdout: `
				ninja: Entering directory /foo
				...something went wrong...
            `,
			expectedFailureMessage: unrecognizedFailureMsg,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			tc.stdout = normalize(tc.stdout)
			tc.expectedFailureMessage = normalize(tc.expectedFailureMessage)

			sr := &fakeSubprocessRunner{
				mockStdout: []byte(tc.stdout),
				fail:       tc.fail,
			}
			r := ninjaRunner{
				runner:    sr,
				ninjaPath: filepath.Join(t.TempDir(), "ninja"),
				buildDir:  filepath.Join(t.TempDir(), "out"),
				jobCount:  23, // Arbitrary but distinctive value.
			}
			msg, gotActionData, err := runNinja(ctx, r, []string{"foo", "bar"}, false, nil)
			if tc.fail {
				if !errors.Is(err, errSubprocessFailure) {
					t.Fatalf("Expected a subprocess failure error but got: %s", err)
				}
			} else if err != nil {
				t.Fatalf("Unexpected error: %s", err)
			}

			if len(sr.commandsRun) != 1 {
				t.Fatalf("Expected runNinja to run 1 command but got %d", len(sr.commandsRun))
			}
			cmd := sr.commandsRun[0]
			if cmd[0] != r.ninjaPath {
				t.Fatalf("runNinja ran wrong executable %q (expected %q)", cmd[0], r.ninjaPath)
			}
			foundJobCount := false
			for i, part := range cmd {
				if part == "-j" {
					foundJobCount = true
					if i+1 >= len(cmd) || cmd[i+1] != fmt.Sprintf("%d", r.jobCount) {
						t.Errorf("Wrong value for -j flag: %v", cmd)
					}
				}
			}
			if !foundJobCount {
				t.Errorf("runNinja didn't set the -j flag. Full command: %v", cmd)
			}

			if diff := cmp.Diff(tc.expectedFailureMessage, msg); diff != "" {
				t.Errorf("Unexpected failure message diff (-want +got):\n%s", diff)
			}
			if diff := cmp.Diff(tc.expectedActionData, gotActionData, protocmp.Transform()); diff != "" {
				t.Errorf("Unexpected action data diff (-want, +got):\n%s", diff)
			}
		})
	}
}

func TestRunWithNinjaExplain(t *testing.T) {
	gotStdout := new(strings.Builder)
	ctx := streams.ContextWithStdout(context.Background(), gotStdout)

	sr := &fakeSubprocessRunner{
		mockStdout: []byte(`ninja: Entering directory /foo
[1/1] ACTION //foo
ninja explain: obj/build/foo is dirty`),
	}
	r := ninjaRunner{
		runner:    sr,
		ninjaPath: filepath.Join(t.TempDir(), "ninja"),
		buildDir:  filepath.Join(t.TempDir(), "out"),
		jobCount:  32, // Arbitrary but distinctive value.
	}

	explainSink := new(strings.Builder)

	if _, _, err := runNinja(ctx, r, []string{"foo", "bar"}, true /* explain */, explainSink); err != nil {
		t.Fatalf("runNinja failed: %s", err)
	}

	wantExplainOutput := "ninja explain: obj/build/foo is dirty\n"
	if diff := cmp.Diff(wantExplainOutput, explainSink.String()); diff != "" {
		t.Errorf("Unexpected explain output diff (-want +got):\n%s", diff)
	}

	wantStdout := `ninja: Entering directory /foo
[1/1] ACTION //foo
`
	if diff := cmp.Diff(wantStdout, gotStdout.String()); diff != "" {
		t.Errorf("Unexpected stdout output diff (-want +got):\n%s", diff)
	}
}

// normalize removes a leading newline and trailing spaces from a multiline
// string, ensuring that the expected failure message has the same whitespace
// formatting as failure messages emitted by runNinja.
func normalize(s string) string {
	s = strings.TrimLeft(s, "\n")
	s = strings.TrimRight(s, " ")
	return s
}

func TestCheckNinjaNoop(t *testing.T) {
	testCases := []struct {
		name       string
		isMac      bool
		stdout     string
		fail       bool
		expectNoop bool
		expectErr  bool
	}{
		{
			name:       "no-op",
			stdout:     "ninja: Entering directory /foo\nninja: no work to do.",
			expectNoop: true,
		},
		{
			name:      "ninja fails",
			stdout:    "ninja: Entering directory /foo\nninja: no work to do.",
			fail:      true,
			expectErr: true,
		},
		{
			name:       "dirty",
			stdout:     "ninja: Entering directory /foo\n[1/1] STAMP foo.stamp",
			expectNoop: false,
		},
		{
			name:       "mac dirty",
			isMac:      true,
			stdout:     "ninja: Entering directory /foo\n[1/1] STAMP foo.stamp",
			expectNoop: false,
		},
		{
			name:       "broken mac path",
			isMac:      true,
			stdout:     "ninja: Entering directory /foo\nninja explain: ../../../../usr/bin/env is dirty",
			expectNoop: true,
		},
		{
			name:       "ninja fails on mac, with known broken mac path",
			isMac:      true,
			fail:       true,
			stdout:     "ninja: Entering directory /foo\nninja explain: ../../../../usr/bin/env is dirty",
			expectNoop: true,
		},
		{
			name:      "ninja fails on mac, no broken path",
			isMac:     true,
			fail:      true,
			stdout:    "ninja: Entering directory /foo\n[1/1] STAMP foo.stamp",
			expectErr: true,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			r := ninjaRunner{
				runner: &fakeSubprocessRunner{
					mockStdout: []byte(tc.stdout),
					fail:       tc.fail,
				},
				ninjaPath: "ninja",
				buildDir:  t.TempDir(),
			}
			noop, logFiles, err := checkNinjaNoop(context.Background(), r, []string{"foo"}, tc.isMac)
			if err != nil {
				if !tc.expectErr {
					t.Fatal(err)
				}
				return
			} else if tc.expectErr {
				t.Fatalf("Expected an error but got nil")
			}
			if noop != tc.expectNoop {
				t.Fatalf("Unexpected ninja no-op result: got %t, expected %t", noop, tc.expectNoop)
			}
			if tc.expectNoop {
				if len(logFiles) > 0 {
					t.Errorf("Expected no log files in case of no-op, but got: %+v", logFiles)
				}
			} else if len(logFiles) != 2 {
				t.Errorf("Expected 2 log files in case of non-no-op, but got: %+v", logFiles)
			}
		})
	}
}

func TestDirtyLineForPackage(t *testing.T) {
	testCases := []struct {
		name      string
		input     string
		want      string
		expectErr bool
	}{
		{
			name:  "implicit basename",
			input: "//foo/bar",
			want:  "ninja explain: obj/foo/bar/meta/contents is dirty",
		},
		{
			name:  "toolchain suffix",
			input: "//foo/bar(//toolchain)",
			want:  "ninja explain: obj/foo/bar/meta/contents is dirty",
		},
		{
			name:  "explicit basename",
			input: "//foo/bar:baz",
			want:  "ninja explain: obj/foo/bar/baz/meta/contents is dirty",
		},
		{
			name:  "explicit basename with toolchain",
			input: "//foo/bar:baz(//toolchain)",
			want:  "ninja explain: obj/foo/bar/baz/meta/contents is dirty",
		},
		{
			name:  "explicit basename same as directory",
			input: "//foo/bar:bar",
			want:  "ninja explain: obj/foo/bar/meta/contents is dirty",
		},
		{
			// This can happen if there's a bug in the tests.json generation
			// template.
			name:      "double colon in label",
			input:     "//foo/bar::bar",
			expectErr: true,
		},
	}
	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := dirtyLineForPackageLabel(tc.input)
			if err != nil && !tc.expectErr {
				t.Fatal(err)
			} else if tc.expectErr {
				if err == nil {
					t.Fatal("Expected error but got none")
				}
				return
			}
			if diff := cmp.Diff(tc.want, got); diff != "" {
				t.Errorf("Wrong dirty line for test (-want +got):\n%s", diff)
			}
		})
	}
}

func TestAffectedTestsNoWork(t *testing.T) {
	mockTestManifest := []build.Test{
		{Name: "host_test", Path: "host/run.sh"},
		{Name: "fuchsia_test", PackageLabel: "//src/path/to:fuchsia_test(//toolchain)"},
		{Name: "unaffected_test", PackageLabel: "//src/path/to:unaffected_test(//toolchain)"},
		{Name: "never_affected_test", PackageLabel: neverAffectedTestLabels[0] + "(//toolchain)"},
	}

	testCases := []struct {
		name                  string
		ninjaOutput           string
		affectedFiles         []string
		expectedAffectedTests []string
		expectedNoWork        bool
		expectedDryRuns       int
	}{
		{
			name:                  "unaffected",
			ninjaOutput:           "ninja: entering directory /foo" + noWorkString,
			affectedFiles:         []string{"foo.go"},
			expectedAffectedTests: nil,
			expectedNoWork:        true,
		},
		{
			name: "affected tests",
			ninjaOutput: `
ninja: entering directory /foo
ninja explain: obj/src/path/to/fuchsia_test/meta/contents is dirty
ninja explain: obj/another_test/meta/contents is dirty
[3/3] python build.py host/run.sh
            `,
			expectedAffectedTests: []string{"host_test", "fuchsia_test"},
			expectedNoWork:        false,
		},
		{
			name: "affected GN files",
			ninjaOutput: `
ninja: entering directory /foo
ninja explain: obj/src/path/to/fuchsia_test/meta/contents is dirty
ninja explain: obj/another_test/meta/contents is dirty
[3/3] python build.py host/run.sh
			`,
			affectedFiles:         []string{"foo.gn", "bar.gni", "foo.go"},
			expectedAffectedTests: []string{"host_test", "fuchsia_test"},
			expectedNoWork:        false,
			// If GN files are affected, the first dry run should not touch them
			// but the second one should.
			expectedDryRuns: 2,
		},
	}

	oneMinuteAgo := time.Now().Add(-1 * time.Minute)
	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			// Apply default values and otherwise transform the test parameters
			// to make tests easier to specify.
			if tc.expectedDryRuns == 0 {
				tc.expectedDryRuns = 1
			}
			sort.Strings(tc.expectedAffectedTests)
			checkoutDir := t.TempDir()

			subprocessRunner := fakeSubprocessRunner{
				mockStdout: []byte(tc.ninjaOutput),
			}
			r := ninjaRunner{
				runner:    &subprocessRunner,
				ninjaPath: "ninja",
				buildDir:  filepath.Join(checkoutDir, "build"),
			}

			var affectedFilesAbs []string
			for _, path := range tc.affectedFiles {
				affectedFilesAbs = append(affectedFilesAbs, filepath.Join(checkoutDir, path))
			}
			for _, path := range affectedFilesAbs {
				if _, err := os.Create(path); err != nil {
					t.Fatal(err)
				}
				if err := os.Chtimes(path, oneMinuteAgo, oneMinuteAgo); err != nil {
					t.Fatal(err)
				}
			}

			targets := []string{"foo", "bar"}
			result, err := affectedTestsNoWork(context.Background(), r, mockTestManifest, affectedFilesAbs, targets)
			if err != nil {
				t.Fatal(err)
			}

			if len(result.logs) == 0 {
				t.Errorf("Expected logs to contain at least one log, but got none")
			}

			if len(subprocessRunner.commandsRun) != tc.expectedDryRuns {
				t.Errorf("Expected %d dry run(s) but got %d", tc.expectedDryRuns, len(subprocessRunner.commandsRun))
			}
			if diff := cmp.Diff(tc.expectedAffectedTests, result.affectedTests); diff != "" {
				t.Errorf("Unexpected affected tests diff (-want +got):\n%s", diff)
			}
			if tc.expectedNoWork != result.noWork {
				t.Errorf("Wrong no work result, wanted %v, got %v", tc.expectedNoWork, result.noWork)
			}

			// Ensure file timestamps weren't modified
			for _, path := range affectedFilesAbs {
				stat, err := os.Stat(path)
				if err != nil {
					t.Fatal(err)
				}
				if !stat.ModTime().Equal(oneMinuteAgo) {
					t.Errorf("Unexpected %v modified time, wanted %v, got %v", path, oneMinuteAgo, stat.ModTime())
				}
			}
		})
	}
}

func TestTouchFiles(t *testing.T) {
	oneMinuteAgo := time.Now().Add(-1 * time.Minute)
	dir := t.TempDir()
	exists1 := filepath.Join(dir, "exists1")
	if _, err := os.Create(exists1); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(exists1, oneMinuteAgo, oneMinuteAgo); err != nil {
		t.Fatal(err)
	}
	exists2 := filepath.Join(dir, "exists2")
	if _, err := os.Create(exists2); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(exists2, oneMinuteAgo, oneMinuteAgo); err != nil {
		t.Fatal(err)
	}
	doesntExist := filepath.Join(dir, "doesntExist")

	resetMap, err := touchFiles([]string{exists1, exists2, doesntExist})
	if err != nil {
		t.Fatal(err)
	}

	stat, err := os.Stat(exists1)
	if err != nil {
		t.Fatal(err)
	}
	if !stat.ModTime().After(oneMinuteAgo) {
		t.Errorf("Did not touch exists1, ModTime was %v", stat.ModTime())
	}
	stat, err = os.Stat(exists2)
	if err != nil {
		t.Fatal(err)
	}
	if !stat.ModTime().After(oneMinuteAgo) {
		t.Errorf("Did not touch exists2, ModTime was %v", stat.ModTime())
	}
	if _, err := os.Stat(doesntExist); !os.IsNotExist(err) {
		t.Errorf("Touched non-existent file doesntExist or other error %v", err)
	}

	if err := resetTouchFiles(resetMap); err != nil {
		t.Fatal(err)
	}
	stat, err = os.Stat(exists1)
	if err != nil {
		t.Fatal(err)
	}
	if !stat.ModTime().Equal(oneMinuteAgo) {
		t.Errorf("Did not reset exists1, ModTime was %v, wanted %v", stat.ModTime(), oneMinuteAgo)
	}
	stat, err = os.Stat(exists2)
	if err != nil {
		t.Fatal(err)
	}
	if !stat.ModTime().Equal(oneMinuteAgo) {
		t.Errorf("Did not reset exists2, ModTime was %v, wanted %v", stat.ModTime(), oneMinuteAgo)
	}
	if _, err := os.Stat(doesntExist); !os.IsNotExist(err) {
		t.Errorf("Reset non-existent file doesntExist or other error %v", err)
	}
}

func TestNinjaDryRun(t *testing.T) {
	testCases := []struct {
		name string
		fail bool
	}{
		{
			name: "success",
		},
		{
			name: "fail",
			fail: true,
		},
	}
	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			var gotStdout strings.Builder
			ctx := streams.ContextWithStdout(context.Background(), &gotStdout)
			mockStdout := "ninja\nstdout\n"
			subprocessRunner := &fakeSubprocessRunner{
				mockStdout: []byte(mockStdout),
				fail:       tc.fail,
			}
			r := ninjaRunner{
				runner:    subprocessRunner,
				ninjaPath: "ninja",
				buildDir:  t.TempDir(),
			}
			stdout, stderr, err := ninjaDryRun(ctx, r, []string{"foo"})
			if err != nil {
				if !tc.fail {
					t.Errorf("Unexpected error: %s", err)
				}
			} else if tc.fail {
				t.Errorf("Expected error but got none")
			}
			if diff := cmp.Diff(mockStdout, stdout); diff != "" {
				t.Errorf("stdout differs from expected (-want +got): %s", diff)
			}
			if stderr != "" {
				t.Errorf("stderr is unexpectedly non-empty: %s", stderr)
			}

			if tc.fail {
				if diff := cmp.Diff(mockStdout, gotStdout.String()); diff != "" {
					t.Errorf("os.Stdout differs from expected (-want +got): %s", diff)
				}
			} else if gotStdout.String() != "" {
				t.Errorf("ninjaDryRun should not emit to os.Stdout if it succeeds")
			}
		})
	}
}

func TestNinjaGraph(t *testing.T) {
	ctx := context.Background()
	stdout := "ninja\ngraph\nstdout"
	r := ninjaRunner{
		runner: &fakeSubprocessRunner{
			mockStdout: []byte(stdout),
		},
		ninjaPath: "ninja",
		buildDir:  t.TempDir(),
	}
	path := filepath.Join(t.TempDir(), "foo.txt")
	if err := ninjaGraph(ctx, r, []string{"foo", "bar"}, path); err != nil {
		t.Fatal(err)
	}
	defer os.Remove(path)
	fileContentsBytes, err := ioutil.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	fileContents := string(fileContentsBytes)
	if diff := cmp.Diff(stdout, fileContents); diff != "" {
		t.Errorf("Unexpected ninja graph file diff (-want +got):\n%s", diff)
	}
}

func TestNinjaCompdb(t *testing.T) {
	ctx := context.Background()
	stdout := "ninja\ncompdb\nstdout"
	r := ninjaRunner{
		runner: &fakeSubprocessRunner{
			mockStdout: []byte(stdout),
		},
		ninjaPath: "ninja",
		buildDir:  t.TempDir(),
	}
	path := filepath.Join(t.TempDir(), "foo.txt")
	if err := ninjaCompdb(ctx, r, path); err != nil {
		t.Fatal(err)
	}
	defer os.Remove(path)
	fileContentsBytes, err := ioutil.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	fileContents := string(fileContentsBytes)
	if diff := cmp.Diff(stdout, fileContents); diff != "" {
		t.Errorf("Unexpected ninja compdb file diff (-want +got):\n%s", diff)
	}
}

func TestStripNinjaExplain(t *testing.T) {
	for _, tc := range []struct {
		name  string
		input string
		want  string
	}{
		{
			name: "empty",
		},
		{
			name:  "space",
			input: " ",
			want:  " ",
		},
		{
			name: "strip ninja explains",
			input: `ninja explain: blah
to keep
  ninja explain: discard
should keep
`,
			want: `to keep
should keep
`,
		},
		{
			name: "no ninja explains",
			input: `build
build more
build even more
`,
			want: `build
build more
build even more
`,
		},
		{
			name:  "no newline",
			input: "oneliner",
			want:  "oneliner",
		},
		{
			name: "ninja explain only",
			input: `ninja explain: I have reasons
  ninja explain: you know`,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got := new(strings.Builder)
			w := stripNinjaExplain(got)

			if _, err := w.Write([]byte(tc.input)); err != nil {
				t.Fatalf("Write failed: %v", err)
			}
			w.Flush()
			if diff := cmp.Diff(tc.want, got.String()); diff != "" {
				t.Errorf("Got unexpected output diff when writing in one pass (-want +got):\n%s", diff)
			}

			got.Reset()
			for _, b := range tc.input {
				if _, err := w.Write([]byte{byte(b)}); err != nil {
					t.Fatalf("Write failed: %v", err)
				}
			}
			w.Flush()
			if diff := cmp.Diff(tc.want, got.String()); diff != "" {
				t.Errorf("Got unexpected output diff when writing byte by byte (-want +got):\n%s", diff)
			}
		})
	}
}
