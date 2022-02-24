// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Builds values to be used in reference benchmarks.
package reference

import (
	"bytes"
	"fmt"
	"strings"
	"text/template"

	gidlconfig "go.fuchsia.dev/fuchsia/tools/fidl/gidl/config"
	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	libllcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/llcpp/lib"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var benchmarkTmpl = template.Must(template.New("tmpl").Parse(`
#include <fidl/test.benchmarkfidl/cpp/wire.h>

namespace benchmark_suite {

{{ range .Benchmarks }}
[[maybe_unused]] {{ .Type }} Build_{{ .Name }}(fidl::AnyArena& allocator) {
	{{ .ValueBuild }}
	auto obj = {{ .ValueVar }};
	return obj;
}
{{ end }}

} // namespace benchmark_suite
`))

type benchmark struct {
	Path, Name, Type     string
	ValueBuild, ValueVar string
}

type benchmarkTmplInput struct {
	Benchmarks []benchmark
}

func GenerateBenchmarks(gidl gidlir.All, fidl fidlgen.Root, config gidlconfig.GeneratorConfig) ([]byte, error) {
	schema := gidlmixer.BuildSchema(fidl)
	tmplInput := benchmarkTmplInput{}
	for _, gidlBenchmark := range gidl.Benchmark {
		decl, err := schema.ExtractDeclaration(gidlBenchmark.Value, gidlBenchmark.HandleDefs)
		if err != nil {
			return nil, fmt.Errorf("reference benchmark %s: %s", gidlBenchmark.Name, err)
		}
		if gidlir.ContainsUnknownField(gidlBenchmark.Value) {
			continue
		}
		valBuild, valVar := libllcpp.BuildValueAllocator("allocator", gidlBenchmark.Value, decl, libllcpp.HandleReprRaw)
		tmplInput.Benchmarks = append(tmplInput.Benchmarks, benchmark{
			Path:       gidlBenchmark.Name,
			Name:       benchmarkName(gidlBenchmark.Name),
			Type:       llcppBenchmarkType(gidlBenchmark.Value),
			ValueBuild: valBuild,
			ValueVar:   valVar,
		})
	}
	var buf bytes.Buffer
	if err := benchmarkTmpl.Execute(&buf, tmplInput); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func llcppBenchmarkType(value gidlir.Value) string {
	return fmt.Sprintf("test_benchmarkfidl::wire::%s", gidlir.TypeFromValue(value))
}

func benchmarkName(gidlName string) string {
	return strings.ReplaceAll(gidlName, "/", "_")
}
