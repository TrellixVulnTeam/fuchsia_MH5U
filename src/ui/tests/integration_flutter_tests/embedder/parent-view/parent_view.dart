// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:args/args.dart';
import 'package:fidl_fuchsia_sys/fidl_async.dart';
import 'package:fidl_fuchsia_ui_app/fidl_async.dart';
import 'package:fidl_fuchsia_ui_views/fidl_async.dart';
import 'package:flutter/material.dart';
import 'package:fuchsia_scenic_flutter/fuchsia_view.dart';
import 'package:fuchsia_services/services.dart';
import 'package:zircon/zircon.dart';

const _kChildAppUrl =
    'fuchsia-pkg://fuchsia.com/child-view#meta/child-view.cmx';

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  final parser = ArgParser()
    ..addFlag('showOverlay', defaultsTo: false)
    ..addFlag('hitTestable', defaultsTo: true)
    ..addFlag('focusable', defaultsTo: true)
    ..addFlag('useFlatland', defaultsTo: false);
  final arguments = parser.parse(args);
  for (final option in arguments.options) {
    print('parent-view: $option: ${arguments[option]}');
  }

  final useFlatland = arguments['useFlatland'];
  if (useFlatland) {
    final viewportCreationToken = _launchFlatlandApp(_kChildAppUrl);
    runApp(MaterialApp(
      debugShowCheckedModeBanner: false,
      home: TestApp(
        FuchsiaViewConnection.flatland(viewportCreationToken),
        showOverlay: arguments['showOverlay'],
        hitTestable: arguments['hitTestable'],
        focusable: arguments['focusable'],
      ),
    ));
  } else {
    final childViewToken = _launchGfxApp(_kChildAppUrl);
    runApp(MaterialApp(
      debugShowCheckedModeBanner: false,
      home: TestApp(
        FuchsiaViewConnection(childViewToken),
        showOverlay: arguments['showOverlay'],
        hitTestable: arguments['hitTestable'],
        focusable: arguments['focusable'],
      ),
    ));
  }
}

class TestApp extends StatelessWidget {
  static const _black = Color.fromARGB(255, 0, 0, 0);
  static const _blue = Color.fromARGB(255, 0, 0, 255);

  final FuchsiaViewConnection connection;
  final bool showOverlay;
  final bool hitTestable;
  final bool focusable;

  final _backgroundColor = ValueNotifier(_blue);

  TestApp(this.connection,
      {this.showOverlay = false,
      this.hitTestable = true,
      this.focusable = true});

  @override
  Widget build(BuildContext context) {
    return Listener(
      onPointerDown: (_) => _backgroundColor.value = _black,
      child: AnimatedBuilder(
          animation: _backgroundColor,
          builder: (context, snapshot) {
            return Container(
              color: _backgroundColor.value,
              child: Stack(
                alignment: Alignment.center,
                children: [
                  FractionallySizedBox(
                    widthFactor: 0.33,
                    heightFactor: 0.33,
                    child: FuchsiaView(
                      controller: connection,
                      hitTestable: hitTestable,
                      focusable: focusable,
                    ),
                  ),
                  if (showOverlay)
                    FractionallySizedBox(
                      widthFactor: 0.66,
                      heightFactor: 0.66,
                      child: Container(
                        alignment: Alignment.topRight,
                        child: FractionallySizedBox(
                          widthFactor: 0.5,
                          heightFactor: 0.5,
                          child: Container(
                            color: Color.fromARGB(255, 0, 255, 0),
                          ),
                        ),
                      ),
                    ),
                ],
              ),
            );
          }),
    );
  }
}

ViewportCreationToken _launchFlatlandApp(String componentUrl) {
  final incoming = Incoming();
  final componentController = ComponentControllerProxy();

  final launcher = LauncherProxy();
  Incoming.fromSvcPath()
    ..connectToService(launcher)
    ..close();
  launcher.createComponent(
    LaunchInfo(
      url: componentUrl,
      directoryRequest: incoming.request().passChannel(),
    ),
    componentController.ctrl.request(),
  );
  launcher.ctrl.close();

  ViewProviderProxy viewProvider = ViewProviderProxy();
  incoming
    ..connectToService(viewProvider)
    ..close();

  final viewTokens = ChannelPair();
  assert(viewTokens.status == ZX.OK);
  final viewportCreationToken = ViewportCreationToken(value: viewTokens.first);
  final viewCreationToken = ViewCreationToken(value: viewTokens.second);

  final createViewArgs = CreateView2Args(viewCreationToken: viewCreationToken);
  viewProvider.createView2(createViewArgs);
  viewProvider.ctrl.close();

  return viewportCreationToken;
}

ViewHolderToken _launchGfxApp(String componentUrl) {
  final incoming = Incoming();
  final componentController = ComponentControllerProxy();

  final launcher = LauncherProxy();
  Incoming.fromSvcPath()
    ..connectToService(launcher)
    ..close();
  launcher.createComponent(
    LaunchInfo(
      url: componentUrl,
      directoryRequest: incoming.request().passChannel(),
    ),
    componentController.ctrl.request(),
  );
  launcher.ctrl.close();

  ViewProviderProxy viewProvider = ViewProviderProxy();
  incoming
    ..connectToService(viewProvider)
    ..close();

  final viewTokens = EventPairPair();
  assert(viewTokens.status == ZX.OK);
  final viewHolderToken = ViewHolderToken(value: viewTokens.first);
  final viewToken = ViewToken(value: viewTokens.second);

  viewProvider.createView(viewToken.value, null, null);
  viewProvider.ctrl.close();

  return viewHolderToken;
}
