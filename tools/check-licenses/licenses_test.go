// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package checklicenses

import (
	"context"
	"path/filepath"
	"testing"
)

func TestLicensesMatchSingleLicenseFile(t *testing.T) {
	root := filepath.Join(*testDataDir, "licenses", "simple")
	config := setupConfig("simple", t)

	licenseAllowList := map[string][]string{
		"apache.lic": {"foo.rs"},
		"bsd.lic":    {"bar.rs"},
	}
	config.LicenseAllowList = licenseAllowList

	l, err := NewLicenses(context.Background(), config)
	if err != nil {
		t.Fatalf("NewLicenses(...): %s", err)
	}
	metrics := NewMetrics()
	ft, err := NewFileTree(context.Background(), root, nil, config, metrics)
	if err != nil {
		t.Fatal(err)
	}
	data := []byte("This is very Apache licensed\nCopyright Foo\n")
	l.MatchSingleLicenseFile(data, "foo.rs", metrics, ft)
	data = []byte("BSD much.\nCopyright Bar Inc\n")
	l.MatchSingleLicenseFile(data, "bar.rs", metrics, ft)
	l.MatchSingleLicenseFile(data, "baz.rs", metrics, ft)

	if metrics.values["num_single_license_file_match"] != 3 {
		t.Error(metrics.values["num_single_license_file_match"])
	}
}

func TestLicensesMatchFile(t *testing.T) {
	config := setupConfig("simple", t)

	licenseAllowList := map[string][]string{
		"apache.lic": {"foo.rs"},
		"bsd.lic":    {"bar.rs"},
	}
	config.LicenseAllowList = licenseAllowList

	l, err := NewLicenses(context.Background(), config)
	if err != nil {
		t.Fatalf("NewLicenses(...): %s", err)
	}
	metrics := NewMetrics()
	data := []byte("This is very Apache licensed\nCopyright Foo\n")
	ok, _, _ := l.MatchFile(data, "foo.rs", metrics)
	if !ok {
		t.Error("Apache didn't match")
	}
	data = []byte("BSD much.\nCopyright Bar Inc\n")
	ok, _, _ = l.MatchFile(data, "bar.rs", metrics)
	if !ok {
		t.Error("Apache didn't match")
	}
	ok, _, _ = l.MatchFile(data, "baz.rs", metrics)
	if !ok {
		t.Error("BSD didn't match")
	}
	if metrics.values["num_licensed"] != 3 {
		t.Error(metrics.values["num_licensed"])
	}
}

func TestNewLicenses(t *testing.T) {
	config := setupConfig("simple", t)

	l, err := NewLicenses(context.Background(), config)
	if err != nil {
		t.Fatalf("NewLicenses(...): %s", err)
	}
	if len(l.licenses) != 2 {
		t.Fatalf("Got %#v", l.licenses)
	}
	// bsd comes first because it is shorter.
	if l.licenses[0].Category != "bsd.lic" {
		t.Fatalf("Got %#v", l.licenses[0])
	}
	if l.licenses[1].Category != "apache.lic" {
		t.Fatalf("Got %#v", l.licenses[0])
	}
}

func TestCheckLicenseAllowList(t *testing.T) {
	config := setupConfig("simple", t)

	licenseAllowList := map[string][]string{
		"apache.lic": {"foo.rs"},
		"bsd.lic":    {"bar.rs"},
	}
	config.LicenseAllowList = licenseAllowList

	l, err := NewLicenses(context.Background(), config)
	if err != nil {
		t.Fatalf("NewLicenses(...): %s", err)
	}
	if !l.licenses[0].CheckAllowList("bar.rs") {
		t.Errorf("%s was not able to be used in bar.rs", l.licenses[0].Category)
	}
	if !l.licenses[1].CheckAllowList("foo.rs") {
		t.Errorf("%s was not able to be used in foo.rs", l.licenses[1].Category)
	}
	if l.licenses[0].CheckAllowList("foo.rs") {
		t.Errorf("%s should not be able to be used in foo.rs", l.licenses[0].Category)
	}
	if l.licenses[1].CheckAllowList("bar.rs") {
		t.Errorf("%s should not able to be used in bar.rs", l.licenses[0].Category)
	}
}

func setupConfig(name string, t *testing.T) *Config {
	configPath := filepath.Join(*testDataDir, "licenses", "simple", "config.json")
	config, err := NewConfig(configPath)
	if err != nil {
		t.Fatal(err)
	}
	config.LicensePatternDir = filepath.Join(*testDataDir, config.LicensePatternDir)
	return config
}
