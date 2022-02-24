// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:io';

import 'package:doc_checker/errors.dart';
import 'package:doc_checker/yaml_checker.dart';
import 'package:test/test.dart';

void main() {
  // Clean up the yaml files after every test
  tearDown(() {
    List<File> files = [
      File('${Directory.systemTemp.path}/_toc.yaml'),
      File('${Directory.systemTemp.path}/docs/_toc.yaml'),
      File('${Directory.systemTemp.path}/docs/_included.yaml'),
      File('${Directory.systemTemp.path}/docs/hello.md'),
      File('${Directory.systemTemp.path}/docs/world.md'),
      File('${Directory.systemTemp.path}/docs/zircon/good.md'),
      File('${Directory.systemTemp.path}/docs/objects/hello.md'),
      File('${Directory.systemTemp.path}/docs/objects/README.md'),
      File('${Directory.systemTemp.path}/docs/objects.md'),
    ];

    for (var f in files) {
      if (f.existsSync()) {
        f.deleteSync();
      }
    }
  });

  group('doc_checker yaml_checker tests', () {
    test('empty list of toc files', () async {
      YamlChecker checker =
          YamlChecker(Directory.systemTemp.path, null, [], [], '');
      bool ret = await checker.check();
      expect(ret, equals(true));
    });

    test('filter hidden directory', () {
      YamlChecker checker = YamlChecker('/usr/docs', null, [], [], '');
      final files = [
        '/usr/docs/_hidden/a.md',
        '/usr/docs/_hidden/b.md',
        '/usr/docs/not_hidden/c.md',
      ];
      expect(
          checker.filterHidden(files), equals(['/usr/docs/not_hidden/c.md']));
    });

    test('filter hidden file', () {
      YamlChecker checker = YamlChecker('/usr/_foo/docs', null, [], [], '');
      final files = [
        '/usr/_foo/docs/_a.md',
        '/usr/_foo/docs/b.md',
      ];
      expect(checker.filterHidden(files), equals(['/usr/_foo/docs/b.md']));
    });

    test('Happy path single yaml file', () async {
      const String contents = '''toc:
- title: Hello
  path: /docs/hello.md
- title: "World"
  path: /docs/world.md
''';
      File file = File('${Directory.systemTemp.path}/docs/_toc.yaml')
        ..createSync(recursive: true)
        ..writeAsStringSync(contents);
      File('${Directory.systemTemp.path}/docs/hello.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('hello');
      File('${Directory.systemTemp.path}/docs/world.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('world');
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      if (!ret) {
        print('Unexpected errors: ${checker.errors}');
      }
      expect(ret, equals(true));
      expect(checker.errors.isEmpty, equals(true));
    });

    test('Misspelled keywords', () async {
      const String contents = '''toc:
- tile: Hello
  path: /docs/hello.md
- title: "World"
  filepath: /docs/world.md
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(contents);
      File('${Directory.systemTemp.path}/docs/hello.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('hello');
      File('${Directory.systemTemp.path}/docs/world.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('world');
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();

      expect(ret, equals(false));
      expect(checker.errors[0].content, equals('Unknown Content Key tile'));
      expect(checker.errors[1].content, equals('Unknown Content Key filepath'));
      expect(checker.errors.length, equals(2));
    });

    test('Bad paths ', () async {
      const String content = '''toc:
- title: Hello
  path: /docs/hello.md
- title: "World"
  path: /docs/zircon/good.md
- title: "Source"
  path: /src/ref/1.cc
- title: Good
  path: /docs/hello.md
- title: Google
  path: https://google.com
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(content);
      File('${Directory.systemTemp.path}/docs/hello.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('hello');
      File('${Directory.systemTemp.path}/docs/zircon/good.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('good');
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      expect(ret, equals(false));
      expect(
          checker.errors[0].content,
          equals(
              'Path needs to start with \'/docs\' or \'/reference\', got /src/ref/1.cc'));
      expect(checker.errors.length, equals(1));
    });

    test('include happy path', () async {
      const String content = '''toc:
- title: Hello
  section:
  - include: /docs/included/_toc.yaml
''';

      const String includedContent = '''toc:
- title: World
  path: /docs/world.md
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(content);

      File('${Directory.systemTemp.path}/docs/included/_toc.yaml')
        ..createSync(recursive: true)
        ..writeAsStringSync(includedContent);
      File('${Directory.systemTemp.path}/docs/world.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('world');
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      expect(ret, equals(true));
    });

    test('include invalid path', () async {
      const String content = '''toc:
- title: Hello
  section:
  - include: /docs/included/_toc.yaml
''';

      const String includedContent = '''toc:
- title: World
  path: /src/sample/1.c
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(content);
      File('${Directory.systemTemp.path}/docs/included/_toc.yaml')
        ..createSync(recursive: true)
        ..writeAsStringSync(includedContent);
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      expect(ret, equals(false));
      expect(
          checker.errors[0].content,
          equals(
              'Path needs to start with \'/docs\' or \'/reference\', got /src/sample/1.c'));
    });

    test('include file not found', () async {
      const String content = '''toc:
- title: Hello
  section:
  - include: /docs/included_notfound/_toc.yaml
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(content);
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      expect(ret, equals(false));
      expect(checker.errors.length, equals(1));
      expect(
          checker.errors[0].content,
          startsWith(
              'FileSystemException: Cannot open file, path = \'${Directory.systemTemp.path}/docs/included_notfound/_toc.yaml\''));
    });

    // Test when there is a directory with the same name and there is a README.md in that
    // directory, and there is a markdown file with the name of the directory.
    // Ideally, there should not be this type of ambiguity, but it happens.
    test('file and dir with same name', () async {
      const String content = '''toc:
- title: Objects readme
  path: /docs/objects
- title: readme
  path: /docs/objects/README.md
- title: Objects
  path: /docs/objects.md
- title: "Hello objects"
  path: /docs/objects/hello.md
''';

      File file = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(content);
      File('${Directory.systemTemp.path}/docs/objects/README.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('readme objects');
      File('${Directory.systemTemp.path}/docs/objects/hello.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('hello objects');
      File('${Directory.systemTemp.path}/docs/objects.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('objects');
      YamlChecker checker = YamlChecker(Directory.systemTemp.path,
          file.absolute.path, [file.absolute.path], [], '');
      bool ret = await checker.check();
      if (!ret) {
        print('Unexpected failure: ${checker.errors}');
      }
      expect(ret, equals(true));
    });

    test('non-fuchsia.dev yaml files', () async {
      const String tocYaml = '''toc:
- title: Objects
  path: /docs/objects.md
''';

      const String otherYaml = '''rfcs:
- name: "RFC-0001"
  path: /docs/rfc001.md
''';

      File tocFile = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(tocYaml);
      File('${Directory.systemTemp.path}/docs/objects.md')
        ..createSync(recursive: true)
        ..writeAsStringSync('objects');
      File otherFile =
          File('${Directory.systemTemp.path}/docs/contribute/_rfcs.yaml')
            ..createSync(recursive: true)
            ..writeAsStringSync(otherYaml);
      YamlChecker checker = YamlChecker(
          Directory.systemTemp.path,
          tocFile.absolute.path,
          [tocFile.absolute.path, otherFile.absolute.path],
          [],
          '');
      bool ret = await checker.check();
      if (!ret) {
        print('Unexpected failure: ${checker.errors}');
      }
      expect(ret, equals(true));
    });

    test('/reference link in yaml', () async {
      const String tocYaml = '''toc:
- title: Reference
  path: /reference/tools/sdk/fidlc.md
''';

      File tocFile = File('${Directory.systemTemp.path}/_toc.yaml')
        ..writeAsStringSync(tocYaml);
      YamlChecker checker = YamlChecker(
          Directory.systemTemp.path,
          tocFile.absolute.path,
          [tocFile.absolute.path],
          [],
          'https://fuchsia.dev');
      bool ret = await checker.check();
      if (!ret) {
        print('Unexpected failure: ${checker.errors}');
      }
      expect(ret, equals(true));
    });

    test('non-fuchsia.dev yaml file with invalid syntax', () async {
      const String otherYaml = '''rfcs:
X name "RFC-0001"
  path /docs/rfc001.md
''';

      File otherFile =
          File('${Directory.systemTemp.path}/docs/contribute/_rfcs.yaml')
            ..createSync(recursive: true)
            ..writeAsStringSync(otherYaml);
      YamlChecker checker = YamlChecker(
          Directory.systemTemp.path, null, [otherFile.absolute.path], [], '');
      bool ret = await checker.check();
      expect(ret, equals(false));
      expect(checker.errors.length, equals(1));
      expect(checker.errors[0].type, ErrorType.unparseableYaml);
    });

    test('root yaml is not a _toc.yaml', () async {
      File otherFile =
          File('${Directory.systemTemp.path}/docs/contribute/_rfcs.yaml')
            ..createSync(recursive: true)
            ..writeAsStringSync('');

      expect(
          () => YamlChecker(
              Directory.systemTemp.path, otherFile.absolute.path, [], [], ''),
          throwsA(isA<AssertionError>()));
    });
  }); // group
}
