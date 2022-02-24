// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"os"
	"regexp"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
	"go.fuchsia.dev/fuchsia/tools/fidl/measure-tape/src/hlcpp"
	"go.fuchsia.dev/fuchsia/tools/fidl/measure-tape/src/measurer"
	"go.fuchsia.dev/fuchsia/tools/fidl/measure-tape/src/rust"
)

type listOfStrings []string

func (l *listOfStrings) String() string {
	return strings.Join(*l, " ")
}

func (l *listOfStrings) Set(s string) error {
	*l = append(*l, s)
	return nil
}

var jsonFiles listOfStrings
var targetTypes listOfStrings
var targetBinding = flag.String("target-binding", "",
	"Target binding for which to generate the measure tape")
var outCc = flag.String("out-cc", "",
	"Write path for .cc file\nRequired for target binding hlcpp")
var outH = flag.String("out-h", "",
	"Write path for .h file\nRequired for target binding hlcpp")
var hIncludePath = flag.String("h-include-path", "",
	"Include path for the .h file\nRequired for target binding hlcpp")
var onlyCheckToFile = flag.String("only-check-to-file", "",
	"Enables verification only mode, which checks the .cc and .h\nRequired for target binding hlcpp")
var outRs = flag.String("out-rs", "",
	"Write path for .rs file\nRequired for target binding rust")

func flagsValid() bool {
	if len(jsonFiles) == 0 {
		return false
	}
	if len(targetTypes) == 0 {
		return false
	}
	switch *targetBinding {
	case "hlcpp":
		if len(*outCc) == 0 {
			return false
		}
		if len(*outH) == 0 {
			return false
		}
		if len(*hIncludePath) == 0 {
			return false
		}
	case "rust":
		if len(*outRs) == 0 {
			return false
		}
	default:
		return false
	}
	return true
}

func main() {
	flag.Var(&jsonFiles, "json", "Path(s) to JSON IR")
	flag.Var(&targetTypes, "target-types", "Target type(s) to measure, e.g. fuchsia.ui.scenic/Command")
	flag.Parse()

	if !flag.Parsed() || !flagsValid() {
		flag.PrintDefaults()
		os.Exit(1)
	}

	var roots []fidlgen.Root
	for _, filename := range jsonFiles {
		root, err := fidlgen.ReadJSONIr(filename)
		if err != nil {
			log.Fatal(err)
		}
		roots = append(roots, root)
	}

	var (
		m          = measurer.NewMeasurer(roots)
		allMethods = make(map[measurer.MethodID]*measurer.Method)
		targetMts  []*measurer.MeasuringTape
	)
	for _, targetType := range targetTypes {
		targetMt, err := m.MeasuringTapeFor(targetType)
		if err != nil {
			panic(err)
		}
		targetMts = append(targetMts, targetMt)

		for id, m := range measurer.NewCodeGenerator(targetMt).Generate() {
			allMethods[id] = m
		}
	}

	switch *targetBinding {
	case "hlcpp":
		hlcppGen(m, targetMts, allMethods)
	case "rust":
		rustGen(m, targetMts, allMethods)
	}
}

func hlcppGen(m *measurer.Measurer, targetMts []*measurer.MeasuringTape,
	allMethods map[measurer.MethodID]*measurer.Method) {

	var (
		printer     = hlcpp.NewPrinter(m, *hIncludePath)
		bufH, bufCc bytes.Buffer
	)
	printer.WriteH(&bufH, targetMts)
	printer.WriteCc(&bufCc, targetMts, allMethods)

	if len(*onlyCheckToFile) == 0 {
		writeFile(*outH, bufH.Bytes())
		writeFile(*outCc, bufCc.Bytes())
	} else {
		verifyMeasureTape(bufH.Bytes(), bufCc.Bytes())
	}
}

func writeFile(path string, data []byte) {
	if err := ioutil.WriteFile(path, data, 0644); err != nil {
		panic(err)
	}
}

func verifyMeasureTape(expectedH, expectedCc []byte) {
	actualH, err := ioutil.ReadFile(*outH)
	if err != nil {
		panic(err)
	}
	actualCc, err := ioutil.ReadFile(*outCc)
	if err != nil {
		panic(err)
	}
	if bytes.Compare(actualH, expectedH) != 0 || bytes.Compare(actualCc, expectedCc) != 0 {
		fmt.Fprintf(os.Stderr, "%s and/or %s is out of date! Please run the following\n\n", *outH, *outCc)
		skipUntil := 0
		for i, arg := range os.Args {
			if matched, _ := regexp.MatchString("^-?-only-check-to-file$", arg); matched {
				skipUntil = i + 2
				continue
			}
			if i < skipUntil {
				continue
			}
			if i != 0 {
				fmt.Fprintf(os.Stderr, " \\\n\t")
			}
			fmt.Fprintf(os.Stderr, "%s", arg)
		}
		fmt.Fprintf(os.Stderr, "\n\n")
		os.Exit(1)
	}
	writeFile(*onlyCheckToFile, []byte{})
}

func rustGen(m *measurer.Measurer,
	targetMts []*measurer.MeasuringTape,
	allMethods map[measurer.MethodID]*measurer.Method) {

	var bufRs bytes.Buffer
	rust.WriteRs(&bufRs, m, targetMts, allMethods)
	writeFile(*outRs, bufRs.Bytes())
}
