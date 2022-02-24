// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"context"
	"debug/elf"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.fuchsia.dev/fuchsia/tools/debug/covargs"
	"go.fuchsia.dev/fuchsia/tools/debug/covargs/api/llvm"
	"go.fuchsia.dev/fuchsia/tools/debug/symbolize"
	"go.fuchsia.dev/fuchsia/tools/lib/cache"
	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/flagmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/lib/retry"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
	"golang.org/x/sync/errgroup"
)

const (
	shardSize              = 1000
	symbolCacheSize        = 100
	cloudFetchMaxAttempts  = 2
	cloudFetchRetryBackoff = 500 * time.Millisecond
	cloudFetchTimeout      = 60 * time.Second
)

var (
	colors          color.EnableColor
	level           logger.LogLevel
	summaryFile     flagmisc.StringsValue
	buildIDDirPaths flagmisc.StringsValue
	symbolServers   flagmisc.StringsValue
	symbolCache     string
	dryRun          bool
	skipFunctions   bool
	outputDir       string
	llvmCov         string
	llvmProfdata    flagmisc.StringsValue
	outputFormat    string
	jsonOutput      string
	reportDir       string
	saveTemps       string
	basePath        string
	diffMappingFile string
	compilationDir  string
	pathRemapping   flagmisc.StringsValue
	srcFiles        flagmisc.StringsValue
	numThreads      int
	jobs            int
)

func init() {
	colors = color.ColorAuto
	level = logger.InfoLevel

	flag.Var(&colors, "color", "can be never, auto, always")
	flag.Var(&level, "level", "can be fatal, error, warning, info, debug or trace")
	flag.Var(&summaryFile, "summary", "path to summary.json file")
	flag.Var(&buildIDDirPaths, "build-id-dir", "path to .build-id directory")
	flag.Var(&symbolServers, "symbol-server", "a GCS URL or bucket name that contains debug binaries indexed by build ID")
	flag.StringVar(&symbolCache, "symbol-cache", "", "path to directory to store cached debug binaries in")
	flag.BoolVar(&dryRun, "dry-run", false, "if set the system prints out commands that would be run instead of running them")
	flag.BoolVar(&skipFunctions, "skip-functions", true, "if set, the coverage report enabled by the `report-dir` flag will not include function coverage")
	flag.StringVar(&outputDir, "output-dir", "", "the directory to output results to")
	flag.Var(&llvmProfdata, "llvm-profdata", "the location of llvm-profdata")
	flag.StringVar(&llvmCov, "llvm-cov", "llvm-cov", "the location of llvm-cov")
	flag.StringVar(&outputFormat, "format", "html", "the output format used for llvm-cov")
	flag.StringVar(&jsonOutput, "json-output", "", "outputs profile information to the specified file")
	flag.StringVar(&saveTemps, "save-temps", "", "save temporary artifacts in a directory")
	flag.StringVar(&reportDir, "report-dir", "", "the directory to save the report to")
	flag.StringVar(&basePath, "base", "", "base path for source tree")
	flag.StringVar(&diffMappingFile, "diff-mapping", "", "path to diff mapping file")
	flag.StringVar(&compilationDir, "compilation-dir", "", "the directory used as a base for relative coverage mapping paths, passed through to llvm-cov")
	flag.Var(&pathRemapping, "path-equivalence", "<from>,<to> remapping of source file paths passed through to llvm-cov")
	flag.Var(&srcFiles, "src-file", "path to a source file to generate coverage for. If provided, only coverage for these files will be generated.\n"+
		"Multiple files can be specified with multiple instances of this flag.")
	flag.IntVar(&numThreads, "num-threads", 0, "number of processing threads")
	flag.IntVar(&jobs, "jobs", runtime.NumCPU(), "number of parallel jobs")
}

const llvmProfileSinkType = "llvm-profile"

// Output is indexed by dump name
func readSummary(summaryFiles []string) (runtests.DataSinkMap, error) {
	sinks := make(runtests.DataSinkMap)

	for _, summaryFile := range summaryFiles {
		// TODO(phosek): process these in parallel using goroutines.
		file, err := os.Open(summaryFile)
		if err != nil {
			return nil, fmt.Errorf("cannot open %q: %w", summaryFile, err)
		}
		defer file.Close()

		var summary runtests.TestSummary
		if err := json.NewDecoder(file).Decode(&summary); err != nil {
			return nil, fmt.Errorf("cannot decode %q: %w", summaryFile, err)
		}

		dir := filepath.Dir(summaryFile)
		for _, detail := range summary.Tests {
			for name, data := range detail.DataSinks {
				for _, sink := range data {
					sinks[name] = append(sinks[name], runtests.DataSink{
						Name: sink.Name,
						File: filepath.Join(dir, sink.File),
					})
				}
			}
		}
	}

	return sinks, nil
}

type Action struct {
	Path string   `json:"cmd"`
	Args []string `json:"args"`
}

func (a Action) Run(ctx context.Context) ([]byte, error) {
	logger.Debugf(ctx, "%s\n", a.String())
	if !dryRun {
		return exec.Command(a.Path, a.Args...).CombinedOutput()
	}
	return nil, nil
}

func (a Action) String() string {
	var buf bytes.Buffer
	fmt.Fprint(&buf, a.Path)
	for _, arg := range a.Args {
		fmt.Fprintf(&buf, " %s", arg)
	}
	return buf.String()
}

const instrProfRawMagic = uint64(255)<<56 | uint64('l')<<48 |
	uint64('p')<<40 | uint64('r')<<32 | uint64('o')<<24 |
	uint64('f')<<16 | uint64('r')<<8 | uint64(129)

type versionFetcher struct {
	mu    sync.RWMutex
	cache map[string]uint64
}

func newVersionFetcher() *versionFetcher {
	return &versionFetcher{cache: make(map[string]uint64)}
}

func (f *versionFetcher) getVersion(filepath string) (uint64, error) {
	f.mu.RLock()
	v, ok := f.cache[filepath]
	f.mu.RUnlock()
	if ok {
		return v, nil
	}

	file, err := os.Open(filepath)
	if err != nil {
		return 0, err
	}
	defer file.Close()
	var magic uint64
	err = binary.Read(file, binary.LittleEndian, &magic)
	if err != nil {
		return 0, fmt.Errorf("failed to read magic: %w", err)
	}
	if magic != instrProfRawMagic {
		return 0, fmt.Errorf("invalid magic: %x", magic)
	}
	var version uint64
	err = binary.Read(file, binary.LittleEndian, &version)
	if err != nil {
		return 0, fmt.Errorf("failed to read version: %w", err)
	}

	f.mu.Lock()
	f.cache[filepath] = version
	f.mu.Unlock()

	return version, nil
}

func isInstrumented(filepath string) bool {
	file, err := os.Open(filepath)
	if err != nil {
		return false
	}
	defer file.Close()
	elfFile, err := elf.NewFile(file)
	if err != nil {
		return false
	}
	elfSymbols, err := elfFile.Symbols()
	if err != nil {
		return false
	}
	for _, symbol := range elfSymbols {
		if symbol.Name == "__llvm_profile_initialize" {
			return true
		}
	}
	return false
}

type profileReadingError struct {
	profile    string
	errMessage string
}

func (e *profileReadingError) Error() string {
	return fmt.Sprintf("cannot read profile %q: %q", e.profile, e.errMessage)
}

// Returns the embedded build id read from a profile by invoking llvm-profdata tool.
// llvm-profdata show --binary-ids
func readEmbeddedBuildId(ctx context.Context, tool string, profile string) (string, error) {
	args := []string{
		"show",
		"--binary-ids",
	}
	args = append(args, profile)
	readCmd := Action{Path: tool, Args: args}
	output, err := readCmd.Run(ctx)
	if err != nil {
		return "", &profileReadingError{profile, string(output)}
	}

	// Split the lines in llvm-profdata output, which should have the following lines for build ids.
	// Binary IDs:
	// 1696251c (This is an example for build id)
	splittedOutput := strings.Split((string(output)), "\n")
	if len(splittedOutput) < 2 {
		return "", fmt.Errorf("invalid llvm-profdata output in profile %q: %w", profile, err)
	}

	embeddedBuildId := splittedOutput[len(splittedOutput)-2]
	// Check if embedded build id consists of hex characters.
	_, err = hex.DecodeString(embeddedBuildId)
	if err != nil {
		return "", fmt.Errorf("invalid build id in profile %q: %w", profile, err)
	}

	return embeddedBuildId, nil
}

// Partition raw profiles by version
type partition struct {
	tool     string
	profiles []string
}

type profileEntry struct {
	Profile string `json:"profile"`
	Module  string `json:"module"`
}

// mergeEntries combines data from runtests and build ids embedded in profiles
// returning a sequence of entries, where each entry contains
// a raw profile and module specified by build ID present in that profile.
func mergeEntries(ctx context.Context, vf *versionFetcher, summary runtests.DataSinkMap, partitions map[uint64]*partition) ([]profileEntry, error) {
	// Dedupe profiles so we only fetch build IDs once for each.
	profiles := make(map[string]struct{})
	for _, sink := range summary[llvmProfileSinkType] {
		profiles[sink.File] = struct{}{}
	}

	profileEntryChan := make(chan profileEntry, len(profiles))
	sems := make(chan struct{}, jobs)
	var eg errgroup.Group
	for profile := range profiles {
		profile := profile // capture range variable.
		sems <- struct{}{}
		eg.Go(func() error {
			defer func() { <-sems }()

			version, err := vf.getVersion(profile)
			if err != nil {
				// TODO(fxbug.dev/83504): Known issue causes occasional failures on host tests.
				// Once resolved, return an error.
				logger.Warningf(ctx, "cannot read version from profile %q: %w", profile, err)
				return nil
			}

			// Find the associated llvm-profdata tool.
			partition, ok := partitions[version]
			if !ok {
				partition = partitions[0]
			}

			// Read embedded build ids, which are enabled for profile versions 7 and above.
			embeddedBuildId, err := readEmbeddedBuildId(ctx, partition.tool, profile)
			if err != nil {
				switch err.(type) {
				// TODO(fxbug.dev/83504): Known issue causes occasional malformed profiles on host tests.
				// Only log the warning for such cases now. Once resolved, return an error.
				case *profileReadingError:
					logger.Warningf(ctx, err.Error())
					return nil
				default:
					return err
				}
			}
			profileEntryChan <- profileEntry{
				Profile: profile,
				Module:  embeddedBuildId,
			}
			return nil
		})
	}

	if err := eg.Wait(); err != nil {
		return nil, err
	}
	close(profileEntryChan)

	var entries []profileEntry
	for pe := range profileEntryChan {
		entries = append(entries, pe)
	}
	return entries, nil
}

func process(ctx context.Context, repo symbolize.Repository) error {
	partitions := make(map[uint64]*partition)
	var err error

	for _, profdata := range llvmProfdata {
		var version uint64
		s := strings.SplitN(profdata, "=", 2)
		if len(s) > 1 {
			version, err = strconv.ParseUint(s[1], 10, 64)
			if err != nil {
				return fmt.Errorf("invalid version number %q: %w", s[1], err)
			}
		}
		partitions[version] = &partition{tool: s[0]}
	}

	if _, ok := partitions[0]; !ok {
		return fmt.Errorf("missing default llvm-profdata tool path")
	}

	// Read in all the data in summary file
	summary, err := readSummary(summaryFile)
	if err != nil {
		return fmt.Errorf("parsing info: %w", err)
	}

	vf := newVersionFetcher()

	// Merge all the information
	entries, err := mergeEntries(ctx, vf, summary, partitions)

	if err != nil {
		return fmt.Errorf("merging info: %w", err)
	}

	tempDir := saveTemps
	if saveTemps == "" {
		tempDir, err = ioutil.TempDir(saveTemps, "covargs")
		if err != nil {
			return fmt.Errorf("cannot create temporary dir: %w", err)
		}
		defer os.RemoveAll(tempDir)
	}

	if jsonOutput != "" {
		file, err := os.Create(jsonOutput)
		if err != nil {
			return fmt.Errorf("creating profile output file: %w", err)
		}
		defer file.Close()
		if err := json.NewEncoder(file).Encode(entries); err != nil {
			return fmt.Errorf("writing profile information: %w", err)
		}
	}

	for _, entry := range entries {
		version, err := vf.getVersion(entry.Profile)
		if err != nil {
			// TODO(fxbug.dev/83504): Known issue causes occasional failures on host tests.
			// Once resolved, return error below.
			logger.Warningf(ctx, "cannot read version from profile %q: %w", entry.Profile, err)
			continue
		}
		partition, ok := partitions[version]
		if !ok {
			partition = partitions[0]
		}
		partition.profiles = append(partition.profiles, entry.Profile)
	}

	profdataFiles := []string{}
	for version, partition := range partitions {
		if len(partition.profiles) == 0 {
			continue
		}

		// Make the llvm-profdata response file
		profdataFile, err := os.Create(filepath.Join(tempDir, "llvm-profdata.rsp"))
		if err != nil {
			return fmt.Errorf("creating llvm-profdata.rsp file: %w", err)
		}

		for _, profile := range partition.profiles {
			fmt.Fprintf(profdataFile, "%s\n", profile)
		}
		profdataFile.Close()

		// Merge all raw profiles
		mergedFile := filepath.Join(tempDir, fmt.Sprintf("merged%d.profdata", version))
		args := []string{
			"merge",
			"--failure-mode=all",
			"--sparse",
			"--output", mergedFile,
		}
		if numThreads != 0 {
			args = append(args, "--num-threads", strconv.Itoa(numThreads))
		}
		args = append(args, "@"+profdataFile.Name())
		mergeCmd := Action{Path: partition.tool, Args: args}
		data, err := mergeCmd.Run(ctx)
		if err != nil {
			return fmt.Errorf("%s failed with %v:\n%s", mergeCmd.String(), err, string(data))
		}
		profdataFiles = append(profdataFiles, mergedFile)
	}

	mergedFile := filepath.Join(tempDir, "merged.profdata")
	args := []string{
		"merge",
		"--failure-mode=all",
		"--sparse",
		"--output", mergedFile,
	}
	if numThreads != 0 {
		args = append(args, "--num-threads", strconv.Itoa(numThreads))
	}
	args = append(args, profdataFiles...)
	mergeCmd := Action{Path: partitions[0].tool, Args: args}
	data, err := mergeCmd.Run(ctx)
	if err != nil {
		return fmt.Errorf("%s failed with %v:\n%s", mergeCmd.String(), err, string(data))
	}

	// Gather the set of modules and coverage files
	modules := []symbolize.FileCloser{}
	files := make(chan symbolize.FileCloser)
	malformedModules := make(chan string)
	s := make(chan struct{}, jobs)
	var wg sync.WaitGroup
	for _, entry := range entries {
		wg.Add(1)
		go func(module string) {
			defer wg.Done()
			s <- struct{}{}
			defer func() { <-s }()
			var file symbolize.FileCloser
			if err := retry.Retry(ctx, retry.WithMaxAttempts(retry.NewConstantBackoff(cloudFetchRetryBackoff), cloudFetchMaxAttempts), func() error {
				var err error
				file, err = repo.GetBuildObject(module)
				return err
			}, nil); err != nil {
				logger.Warningf(ctx, "module with build id %s not found: %v\n", module, err)
				return
			}
			if isInstrumented(file.String()) {
				// Run llvm-cov with the individual module to make sure it's valid.
				args := []string{
					"show",
					"-instr-profile", mergedFile,
					"-summary-only",
				}
				for _, remapping := range pathRemapping {
					args = append(args, "-path-equivalence", remapping)
				}
				args = append(args, file.String())
				showCmd := Action{Path: llvmCov, Args: args}
				data, err := showCmd.Run(ctx)
				if err != nil {
					logger.Warningf(ctx, "module %s returned err %v:\n%s", module, err, string(data))
					file.Close()
					malformedModules <- module
				} else {
					files <- file
				}
			} else {
				file.Close()
			}
		}(entry.Module)
	}
	go func() {
		wg.Wait()
		close(malformedModules)
		close(files)
	}()
	var malformed []string
	go func() {
		for m := range malformedModules {
			malformed = append(malformed, m)
		}
	}()
	for f := range files {
		modules = append(modules, f)
		// Make sure we close all modules in the case of error
		defer f.Close()
	}

	// Write the malformed modules to a file in order to keep track of the tests affected by fxbug.dev/74189.
	if err := ioutil.WriteFile(filepath.Join(tempDir, "malformed_binaries.txt"), []byte(strings.Join(malformed, "\n")), os.ModePerm); err != nil {
		return fmt.Errorf("failed to write malformed binaries to a file: %w", err)
	}

	// Make the llvm-cov response file
	covFile, err := os.Create(filepath.Join(tempDir, "llvm-cov.rsp"))
	if err != nil {
		return fmt.Errorf("creating llvm-cov.rsp file: %w", err)
	}
	for i, module := range modules {
		// llvm-cov expects a positional arg representing the first
		// object file before it processes the rest of the positional
		// args as source files, so we don't use an -object flag with
		// the first file.
		if i == 0 {
			fmt.Fprintf(covFile, "%s\n", module.String())
		} else {
			fmt.Fprintf(covFile, "-object %s\n", module.String())
		}
	}
	for _, srcFile := range srcFiles {
		fmt.Fprintf(covFile, "%s\n", srcFile)
	}
	covFile.Close()

	if outputDir != "" {
		// Make the output directory
		err := os.MkdirAll(outputDir, os.ModePerm)
		if err != nil {
			return fmt.Errorf("creating output dir %s: %w", outputDir, err)
		}

		// Produce HTML report
		args := []string{
			"show",
			"-format", outputFormat,
			"-instr-profile", mergedFile,
			"-output-dir", outputDir,
		}
		if compilationDir != "" {
			args = append(args, "-compilation-dir", compilationDir)
		}
		for _, remapping := range pathRemapping {
			args = append(args, "-path-equivalence", remapping)
		}
		args = append(args, "@"+covFile.Name())
		showCmd := Action{Path: llvmCov, Args: args}
		data, err := showCmd.Run(ctx)
		if err != nil {
			return fmt.Errorf("%v:\n%s", err, string(data))
		}
		logger.Debugf(ctx, "%s\n", string(data))
	}

	if reportDir != "" {
		// Make the export directory
		err := os.MkdirAll(reportDir, os.ModePerm)
		if err != nil {
			return fmt.Errorf("creating export dir %s: %w", reportDir, err)
		}

		stderrFilename := filepath.Join(tempDir, "llvm-cov.stderr.log")
		stderrFile, err := os.Create(stderrFilename)
		if err != nil {
			return fmt.Errorf("creating export %q: %w", stderrFilename, err)
		}
		defer stderrFile.Close()

		// Export data in machine readable format.
		var b bytes.Buffer
		args := []string{
			"export",
			"-instr-profile", mergedFile,
			"-skip-expansions",
		}
		if skipFunctions {
			args = append(args, "-skip-functions")
		}
		for _, remapping := range pathRemapping {
			args = append(args, "-path-equivalence", remapping)
		}
		args = append(args, "@"+covFile.Name())
		cmd := exec.Command(llvmCov, args...)
		cmd.Stdout = &b
		cmd.Stderr = stderrFile
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("failed to export: %w", err)
		}

		coverageFilename := filepath.Join(tempDir, "coverage.json")
		if err := ioutil.WriteFile(coverageFilename, b.Bytes(), 0644); err != nil {
			return fmt.Errorf("writing coverage %q: %w", coverageFilename, err)
		}

		var export llvm.Export
		if err := json.NewDecoder(&b).Decode(&export); err != nil {
			return fmt.Errorf("failed to load the exported file: %w", err)
		}

		var mapping *covargs.DiffMapping
		if diffMappingFile != "" {
			file, err := os.Open(diffMappingFile)
			if err != nil {
				return fmt.Errorf("cannot open %q: %w", diffMappingFile, err)
			}
			defer file.Close()

			if err := json.NewDecoder(file).Decode(mapping); err != nil {
				return fmt.Errorf("failed to load the diff mapping file: %w", err)
			}
		}

		files, err := covargs.ConvertFiles(&export, basePath, mapping)
		if err != nil {
			return fmt.Errorf("failed to convert files: %w", err)
		}

		if _, err := covargs.SaveReport(files, shardSize, reportDir); err != nil {
			return fmt.Errorf("failed to save report: %w", err)
		}
	}

	return nil
}

func main() {
	flag.Parse()

	log := logger.NewLogger(level, color.NewColor(colors), os.Stdout, os.Stderr, "")
	ctx := logger.WithLogger(context.Background(), log)

	var repo symbolize.CompositeRepo
	for _, dir := range buildIDDirPaths {
		repo.AddRepo(symbolize.NewBuildIDRepo(dir))
	}
	var fileCache *cache.FileCache
	if len(symbolServers) > 0 {
		if symbolCache == "" {
			log.Fatalf("-symbol-cache must be set if a symbol server is used")
		}
		var err error
		if fileCache, err = cache.GetFileCache(symbolCache, symbolCacheSize); err != nil {
			log.Fatalf("%v\n", err)
		}
	}
	for _, symbolServer := range symbolServers {
		// TODO(atyfto): Remove when all consumers are passing GCS URLs.
		if !strings.HasPrefix(symbolServer, "gs://") {
			symbolServer = "gs://" + symbolServer
		}
		cloudRepo, err := symbolize.NewCloudRepo(ctx, symbolServer, fileCache)
		if err != nil {
			log.Fatalf("%v\n", err)
		}
		cloudRepo.SetTimeout(cloudFetchTimeout)
		repo.AddRepo(cloudRepo)
	}

	if err := process(ctx, &repo); err != nil {
		log.Errorf("%v\n", err)
		os.Exit(1)
	}
}
