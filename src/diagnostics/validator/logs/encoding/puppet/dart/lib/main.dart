// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:typed_data';

import 'package:fidl/fidl.dart';
import 'package:fidl_fuchsia_diagnostics_stream/fidl_async.dart' as fidl_stream;
import 'package:fidl_fuchsia_mem/fidl_async.dart' as fidl_mem;
import 'package:fidl_fuchsia_validate_logs/fidl_async.dart' as fidl_validate;
import 'package:fuchsia_diagnostic_streams/streams.dart' as streams;
import 'package:fuchsia_logger/logger.dart';
import 'package:fuchsia_services/services.dart';
import 'package:zircon/zircon.dart';

class _EncodingPuppetImpl extends fidl_validate.EncodingPuppet {
  final _binding = fidl_validate.EncodingPuppetBinding();

  static const int bufferSize = 1024;

  void bind(InterfaceRequest<fidl_validate.EncodingPuppet> request) {
    _binding.bind(this, request);
  }

  @override
  Future<fidl_mem.Buffer> encode(fidl_stream.Record record) async {
    ByteData bytes = ByteData(bufferSize);
    final len = streams.writeRecord(bytes, record);
    final vmo = SizedVmo.fromUint8List(bytes.buffer.asUint8List(0, len));
    return fidl_mem.Buffer(vmo: vmo, size: vmo.size);
  }
}

void main(List<String> args) {
  setupLogger();
  final context = ComponentContext.create();
  final validate = _EncodingPuppetImpl();

  context.outgoing
    ..addPublicService<fidl_validate.EncodingPuppet>(
        validate.bind, fidl_validate.EncodingPuppet.$serviceName)
    ..serveFromStartupInfo();
}
