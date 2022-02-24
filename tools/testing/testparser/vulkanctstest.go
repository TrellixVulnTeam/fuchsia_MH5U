// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testparser

import (
	"regexp"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
)

var (
	ctsTestPreamblePattern  = regexp.MustCompile(`^dEQP Core .* starting\.\.$`)
	ctsTestCaseStartPattern = regexp.MustCompile(`^Test case '(.*)'\.\.$`)
	ctsTestCasePattern      = regexp.MustCompile(`^(Pass|Fail|QualityWarning|CompatibilityWarning|Pending|NotSupported|ResourceError|InternalError|Crash|Timeout) \(.*\)$`)
)

func parseVulkanCtsTest(lines [][]byte) []runtests.TestCaseResult {
	var res []runtests.TestCaseResult
	var displayName string
	var suiteName string
	var caseName string
	for _, line := range lines {
		line := string(line)
		m := ctsTestCaseStartPattern.FindStringSubmatch(line)
		if m != nil {
			displayName = m[1]
			dotIndex := strings.LastIndex(displayName, ".")
			suiteName = displayName[0:dotIndex]
			caseName = displayName[dotIndex+1:]
			continue
		}
		m = ctsTestCasePattern.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		var status runtests.TestResult
		// List of results is in framework/qphelper/qpTestLog.c
		switch m[1] {
		case "Pass", "QualityWarning", "CompatibilityWarning":
			status = runtests.TestSuccess
		case "Fail",
			"Pending",
			"ResourceError",
			"InternalError",
			"Crash":
			status = runtests.TestFailure
		case "NotSupported":
			status = runtests.TestSkipped
		case "Timeout":
			status = runtests.TestAborted
		}
		res = append(res, runtests.TestCaseResult{
			DisplayName: displayName,
			SuiteName:   suiteName,
			CaseName:    caseName,
			Status:      status,
			Format:      "VulkanCtsTest",
		})
		displayName = ""
	}
	// Check for incomplete tests
	if displayName != "" {
		res = append(res, runtests.TestCaseResult{
			DisplayName: displayName,
			SuiteName:   suiteName,
			CaseName:    caseName,
			Status:      runtests.TestFailure,
			Format:      "VulkanCtsTest",
		})
	}
	return res
}
