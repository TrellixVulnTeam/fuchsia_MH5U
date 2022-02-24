// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.package main

package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"errors"
	"flag"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"go.fuchsia.dev/fuchsia/tools/build/ninjago/compdb"
	"go.fuchsia.dev/fuchsia/tools/build/ninjago/ninjalog"
)

var testDataDir = flag.String("test_data_dir", "../test_data", "Path to ../test_data/; only used in GN build")

func readAndUnzip(t *testing.T, path string) *gzip.Reader {
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("Failed to read %q: %v", path, err)
	}
	t.Cleanup(func() { f.Close() })

	unzipped, err := gzip.NewReader(f)
	if err != nil {
		t.Fatalf("Failed to unzip %q: %v", path, err)
	}
	t.Cleanup(func() { unzipped.Close() })
	return unzipped
}

func TestExtractAndSerializeBuildStats(t *testing.T) {
	graph, err := constructGraph(inputs{
		ninjalog: readAndUnzip(t, filepath.Join(*testDataDir, "ninja_log.gz")),
		compdb:   readAndUnzip(t, filepath.Join(*testDataDir, "compdb.json.gz")),
		graph:    readAndUnzip(t, filepath.Join(*testDataDir, "graph.dot.gz")),
	})
	if err != nil {
		t.Fatalf("Failed to construct graph: %v", err)
	}

	stats, err := extractBuildStats(&graph, 0)
	if err != nil {
		t.Fatalf("Failed to extract build stats: %v", err)
	}
	if len(stats.CriticalPath) == 0 {
		t.Errorf("Critical path in stats is emtpy, expect non-empty")
	}
	if len(stats.Slowests) == 0 {
		t.Errorf("Slowest builds in stats is empty, expect non-empty")
	}
	if len(stats.CatBuildTimes) == 0 {
		t.Errorf("Build times by category in stats is empty, expect non-empty")
	}
	if len(stats.All) == 0 {
		t.Errorf("All in stats is empty, expect non-empty")
	}

	buffer := new(bytes.Buffer)
	if err := serializeBuildStats(stats, buffer); err != nil {
		t.Fatalf("Failed to serialize build stats: %v", err)
	}
	var gotStats buildStats
	if err := json.NewDecoder(buffer).Decode(&gotStats); err != nil {
		t.Fatalf("Failed to deserialize build stats: %v", err)
	}
	if diff := cmp.Diff(stats, gotStats); diff != "" {
		t.Errorf("build stats diff after deserialization (-want, +got):\n%s", diff)
	}
}

type stubGraph struct {
	steps []ninjalog.Step
	err   error
}

func (g *stubGraph) PopulatedSteps() ([]ninjalog.Step, error) {
	return g.steps, g.err
}

func TestExtractStats(t *testing.T) {
	for _, v := range []struct {
		name               string
		minActionBuildTime time.Duration
		g                  stubGraph
		want               buildStats
	}{
		{
			name: "empty steps",
		},
		{
			name:               "successfully extract stats",
			minActionBuildTime: 0,
			g: stubGraph{
				steps: []ninjalog.Step{
					{
						CmdHash:        1,
						Out:            "a.o",
						Outs:           []string{"aa.o", "aaa.o"},
						End:            3 * time.Second,
						Command:        &compdb.Command{Command: "gomacc a.cc"},
						OnCriticalPath: true,
						Drag:           123 * time.Second,
					},
					{
						CmdHash:        2,
						Out:            "b.o",
						Start:          3 * time.Second,
						End:            5 * time.Second,
						Command:        &compdb.Command{Command: "rustc b.rs"},
						OnCriticalPath: true,
						Drag:           321 * time.Second,
					},
					{
						CmdHash:    3,
						Out:        "c.o",
						Start:      9 * time.Second,
						End:        10 * time.Second,
						Command:    &compdb.Command{Command: "gomacc c.cc"},
						TotalFloat: 789 * time.Second,
					},
				},
			},
			want: buildStats{
				CriticalPath: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
						Drag:     123 * time.Second,
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
						Drag:     321 * time.Second,
					},
				},
				Slowests: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
						Drag:     123 * time.Second,
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
						Drag:     321 * time.Second,
					},
					{
						Command:    "gomacc c.cc",
						Outputs:    []string{"c.o"},
						Start:      9 * time.Second,
						End:        10 * time.Second,
						Category:   "gomacc",
						TotalFloat: 789 * time.Second,
					},
				},
				CatBuildTimes: []catBuildTime{
					{
						Category:     "gomacc",
						Count:        2,
						BuildTime:    4 * time.Second,
						MinBuildTime: time.Second,
						MaxBuildTime: 3 * time.Second,
					},
					{
						Category:     "rustc",
						Count:        1,
						BuildTime:    2 * time.Second,
						MinBuildTime: 2 * time.Second,
						MaxBuildTime: 2 * time.Second,
					},
				},
				All: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
						Drag:     123 * time.Second,
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
						Drag:     321 * time.Second,
					},
					{
						Command:    "gomacc c.cc",
						Outputs:    []string{"c.o"},
						Start:      9 * time.Second,
						End:        10 * time.Second,
						Category:   "gomacc",
						TotalFloat: 789 * time.Second,
					},
				},
				Actions: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
						Drag:     123 * time.Second,
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
						Drag:     321 * time.Second,
					},
					{
						Command:    "gomacc c.cc",
						Outputs:    []string{"c.o"},
						Start:      9 * time.Second,
						End:        10 * time.Second,
						Category:   "gomacc",
						TotalFloat: 789 * time.Second,
					},
				},
				TotalBuildTime: 6 * time.Second,
				BuildDuration:  10 * time.Second,
			},
		},
		{
			name:               "filter short actions",
			minActionBuildTime: time.Minute,
			g: stubGraph{
				steps: []ninjalog.Step{
					{CmdHash: 1, Out: "1", End: time.Second},
					{CmdHash: 2, Out: "2", End: time.Minute},
					{CmdHash: 3, Out: "3", End: 2 * time.Minute},
				},
			},
			want: buildStats{
				Slowests: []action{
					{Outputs: []string{"3"}, Category: "unknown", End: 2 * time.Minute},
					{Outputs: []string{"2"}, Category: "unknown", End: time.Minute},
					{Outputs: []string{"1"}, Category: "unknown", End: time.Second},
				},
				CatBuildTimes: []catBuildTime{
					{
						Category:     "unknown",
						Count:        3,
						BuildTime:    3*time.Minute + time.Second,
						MinBuildTime: time.Second,
						MaxBuildTime: 2 * time.Minute,
					},
				},
				All: []action{
					{Outputs: []string{"2"}, Category: "unknown", End: time.Minute},
					{Outputs: []string{"3"}, Category: "unknown", End: 2 * time.Minute},
				},
				Actions: []action{
					{Outputs: []string{"2"}, Category: "unknown", End: time.Minute},
					{Outputs: []string{"3"}, Category: "unknown", End: 2 * time.Minute},
				},
				TotalBuildTime: 3*time.Minute + time.Second,
				BuildDuration:  2 * time.Minute,
			},
		},
	} {
		t.Run(v.name, func(t *testing.T) {
			gotStats, err := extractBuildStats(&v.g, v.minActionBuildTime)
			if err != nil {
				t.Fatalf("extractBuildStats(%#v, %s) got error: %v", v.g, v.minActionBuildTime, err)
			}
			if diff := cmp.Diff(v.want, gotStats, cmpopts.EquateEmpty()); diff != "" {
				t.Errorf("extractBuildStats(%#v, %s) got stats diff (-want +got):\n%s", v.g, v.minActionBuildTime, diff)
			}
		})
	}
}

func TestExtractStatsError(t *testing.T) {
	g := stubGraph{err: errors.New("test critical path error")}
	if _, err := extractBuildStats(&g, 0); err == nil {
		t.Errorf("extractBuildStats(%#v, nil) got no error, want error", g)
	}
}
