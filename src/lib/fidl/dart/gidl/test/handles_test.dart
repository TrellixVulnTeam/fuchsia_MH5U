// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:test/test.dart';
import 'package:sdk.dart.lib.gidl/handles.dart';
import 'package:zircon/zircon.dart';

void main() {
  group('handle library tests', () {
    test('create single channel', () {
      final handles =
          createHandles([HandleDef(HandleSubtype.channel, ZX.RIGHT_TRANSFER)]);
      addTearDown(() {
        closeHandles(handles);
      });
      expect(handles.length, equals(1));
      expect(isHandleClosed(handles[0]), isFalse);
    });

    test('valid handle check', () {
      final pair = ChannelPair();
      expect(pair.status, equals(ZX.OK));
      // save the Handles first before closing them, which will set them to null
      final first = pair.first.handle;
      final second = pair.second.handle;
      expect(isHandleClosed(first), isFalse);
      expect(isHandleClosed(second), isFalse);

      pair.first.close();
      pair.second.close();

      expect(isHandleClosed(first), isTrue);
      expect(isHandleClosed(second), isTrue);
    });
  });
}
