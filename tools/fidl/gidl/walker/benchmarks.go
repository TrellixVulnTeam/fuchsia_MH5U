// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package walker

import (
	"bytes"
	"fmt"
	"strings"
	"text/template"

	gidlconfig "go.fuchsia.dev/fuchsia/tools/fidl/gidl/config"
	libhlcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/hlcpp"
	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	libllcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/llcpp/lib"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var benchmarkTmpl = template.Must(template.New("tmpl").Parse(`
#include <fidl/test.{{ .FidlLibrary }}/cpp/wire.h>
#include <perftest/perftest.h>
#include <cts/tests/pkg/fidl/cpp/test/handle_util.h>

#include <vector>

#include "src/tests/benchmarks/fidl/walker/walker_benchmark_util.h"

namespace {

{{ range .Benchmarks }}
void Build{{ .Name }}(std::function<void({{.Type}})> f) {
{{- if .HandleDefs }}
	auto handle_defs = {{ .HandleDefs }};
{{- end }}
	{{ .ValueBuild }}
	f(std::move({{ .ValueVar }}));
}

bool BenchmarkWalker{{ .Name }}(perftest::RepeatState* state) {
	return walker_benchmarks::WalkerBenchmark<{{ .Type }}>(state, Build{{ .Name }});
}
{{ end }}

void RegisterTests() {
	{{ range .Benchmarks }}
	perftest::RegisterTest("Walker/{{ .Path }}/WallTime", BenchmarkWalker{{ .Name }});
	{{ end }}
}
PERFTEST_CTOR(RegisterTests)

} // namespace
`))

type benchmark struct {
	Path, Name, Type     string
	ValueBuild, ValueVar string
	HandleDefs           string
}

type benchmarkTmplInput struct {
	FidlLibrary string
	Benchmarks  []benchmark
}

func GenerateBenchmarks(gidl gidlir.All, fidl fidlgen.Root, config gidlconfig.GeneratorConfig) ([]byte, error) {
	schema := gidlmixer.BuildSchema(fidl)
	tmplInput := benchmarkTmplInput{
		FidlLibrary: libraryName(config.CppBenchmarksFidlLibrary),
	}
	for _, gidlBenchmark := range gidl.Benchmark {
		decl, err := schema.ExtractDeclaration(gidlBenchmark.Value, gidlBenchmark.HandleDefs)
		if err != nil {
			return nil, fmt.Errorf("walker benchmark %s: %s", gidlBenchmark.Name, err)
		}
		if gidlir.ContainsUnknownField(gidlBenchmark.Value) {
			continue
		}
		valBuild, valVar := libllcpp.BuildValueUnowned(gidlBenchmark.Value, decl, libllcpp.HandleReprRaw)
		tmplInput.Benchmarks = append(tmplInput.Benchmarks, benchmark{
			Path:       gidlBenchmark.Name,
			Name:       benchmarkName(gidlBenchmark.Name),
			Type:       llcppBenchmarkType(config.CppBenchmarksFidlLibrary, gidlBenchmark.Value),
			ValueBuild: valBuild,
			ValueVar:   valVar,
			HandleDefs: libhlcpp.BuildHandleDefs(gidlBenchmark.HandleDefs),
		})
	}
	var buf bytes.Buffer
	if err := benchmarkTmpl.Execute(&buf, tmplInput); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func libraryName(librarySuffix string) string {
	return fmt.Sprintf("benchmarkfidl%s", strings.ReplaceAll(librarySuffix, " ", ""))
}

func llcppBenchmarkType(librarySuffix string, value gidlir.Value) string {
	return fmt.Sprintf("test_%s::wire::%s", libraryName(librarySuffix), gidlir.TypeFromValue(value))
}

func benchmarkName(gidlName string) string {
	return strings.ReplaceAll(gidlName, "/", "_")
}
