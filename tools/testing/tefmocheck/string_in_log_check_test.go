// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package tefmocheck

import (
	"fmt"
	"path"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestCheck(t *testing.T) {
	const killerString = "KILLER STRING"
	const exceptString = "Don't die!"
	exceptBlock := &logBlock{
		startString: "skip_test",
		endString:   "end_test",
	}
	exceptBlock2 := &logBlock{
		startString: "block_start",
		endString:   "block_end",
	}
	c := stringInLogCheck{
		String:         killerString,
		OnlyOnStates:   []string{},
		ExceptString:   exceptString,
		ExceptBlocks:   []*logBlock{exceptBlock, exceptBlock2},
		SkipPassedTask: true,
		Type:           swarmingOutputType,
	}
	gotName := c.Name()
	wantName := "string_in_log/infra_and_test_std_and_klog.txt/KILLER_STRING"
	if gotName != wantName {
		t.Errorf("c.Name() returned %q, want %q", gotName, wantName)
	}

	for _, tc := range []struct {
		name                string
		attributeToTest     bool
		testingOutputs      TestingOutputs
		states              []string
		swarmingResultState string
		shouldMatch         bool
		wantName            string
	}{
		{
			name: "should match simple",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(fmt.Sprintf("PREFIX %s SUFFIX", killerString)),
			},
			shouldMatch: true,
		},
		{
			name: "exceptSuccessfulSwarmingResult",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(fmt.Sprintf("PREFIX %s SUFFIX", killerString)),
			},
			swarmingResultState: "COMPLETED",
		},
		{
			name: "should not match if string in other log",
			testingOutputs: TestingOutputs{
				SerialLog:      []byte(killerString),
				SwarmingOutput: []byte("gentle string"),
			},
		},
		{
			name: "should not match if except_string in log",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(killerString + exceptString),
			},
		},
		{
			name: "should match if string before except_block",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(fmt.Sprintf("PREFIX %s ... %s output %s SUFFIX", killerString, exceptBlock.startString, exceptBlock.endString)),
			},
			shouldMatch: true,
		},
		{
			name: "should match if string after except_block",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(fmt.Sprintf("PREFIX %s output %s ... %s SUFFIX", exceptBlock.startString, exceptBlock.endString, killerString)),
			},
			shouldMatch: true,
		},
		{
			name: "should not match if string in except_block",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(
					fmt.Sprintf(
						"PREFIX %s %s output %s SUFFIX %s %s %s",
						exceptBlock.startString, killerString, exceptBlock.endString,
						exceptBlock2.startString, killerString, exceptBlock2.endString)),
			},
		},
		{
			name: "should match if string in both except_block and outside except_block",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(fmt.Sprintf(
					"PREFIX %s ... %s %s %s %s %s %s SUFFIX",
					killerString, exceptBlock.startString, killerString, exceptBlock.endString,
					exceptBlock2.startString, killerString, exceptBlock2.endString,
				)),
			},
			shouldMatch: true,
		},
		{
			name: "should match if swarming task state is in expected states",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(killerString),
			},
			states:              []string{"STATE_1", "STATE_2"},
			swarmingResultState: "STATE_1",
			shouldMatch:         true,
		},
		{
			name: "should not match if swarming task state is not in expected states",
			testingOutputs: TestingOutputs{
				SwarmingOutput: []byte(killerString),
			},
			states:              []string{"STATE_1", "STATE_2"},
			swarmingResultState: "NO_STATE",
		},
		{
			name:            "should match per test swarming output",
			attributeToTest: true,
			testingOutputs: TestingOutputs{
				SwarmingOutputPerTest: []TestLog{
					{
						TestName: "foo-test",
						Bytes:    []byte(killerString),
						FilePath: "foo/log.txt",
					},
				},
			},
			shouldMatch: true,
			wantName:    path.Join(wantName, "foo-test"),
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c := c // Make a copy to avoid modifying shared state.
			c.AttributeToTest = tc.attributeToTest
			// It accesses this field for DebugText().
			tc.testingOutputs.SwarmingSummary = &SwarmingTaskSummary{
				Results: &SwarmingRpcsTaskResult{
					TaskId: "abc", State: tc.swarmingResultState,
				},
			}
			c.OnlyOnStates = tc.states
			if c.Check(&tc.testingOutputs) != tc.shouldMatch {
				t.Errorf("c.Check(%q) returned %t, expected %t",
					string(tc.testingOutputs.SerialLog), !tc.shouldMatch, tc.shouldMatch)
			}
			gotName := c.Name()
			if tc.wantName != "" && gotName != tc.wantName {
				t.Errorf("c.Name() returned %q, want %q", gotName, tc.wantName)
			}
			c.DebugText() // minimal coverage, check it doesn't crash.
			swarmingOutputPerTest := tc.testingOutputs.SwarmingOutputPerTest
			gotOutputFiles := c.OutputFiles()
			if len(swarmingOutputPerTest) == 0 && len(gotOutputFiles) != 0 {
				t.Errorf("c.OutputFiles() returned %s, want []", gotOutputFiles)
			}
			if len(swarmingOutputPerTest) == 1 && (len(gotOutputFiles) != 1 || swarmingOutputPerTest[0].FilePath != gotOutputFiles[0]) {
				t.Errorf("c.OutputFiles() returned %s, want %q", gotOutputFiles, swarmingOutputPerTest[0].FilePath)
			}
		})
	}
}

func TestStringInLogsChecks(t *testing.T) {
	t.Run("checks for infra tool logs are bucketed correctly", func(t *testing.T) {
		infraTools := []string{"botanist", "testrunner"}
		for _, check := range fuchsiaLogChecks() {
			for _, tool := range infraTools {
				if strings.Contains(check.Name(), tool) {
					t.Errorf("Log check mentioning tool %q should go in infraToolLogChecks: %q", tool, check.Name())
				}
			}
		}
	})

	t.Run("StringInLogsChecks returns all expected checks", func(t *testing.T) {
		// This is intentionally brittle; we want to make it difficult for
		// people to reorder checks in ways that might degrade tefmocheck's
		// ability to root-cause failures.
		expected := append(fuchsiaLogChecks(), infraToolLogChecks()...)
		got := StringInLogsChecks()
		// Crude check to make sure someone doesn't accidentally strip out most
		// of StringInLogsChecks .
		if len(got) < 50 {
			t.Fatalf("StringInLogsChecks returned only %d checks, expected >=50", len(got))
		}
		// It's ok to have some extra checks preceding the start of the expected
		// checks, so trim those off.
		got = got[len(got)-len(expected):]
		if diff := cmp.Diff(expected, got, cmp.AllowUnexported(stringInLogCheck{}, logBlock{})); diff != "" {
			t.Errorf("StringInLogsChecks() returned diff (-want +got): %s", diff)
		}
	})
}
