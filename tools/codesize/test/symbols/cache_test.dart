// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:io';

import 'package:codesize/symbols/cache.dart';
import 'package:test/test.dart';

void main() {
  group('Cache', () {
    test('pathForBuildId', () {
      expect(Cache(Directory('.build-id')).pathForBuildId('123456').path,
          equals('.build-id/12/3456.debug'));
    });

    test('pathForTempBuildId', () {
      expect(Cache(Directory('.build-id')).pathForTempBuildId('123456').path,
          equals('.build-id/123456.part'));
    });
  });
}
