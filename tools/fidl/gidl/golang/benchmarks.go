// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package golang

import (
	"bytes"
	"fmt"
	"strings"
	"text/template"

	gidlconfig "go.fuchsia.dev/fuchsia/tools/fidl/gidl/config"
	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var benchmarkTmpl = template.Must(template.New("benchmarkTmpls").Parse(`
package benchmark_suite

import (
	"context"
	"sync"
	"testing"

	"fidl/test/benchmarkfidl"

	"syscall/zx"
	"syscall/zx/fidl"

	"go.fuchsia.dev/fuchsia/src/lib/component"
)


type pools struct {
	bytes sync.Pool
	handleInfos sync.Pool
	handleDispositions sync.Pool
}

func newPools() *pools {
	return &pools{
		bytes: sync.Pool{
			New: func() interface{} {
				return make([]byte, zx.ChannelMaxMessageBytes)
			},
		},
		handleInfos: sync.Pool{
			New: func() interface{} {
				return make([]zx.HandleInfo, zx.ChannelMaxMessageHandles)
			},
		},
		handleDispositions: sync.Pool{
			New: func() interface{} {
				return make([]zx.HandleDisposition, zx.ChannelMaxMessageHandles)
			},
		},
	}
}

func (p *pools) useOnce() {
	p.bytes.Put(p.bytes.Get().([]byte))
	p.handleInfos.Put(p.handleInfos.Get().([]zx.HandleInfo))
	p.handleDispositions.Put(p.handleDispositions.Get().([]zx.HandleDisposition))
}

{{ range .Benchmarks }}
func BenchmarkEncode{{ .Name }}(b *testing.B) {
	pools := newPools()
	pools.useOnce()
	input := {{ .Value }}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		// This should be kept in sync with the buffer allocation strategy used in Go bindings.
		respb := pools.bytes.Get().([]byte)
		resphd := pools.handleDispositions.Get().([]zx.HandleDisposition)
		_, _, err := fidl.Marshal(fidl.NewCtx(), &input, respb, resphd)
		if err != nil {
			b.Fatal(err)
		}
		pools.bytes.Put(respb)
		pools.handleDispositions.Put(resphd)
	}
}

func BenchmarkDecode{{ .Name }}(b *testing.B) {
	data := make([]byte, 65536)
	input := {{ .Value }}
	_, _, err := fidl.Marshal(fidl.NewCtx(), &input, data, nil)
	if err != nil {
		b.Fatal(err)
	}

	var output {{ .ValueType }}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		err := fidl.Unmarshal(fidl.NewCtx(), data, nil, &output)
		if err != nil {
			b.Fatal(err)
		}
	}
}

{{ if .EnableSendEventBenchmark }}
func BenchmarkSendEvent{{ .Name }}(b *testing.B) {
	sender_end, recipient_end, err := zx.NewChannel(0)
	if err != nil {
		b.Fatal(err)
	}
	defer sender_end.Close()
	defer recipient_end.Close()
	sender_proxy := {{ .ValueType }}EventProtocolEventProxy(fidl.ChannelProxy{Channel: sender_end})
	recipient_proxy := {{ .ValueType }}EventProtocolWithCtxInterface(fidl.ChannelProxy{Channel: recipient_end})

	input := {{ .Value }}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if err := sender_proxy.Send(input); err != nil {
			b.Fatal(err)
		}
		if _, err := recipient_proxy.ExpectSend(context.Background()); err != nil {
			b.Fatal(err)
		}
	}
}
{{ end }}

{{ if .EnableEchoCallBenchmark }}
type {{ .Name }}EchoCallService struct {}

func (s *{{ .Name }}EchoCallService) Echo(ctx fidl.Context, val {{ .ValueType }}) ({{ .ValueType }}, error) {
	return val, nil
}

func BenchmarkEchoCall{{ .Name }}(b *testing.B) {
	client_end, server_end, err := zx.NewChannel(0)
	if err != nil {
		b.Fatal(err)
	}
	defer client_end.Close()
	stub := &{{ .ValueType }}EchoCallWithCtxStub{
		Impl: &{{ .Name }}EchoCallService{},
	}
	go component.Serve(context.Background(), stub, server_end, component.ServeOptions{
		OnError: func (err error) {
			b.Fatal(err)
		},
	})
	proxy := {{ .ValueType }}EchoCallWithCtxInterface(fidl.ChannelProxy{Channel: client_end})

	input := {{ .Value }}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := proxy.Echo(context.Background(), input); err != nil {
			b.Fatal(err)
		}
	}
}
{{- end -}}
{{ end }}

type Benchmark struct {
	Label string
	BenchFunc func(*testing.B)
}

// Benchmarks is read by go_fidl_benchmarks_lib.
var Benchmarks = []Benchmark{
{{ range .Benchmarks }}
	{
		Label: "Encode/{{ .ChromeperfPath }}",
		BenchFunc: BenchmarkEncode{{ .Name }},
	},
	{
		Label: "Decode/{{ .ChromeperfPath }}",
		BenchFunc: BenchmarkDecode{{ .Name }},
	},
	{{ if .EnableSendEventBenchmark }}
	{
		Label: "SendEvent/{{ .ChromeperfPath }}",
		BenchFunc: BenchmarkSendEvent{{ .Name }},
	},
	{{- end -}}
	{{ if .EnableEchoCallBenchmark }}
	{
		Label: "EchoCall/{{ .ChromeperfPath }}",
		BenchFunc: BenchmarkEchoCall{{ .Name }},
	},
	{{- end -}}
{{ end }}
}
`))

type benchmarkTmplInput struct {
	Benchmarks []benchmark
}

type benchmark struct {
	Name, ChromeperfPath, Value, ValueType            string
	EnableSendEventBenchmark, EnableEchoCallBenchmark bool
}

// GenerateBenchmarks generates Go benchmarks.
func GenerateBenchmarks(gidl gidlir.All, fidl fidlgen.Root, config gidlconfig.GeneratorConfig) ([]byte, error) {
	schema := gidlmixer.BuildSchema(fidl)
	var benchmarks []benchmark
	for _, gidlBenchmark := range gidl.Benchmark {
		decl, err := schema.ExtractDeclaration(gidlBenchmark.Value, gidlBenchmark.HandleDefs)
		if err != nil {
			return nil, fmt.Errorf("benchmark %s: %s", gidlBenchmark.Name, err)
		}
		value := visit(gidlBenchmark.Value, decl)
		benchmarks = append(benchmarks, benchmark{
			Name:                     goBenchmarkName(gidlBenchmark.Name),
			ChromeperfPath:           gidlBenchmark.Name,
			Value:                    value,
			ValueType:                declName(decl),
			EnableSendEventBenchmark: gidlBenchmark.EnableSendEventBenchmark,
			EnableEchoCallBenchmark:  gidlBenchmark.EnableEchoCallBenchmark,
		})
	}
	input := benchmarkTmplInput{
		Benchmarks: benchmarks,
	}
	var buf bytes.Buffer
	err := withGoFmt{benchmarkTmpl}.Execute(&buf, input)
	return buf.Bytes(), err
}

func goBenchmarkName(gidlName string) string {
	return strings.ReplaceAll(gidlName, "/", "_")
}
