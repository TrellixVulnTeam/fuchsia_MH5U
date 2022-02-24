// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package checklicenses

import (
	"fmt"
	"strings"
	"sync"
)

// Metrics is used for instrumentation
type Metrics struct {
	values map[string]uint
	order  []string

	sync.RWMutex
}

func NewMetrics() *Metrics {
	m := &Metrics{
		values: make(map[string]uint),
		order: []string{
			"num_extensions_excluded",
			"num_licensed",
			"num_non_single_license_files",
			"num_one_file_matched_to_multiple_single_licenses",
			"num_one_file_matched_to_one_single_license",
			"num_single_license_file_match",
			"num_single_license_files",
			"num_unlicensed",
			"num_with_project_license",
		},
	}

	for _, key := range m.order {
		m.values[key] = 0
	}
	return m
}

func (metrics *Metrics) increment(key string) {
	metrics.Lock()
	metrics.values[key]++
	metrics.Unlock()
}

func (metrics *Metrics) string() string {
	b := &strings.Builder{}
	fmt.Fprintln(b, "Metrics:")
	for _, value := range metrics.order {
		fmt.Fprintf(b, "%s: %d\n", value, metrics.values[value])
	}
	return b.String()
}
