// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:convert' show jsonDecode;
import 'dart:core';
import 'dart:io' show Directory, File, Platform, Process, sleep;

import 'package:test/test.dart';

class ZedmonException implements Exception {
  final String message;

  ZedmonException(this.message);

  @override
  String toString() => 'ZedmonException: $message';
}

/// Description of the Zedmon device and host-side client.
// TODO(fxbug.dev/72454): Share this and other common client interfaces with
// sdk/testing/sl4f/client/lib/src/power.dart.
class ZedmonDescription {
  final double shuntResistance;
  final int timestampIndex;
  final int shuntVoltageIndex;
  final int busVoltageIndex;
  final int powerIndex;

  ZedmonDescription(this.shuntResistance, this.timestampIndex,
      this.shuntVoltageIndex, this.busVoltageIndex, this.powerIndex);
}

// Uses `zedmon describe` to create a ZedmonDescription.
Future<ZedmonDescription> getZedmonDescription(String zedmonPath) async {
  final result = await Process.run(zedmonPath, ['describe']);
  final description = jsonDecode(result.stdout);

  final csvFormat = description['csv_header'].split(',');
  for (var field in [
    'timestamp_micros',
    'shunt_voltage',
    'bus_voltage',
    'power'
  ]) {
    if (!csvFormat.contains(field)) {
      throw ZedmonException('CSV header does not contain field "$field".');
    }
  }

  return ZedmonDescription(
      description['shunt_resistance'],
      csvFormat.indexOf('timestamp_micros'),
      csvFormat.indexOf('shunt_voltage'),
      csvFormat.indexOf('bus_voltage'),
      csvFormat.indexOf('power'));
}

/// Individual timepoint of zedmon data.
class ZedmonRecord {
  /// Record timestamp, relative to the time the zedmon device started.
  int timestampMicros;

  /// Measured shunt voltage (Volts).
  double shuntVoltage;

  /// Measured bus voltage (Volts).
  double busVoltage;

  /// Measured power (Watts).
  double power;

  /// Parses a [ZedmonRecord] from a line of zedmon's CSV output.
  ZedmonRecord(String csvLine, ZedmonDescription desc) {
    final parts = csvLine.split(',');
    if (parts.length != 4) {
      throw ZedmonException(
          'Zedmon CSV line does not have 4 entries. Offending line:\n$csvLine');
    }
    timestampMicros = int.parse(parts[desc.timestampIndex]);
    shuntVoltage = double.parse(parts[desc.shuntVoltageIndex]);
    busVoltage = double.parse(parts[desc.busVoltageIndex]);
    power = double.parse(parts[desc.powerIndex]);
  }
}

void validateZedmonCsvLine(
    String csvLine, ZedmonDescription desc, double powerTolerance) {
  final record = ZedmonRecord(csvLine, desc);
  expect(record.busVoltage * record.shuntVoltage / desc.shuntResistance,
      closeTo(record.power, powerTolerance));
}

// Returns the average power measured by `zedmon record --average` for the
// specified number of seconds.
Future<double> measureAveragePower(String zedmonPath, String tempFilePath,
    ZedmonDescription desc, int seconds) async {
  final result = await Process.run(zedmonPath,
      ['record', '--out', tempFilePath, '--average', '${seconds}s']);
  expect(result.exitCode, equals(0));

  final lines = await File(tempFilePath).readAsLines();
  expect(lines.length, equals(1));

  final parts = lines[0].split(',');
  return double.parse(parts[desc.powerIndex]);
}

// In order to run these tests, the host should be connected to exactly one
// Zedmon device, satisfying:
//  - Hardware version 2.1 (version is printed on the board);
//  - Firmware built from the Zedmon repository's revision cdc9458f45, or
//    equivalent.
//
// The Zedmon must be connected to a test device that:
//  - Consumes a nontrivial amount of power (>1W will certainly suffice);
//  - Consumes nontrial power within 1 second of being connected to power;
//
// Zedmon's relay must be on (its default state) at the beginning of the test.
// The test device will be power-cycled in the course of testing.
Future<void> main() async {
  String zedmonPath;
  ZedmonDescription zedmonDescription;
  Directory tempDir;
  String tempFilePath;

  setUpAll(() async {
    zedmonPath = Platform.script.resolve('runtime_deps/zedmon').toFilePath();
    zedmonDescription = await getZedmonDescription(zedmonPath);
    tempDir = await Directory.current.createTemp();
    tempFilePath = '${tempDir.path}/zedmon.csv';
  });

  tearDown(() async {
    final file = File(tempFilePath);
    if (file.existsSync()) {
      file.deleteSync();
    }
  });

  // `zedmon list` should yield exactly one word, containing a serial number.
  test('zedmon list', () async {
    final result = await Process.run(zedmonPath, ['list']);
    expect(result.exitCode, equals(0));

    final regex = RegExp(r'\W+');
    expect(regex.allMatches(result.stdout).length, equals(1));
  });

  // Records 1 second of Zedmon data and validates the power calculation for
  // each line of output.
  test('zedmon record', () async {
    final result = await Process.run(
        zedmonPath, ['record', '--out', tempFilePath, '--duration', '1s']);
    expect(result.exitCode, equals(0));

    final lines = await File(tempFilePath).readAsLines();

    // Zedmon's nominal output rate is about 1500 Hz. Expecting 1400 lines
    // gives a bit of buffer for packet loss; see fxbug.dev/64161.
    expect(lines.length, greaterThan(1400));

    for (String line in lines) {
      validateZedmonCsvLine(line, zedmonDescription, 1e-4);
    }
  });

  test('zedmon record downsampled', () async {
    final result = await Process.run(zedmonPath, [
      'record',
      '--out',
      tempFilePath,
      '--duration',
      '1s',
      '--interval',
      '100ms'
    ]);
    expect(result.exitCode, equals(0));

    final lines = await File(tempFilePath).readAsLines();

    // We should see exactly 10 records, given a 100ms reporting interval over
    // a 1s duration. (Zedmon's nominal reporting interval is ~667us, so we'd
    // have to miss many consecutive packets before a downsampled packet is
    // skipped, and that would indicate a problem worth investigating.)
    expect(lines.length, equals(10));

    for (String line in lines) {
      // Power in each output record is averaged from the power derived from
      // each sample rather than computed from average shunt voltage and
      // average bus power. Consequently, the tolerance in the power calculation
      // needs to be higher here.
      validateZedmonCsvLine(line, zedmonDescription, 0.01);
    }
  });

  // Collects data over a brief interval using the --host_timestamps flag, and
  // checks that the timestamps are properly offset to lie between host time
  // instants before and after the Zedmon process runs.
  test('zedmon record host timestamps', () async {
    final zedmonArgs = [
      'record',
      '--out',
      tempFilePath,
      '--duration',
      '100ms',
      '--host_timestamps'
    ];

    final start = DateTime.now();
    final result = await Process.run(zedmonPath, zedmonArgs);
    final end = DateTime.now();

    expect(result.exitCode, equals(0));
    final lines = await File(tempFilePath).readAsLines();

    expect(lines.length, greaterThan(0),
        reason: 'No lines in output. stderr: "${result.stderr}"');

    for (String line in lines) {
      final record = ZedmonRecord(line, zedmonDescription);
      final recordTime =
          DateTime.fromMicrosecondsSinceEpoch(record.timestampMicros);

      expect(recordTime.isAfter(start), true,
          reason:
              'Record time $recordTime is not after process start time $start.');
      expect(recordTime.isBefore(end), true,
          reason:
              'Record time $recordTime is not before process end time $end.');
    }
  });

  // Tests that the 5-second average power drops by at least 99% when the
  // relay is turned off.
  test('zedmon relay', () async {
    var result = await Process.run(zedmonPath, ['relay', 'off']);
    expect(result.exitCode, equals(0));
    sleep(Duration(seconds: 1));
    final offPower = await measureAveragePower(
        zedmonPath, tempFilePath, zedmonDescription, 5);

    result = await Process.run(zedmonPath, ['relay', 'on']);
    expect(result.exitCode, equals(0));
    sleep(Duration(seconds: 1));
    final onPower = await measureAveragePower(
        zedmonPath, tempFilePath, zedmonDescription, 5);

    expect(offPower, lessThan(0.01 * onPower));
  });
}
