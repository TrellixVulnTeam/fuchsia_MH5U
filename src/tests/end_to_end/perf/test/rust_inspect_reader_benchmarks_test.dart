// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:logging/logging.dart';
import 'package:sl4f/trace_processing.dart';
import 'package:test/test.dart';

import 'helpers.dart';

const _testName = 'fuchsia.rust_inspect.reader_benchmarks';
const _appPath =
    'fuchsia-pkg://fuchsia.com/rust-inspect-benchmarks#meta/rust-inspect-benchmarks.cmx';
const _catapultConverterPath = 'runtime_deps/catapult_converter';
const _trace2jsonPath = 'runtime_deps/trace2json';

final _log = Logger('RustInspectReaderBenchmarks');

const _iterations = 300;

// TODO(fxbug.dev/53934): Share a MetricsProcessor with netstack_benchmarks_test.
List<TestCaseResults> _metricsProcessor(
    Model model, Map<String, dynamic> extraArgs) {
  final durations = filterEventsTyped<DurationEvent>(
    getAllEvents(model),
    category: 'benchmark',
    name: extraArgs['eventName'],
  ).map((event) => event.duration.toMillisecondsF()).toList();

  _log.info('Got ${durations.length} ${extraArgs['eventName']} events');

  // TODO(fxbug.dev/55161): Make the test fail if durations.length != _iterations.
  // Currently the benchmark stops working during Snapshot/256K/1mhz when
  // running on terminal-x64-release.
  if (durations.length != _iterations) {
    _log.severe('Expected $_iterations ${extraArgs['eventName']} events');
  }

  return durations.isEmpty
      ? []
      : [
          TestCaseResults(
              extraArgs['outputTestName'], Unit.milliseconds, durations)
        ];
}

void main() {
  enableLoggingOutput();

  test(_testName, () async {
    final helper = await PerfTestHelper.make();
    final stopwatch = Stopwatch();

    final traceSession =
        await helper.performance.initializeTracing(categories: ['benchmark']);
    await traceSession.start();

    _log.info(
        'Running: $_appPath --iterations $_iterations --benchmark reader');
    stopwatch.start();
    final result = await helper.component.launch(
        _appPath, ['--iterations', '$_iterations', '--benchmark', 'reader']);
    if (result != 'Success') {
      throw Exception('Failed to launch $_appPath.');
    }
    stopwatch.stop();
    _log.info('Completed $_iterations iterations in '
        '${stopwatch.elapsed.inSeconds} seconds.');

    // TODO(fxbug.dev/54931): Explicitly stop tracing.
    // await traceSession.stop();

    final fxtTraceFile = await traceSession.terminateAndDownload(_testName);
    final jsonTraceFile = await helper.performance
        .convertTraceFileToJson(_trace2jsonPath, fxtTraceFile);

    final List<MetricsSpec> metricsSpecs = [];

    for (final size in ['4K', '16K', '256K', '1M']) {
      for (final frequency in [
        '1hz',
        '10hz',
        '100hz',
        '1khz',
        '10khz',
        '100khz',
        '1mhz',
      ]) {
        metricsSpecs
          ..add(MetricsSpec(name: 'duration', extraArgs: {
            'eventName': 'Snapshot/$size/$frequency',
            'outputTestName': 'Snapshot/$size/$frequency',
          }))
          ..add(MetricsSpec(name: 'duration', extraArgs: {
            'eventName': 'SnapshotTree/$size/$frequency',
            'outputTestName': 'SnapshotTree/$size/$frequency',
          }));
      }
    }

    metricsSpecs
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'UncontendedSnapshotTree/4K',
        'outputTestName': 'UncontendedSnapshotTree/4K'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'UncontendedSnapshotTree/16K',
        'outputTestName': 'UncontendedSnapshotTree/16K'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'UncontendedSnapshotTree/256K',
        'outputTestName': 'UncontendedSnapshotTree/256K'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'UncontendedSnapshotTree/1M',
        'outputTestName': 'UncontendedSnapshotTree/1M'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'SnapshotTree/EmptyVMO',
        'outputTestName': 'SnapshotTree/EmptyVMO'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'SnapshotTree/QuarterFilledVMO',
        'outputTestName': 'SnapshotTree/QuarterFilledVMO'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'SnapshotTree/HalfFilledVMO',
        'outputTestName': 'SnapshotTree/HalfFilled_VMO'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'SnapshotTree/ThreeQuarterFilledVMO',
        'outputTestName': 'SnapshotTree/ThreeQuarterFilledVMO'
      }))
      ..add(MetricsSpec(name: 'duration', extraArgs: {
        'eventName': 'SnapshotTree/FullVMO',
        'outputTestName': 'SnapshotTree/FullVMO'
      }));

    await helper.performance.processTrace(
      MetricsSpecSet(metricsSpecs: metricsSpecs, testName: _testName),
      jsonTraceFile,
      converterPath: _catapultConverterPath,
      registry: {'duration': _metricsProcessor},
    );
  }, timeout: Timeout.none);
}
