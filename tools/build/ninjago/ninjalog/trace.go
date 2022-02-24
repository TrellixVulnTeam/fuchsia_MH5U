// Copyright 2014 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package ninjalog

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"go.fuchsia.dev/fuchsia/tools/build/ninjago/chrometrace"
)

func toTrace(step Step, pid int, tid int) chrometrace.Trace {
	tr := chrometrace.Trace{
		Name:            step.Out,
		Category:        step.Category(),
		EventType:       chrometrace.CompleteEvent,
		TimestampMicros: int(step.Start / time.Microsecond),
		DurationMicros:  int(step.Duration() / time.Microsecond),
		ProcessID:       pid,
		ThreadID:        tid,
		Args: map[string]interface{}{
			"outputs":     step.AllOutputs(),
			"total float": step.TotalFloat.String(),
		},
	}

	if step.Command != nil {
		tr.Args["command"] = step.Command.Command
	}
	if step.OnCriticalPath {
		// Add "critical_path" to category to allow easy searching and highlighting
		// in tracer viewer.
		//
		// Note: given chrome trace viewer does a full text search, searching
		// "critical_path" might yield false positives if they happen to have this
		// string in their output name or command. If we see this happening often we
		// can make this string more unique.
		tr.Category += ",critical_path"
		tr.Args["drag"] = step.Drag.String()
	}
	return tr
}

func toFlowEvents(from, to Step, id int, pid int, tids map[string]int) []chrometrace.Trace {
	return []chrometrace.Trace{
		{
			Name:      "critical_path",
			Category:  "critical_path",
			EventType: chrometrace.FlowEventStart,
			// Start flow event arrow from the middle of the "from" event.
			TimestampMicros: int((from.Start + from.Duration()/2) / time.Microsecond),
			ProcessID:       pid,
			ThreadID:        tids[from.Out],
			ID:              id,
		},
		{
			Name:      "critical_path",
			Category:  "critical_path",
			EventType: chrometrace.FlowEventEnd,
			// Point flow event arrow to the middle of the "to" event.
			TimestampMicros: int((to.Start + to.Duration()/2) / time.Microsecond),
			ProcessID:       pid,
			ThreadID:        tids[to.Out],
			ID:              id,
			// Set "enclosing slice" as the binding point. This tells the trace viewer
			// `TimestampMicros` is in the middle of the "to" event.
			//
			// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/edit#heading=h.4qqub5rv9ybk
			BindingPoint: "e",
		},
	}
}

// ToTraces converts Flow outputs into trace log.
//
// If a non-empty `criticalPath` is provided, steps on the critical path will
// have "critical_path" in their category to enable quick searching and
// highlighting.
func ToTraces(steps [][]Step, pid int) []chrometrace.Trace {
	var criticalThreads, nonCriticalThreads [][]Step
Outer:
	for _, thread := range steps {
		// Elevate threads with critical steps to the top of the trace.
		for _, s := range thread {
			if s.OnCriticalPath {
				criticalThreads = append(criticalThreads, thread)
				continue Outer
			}
		}
		nonCriticalThreads = append(nonCriticalThreads, thread)
	}
	steps = append(criticalThreads, nonCriticalThreads...)

	var criticalPath []Step
	var traces []chrometrace.Trace
	// Record `tid`s of all critical steps for generating flow events later.
	tids := make(map[string]int)
	for tid, thread := range steps {
		for _, step := range thread {
			traces = append(traces, toTrace(step, pid, tid))
			if step.OnCriticalPath {
				tids[step.Out] = tid
				criticalPath = append(criticalPath, step)
			}
		}
	}

	sort.Slice(criticalPath, func(i, j int) bool { return criticalPath[i].Start < criticalPath[j].Start })
	// Draw lines (flow events) between all critical steps.
	for i := 1; i < len(criticalPath); i++ {
		traces = append(traces, toFlowEvents(criticalPath[i-1], criticalPath[i], i, pid, tids)...)
	}

	sort.Sort(chrometrace.ByStart(traces))
	return traces
}

// clangTrace matches the JSON output format from clang when time-trace is
// enabled.
type clangTrace struct {
	// TraceEvents contains all events in this trace.
	TraceEvents []chrometrace.Trace
	// BeginningOfTimeMicros identifies the time when this clang command started,
	// using microseconds since epoch.
	BeginningOfTimeMicros int `json:"beginningOfTime"`
}

// ClangTracesToInterleave returns all clang traces that can be interleaved
// directly into their corresponding build steps from `mainTraces`.
//
// `buildRoot` should point to the directory where the Ninja build of
// `mainTrace` is executed, where this function will look for clang traces next
// to object files built by clang. Object files with no clang traces next to
// them are skipped.
//
// Clang traces include events with very short durations, so `granularity` is
// provided to filter them and reduce the size of returned slice.
func ClangTracesToInterleave(mainTraces []chrometrace.Trace, buildRoot string, granularity time.Duration) ([]chrometrace.Trace, error) {
	var interleaved []chrometrace.Trace

	for _, mainTrace := range mainTraces {
		if !strings.HasSuffix(mainTrace.Name, ".o") {
			continue
		}

		// Clang writes a .json file next to the compiled object file when
		// time-trace is enabled.
		//
		// https://releases.llvm.org/9.0.0/tools/clang/docs/ReleaseNotes.html#new-compiler-flags
		clangTracePath := filepath.Join(buildRoot, strings.TrimSuffix(mainTrace.Name, ".o")+".json")
		if _, err := os.Stat(clangTracePath); err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return nil, err
		}
		traceFile, err := os.Open(clangTracePath)
		if err != nil {
			return nil, err
		}
		var cTrace clangTrace
		if err := json.NewDecoder(traceFile).Decode(&cTrace); err != nil {
			return nil, fmt.Errorf("failed to decode clang trace %s: %w", clangTracePath, err)
		}

		for _, t := range cTrace.TraceEvents {
			// Event names starting with "Total" are sums of event durations of a
			// type, for example: "Total Frontend". They are used to form a bar chart
			// in clang traces, so they always stat from time 0 on separate threads.
			// We exclude them so they don't cause interleaved events to misalign.
			if strings.HasPrefix(t.Name, "Total ") || t.EventType != chrometrace.CompleteEvent || t.DurationMicros < int(granularity/time.Microsecond) {
				continue
			}

			if t.DurationMicros > mainTrace.DurationMicros {
				return nil, fmt.Errorf("clang trace for %q has an event %q with duration %dµs, which is longer than the duration of the this clang build %dµs, please make sure they are from the same build", mainTrace.Name, t.Name, t.DurationMicros, mainTrace.DurationMicros)
			}

			// Left align this event with the corresponding step in the main trace,
			// and put them in the same thread, so in the trace viewer it will be
			// displayed right below that step.
			t.ProcessID = mainTrace.ProcessID
			t.ThreadID = mainTrace.ThreadID
			t.TimestampMicros += mainTrace.TimestampMicros

			interleaved = append(interleaved, t)
		}
	}
	return interleaved, nil
}
