// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:io' as dart_io;

import 'package:codesize/crash_handling.dart';
import 'package:codesize/io.dart';
import 'package:file/file.dart';
import 'package:file/memory.dart';
import 'package:process/process.dart';
import 'package:test/test.dart';

const _mockStatus = '❌ MOCK ❌';

class MockIo implements Io {
  static Map<Object, Object> inject() => {#io: MockIo()};

  final FileSystem _fs = MemoryFileSystem();

  final recordedOut = StringBuffer();

  @override
  StringSink get out => recordedOut;

  final recordedErr = StringBuffer();

  @override
  StringSink get err => recordedErr;

  @override
  void print(Object object) {
    dart_io.stdout.writeln(object);
  }

  @override
  ProcessManager get processManager => throw UnimplementedError();

  @override
  set exitCode(int code) => mockExitCode = code;

  @override
  String get cwd => '/cwd';

  static const Map<String, String> _environment = <String, String>{
    'FUCHSIA_DIR': '/root/path/fuchsia',
    'FUCHSIA_BUILD_DIR': '/root/path/fuchsia/out/default',
  };

  @override
  Map<String, String> get environment => _environment;

  @override
  File createTempFile(String name) =>
      _fs.systemTempDirectory.createTempSync().childFile(name);

  int mockExitCode = 0;
}

void main() {
  group('CrashHandling', () {
    test(
        'Catch known exception',
        () => runWithIo<MockIo, void>(() async {
              await withExceptionHandler(
                  () => throw KnownFailure('example failure'), _mockStatus);
              MockIo io = Io.get();

              // The exit code of a known exception.
              expect(io.mockExitCode, equals(2));

              // No crash logs should be created.
              expect(io._fs.systemTempDirectory.listSync().length, equals(0));

              expect(io.out.toString(), equals(''));
              expect(io.err.toString(), contains('example failure'));
              expect(io.err.toString(),
                  isNot(contains('Oops, `codesize` has crashed.')));
            }));

    test(
        'Catch unexpected error',
        () => runWithIo<MockIo, void>(() async {
              await withExceptionHandler(
                  () => throw Exception('unexpected error'), _mockStatus);
              MockIo io = Io.get();

              /// The exit code of an unexpected error.
              /// From https://fuchsia.dev/fuchsia-src/concepts/api/cli#execution_success_and_failure,
              /// exit code 1 is meant for general errors.
              expect(io.mockExitCode, equals(1));

              // Some crash logs should be created.
              final tmpDirs = io._fs.systemTempDirectory.listSync();
              expect(tmpDirs.length, equals(1));

              expect(
                  io.err.toString(), contains('Oops, `codesize` has crashed.'));

              expect(
                  io.err.toString(),
                  contains(r'''
Brief error description:
Exception: unexpected error
'''
                      .trim()));
              final crashLogPattern = RegExp(
                  '''
Exception: unexpected error

======== status ========
$_mockStatus

======== stack trace ========
#0.*crash_handling_test\\.dart.*
#1.*
              '''
                      .trim(),
                  multiLine: true);
              expect(
                  io._fs
                      .directory(tmpDirs.first)
                      .childFile('crash_report.txt')
                      .readAsStringSync(),
                  matches(crashLogPattern));
            }));
  });
}
