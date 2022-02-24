#!/usr/bin/env python3.8
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import os
import re
import unittest

import test_env
from test_case import TestCaseWithFuzzer


class FuzzerTest(TestCaseWithFuzzer):

    def test_is_resolved(self):
        # The default package is automatically resolved; use another.
        self.create_fuzzer('fake-package2/fake-target1', resolve=False)

        cmd = ['pkgctl', 'pkg-status', self.fuzzer.package_url]
        self.assertFalse(self.fuzzer.is_resolved())
        self.assertSsh(*cmd)

        self.resolve_fuzzer()
        self.assertTrue(self.fuzzer.is_resolved())
        self.assertSsh(*cmd)

    def test_resolve(self):
        # The default package is automatically resolved; use another.
        self.create_fuzzer('fake-package2/fake-target1', resolve=False)
        status_cmd = ['pkgctl', 'pkg-status', self.fuzzer.package_url]
        resolve_cmd = ['pkgctl', 'resolve', self.fuzzer.package_url]

        # Resolution failure
        self.assertError(
            lambda: self.fuzzer.resolve(),
            'Failed to resolve package: fake-package2')
        self.assertSsh(*status_cmd)
        self.assertSsh(*resolve_cmd)

        # This is a little tricky: to test this, the first call to
        # 'pkgctl pkg-status' should fail, but the second, after a call to
        # 'pkgctl resolve', should succeed.
        package_path = '/pkgfs/versions/deadbeef'

        self.set_outputs(
            status_cmd, ['Package on disk: no'],
            end=10.0,
            ssh=True,
            returncode=2)
        self.set_outputs(
            status_cmd, ['Package on disk: yes (path={})'.format(package_path)],
            start=10.0,
            reset=False,
            ssh=True)

        process = self.get_process(resolve_cmd, ssh=True)
        process.duration = 10.0

        self.fuzzer.resolve()
        self.assertSsh(*status_cmd)
        self.assertSsh(*resolve_cmd)
        self.assertEqual(self.fuzzer.package_path, package_path)

    def test_list_artifacts(self):
        artifacts = ['crash-deadbeef', 'leak-deadfa11', 'oom-feedface']
        artifacts = [os.path.join(self.fuzzer.output, a) for a in artifacts]
        for artifact in artifacts:
            self.host.touch(artifact)
        self.assertEqual(self.fuzzer.list_artifacts(), artifacts)

    def test_is_running(self):
        fuzzer1 = self.create_fuzzer('check', 'fake-package1/fake-target1')
        fuzzer2 = self.create_fuzzer('check', 'fake-package1/fake-target2')

        self.assertFalse(fuzzer1.is_running())
        self.assertFalse(fuzzer2.is_running())

        self.set_running(fuzzer1.executable_url, duration=5)
        self.assertTrue(fuzzer1.is_running())
        self.assertFalse(fuzzer2.is_running())

        self.set_running(fuzzer2.executable_url, duration=5)
        self.assertTrue(fuzzer1.is_running())
        self.assertTrue(fuzzer2.is_running())

        # Results are cached until refresh
        self.host.sleep(5)
        self.assertTrue(fuzzer1.is_running())
        self.assertTrue(fuzzer2.is_running())

        self.assertFalse(fuzzer1.is_running(refresh=True))
        self.assertFalse(fuzzer2.is_running(refresh=True))

    def test_require_stopped(self):
        self.fuzzer.require_stopped()

        self.set_running(self.fuzzer.executable_url, duration=10)
        self.assertError(
            lambda: self.fuzzer.require_stopped(),
            'fake-package1/fake-target1 is running and must be stopped first.')
        self.host.sleep(10)
        self.fuzzer.require_stopped()

    def test_start(self):
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            'data/corpus',
        )

    def test_start_with_dictionary(self):
        self.touch_on_device(
            self.ns.resource_abspath(self.fuzzer.executable + '/dictionary'))
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-dict=pkg/data/fake-target1/dictionary',
            '-jobs=1',
            'data/corpus',
        )

    def test_start_with_seed_corpus(self):
        self.create_fuzzer('start', 'fake-package1/fake-target2')
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            'data/corpus',
            'pkg/data/src/fake/package1/target2-corpus',
        )

    def test_start_already_running(self):
        self.create_fuzzer('start', 'fake-package1/fake-target1')
        self.set_running(self.fuzzer.executable_url)
        self.assertError(
            lambda: self.fuzzer.start(),
            'fake-package1/fake-target1 is running and must be stopped first.')

    def test_start_foreground(self):
        self.create_fuzzer('start', str(self.fuzzer), '--foreground')
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            'data/corpus',
        )

    def test_start_debug(self):
        self.create_fuzzer('start', str(self.fuzzer), '--debug')
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-handle_abrt=0',
            '-handle_bus=0',
            '-handle_fpe=0',
            '-handle_ill=0',
            '-handle_segv=0',
            '-jobs=1',
            'data/corpus',
        )

    def test_start_with_options(self):
        self.create_fuzzer(
            'start', str(self.fuzzer), '-option2=foo', '-option1=\'bar baz\'')
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            '-option1=\'bar baz\'',
            '-option2=foo',
            'data/corpus',
        )

    def test_start_with_passthrough(self):
        self.create_fuzzer('start', str(self.fuzzer), '--', '-option1=bar')
        self.fuzzer.start()
        self.assertSsh(
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            'data/corpus',
            '--',
            '-option1=bar',
        )

    def test_start_failure(self):
        # Make the fuzzer fail after 15 "seconds".
        cmd = [
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            'data/corpus',
        ]
        process = self.get_process(cmd, ssh=True)
        process.duration = 15
        process.succeeds = False

        # Make the log appear after 20 "seconds".
        self.create_log(start=20)

        # The fuzzer should fail before the log appears.
        self.assertError(
            lambda: self.fuzzer.start(),
            'fake-package1/fake-target1 failed to start.')
        self.assertSsh(*cmd)
        self.assertEqual(self.host.elapsed, 15)

    def test_start_slow(self):
        # Make the log file appear after 15 "seconds".
        self.create_log(start=15)

        # Make the fuzzer fail after 20 "seconds".
        cmd = [
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            'data/corpus',
        ]
        process = self.get_process(cmd, ssh=True)
        process.duration = 20
        process.succeeds = False

        # The log should appear before the fuzzer fails.
        self.fuzzer.start()
        self.assertSsh(*cmd)
        self.assertEqual(self.host.elapsed, 15)

    # Helper to test Fuzzer.symbolize with different logs.
    MUTATION_SEQUENCE = 'MS: 1 SomeMutation; base unit: foo'
    SYMBOLIZER_OUTPUT = '[[symbolizer output]]'

    def symbolize_helper(self, inputs, job_num=0, echo=False):
        # Prime the sumbolization commnad
        has_mututation_seq = FuzzerTest.MUTATION_SEQUENCE in inputs
        if has_mututation_seq:
            cmd = self.symbolize_cmd()
            self.set_outputs(cmd, [FuzzerTest.SYMBOLIZER_OUTPUT])

        # Ensure the output directory exists, but the log file doesn't
        self.host.mkdir(self.fuzzer.output)
        self.assertFalse(
            self.host.isfile(
                os.path.join(self.fuzzer.output, 'fuzz-latest.log')))

        # Create the input log and symbolize it
        with self.host.open('unsymbolized', 'w+') as unsymbolized:
            for line in inputs:
                unsymbolized.write(line)
                unsymbolized.write('\n')
            unsymbolized.flush()
            unsymbolized.seek(0)
            self.assertEqual(
                has_mututation_seq,
                self.fuzzer.symbolize_log(unsymbolized, job_num, echo=echo))

        # The log file and a symlink should be created.
        self.assertTrue(self.host.isfile(self.fuzzer.logfile(job_num)))
        self.assertTrue(
            self.host.isfile(
                os.path.join(self.fuzzer.output, 'fuzz-latest.log')))

        # Symbolizer output is only inserted if a mutation sequence was present
        if has_mututation_seq:
            self.assertRan(*cmd)
            i = inputs.index(FuzzerTest.MUTATION_SEQUENCE)
            outputs = list(inputs[:i])
            outputs.append(FuzzerTest.SYMBOLIZER_OUTPUT)
            outputs += list(inputs[i:])
        else:
            outputs = list(inputs)
        with self.host.open(self.fuzzer.logfile(job_num)) as symbolized:
            self.assertEqual(symbolized.read().strip().split('\n'), outputs)

    def test_symbolize_log_no_mutation_sequence(self):
        self.symbolize_helper([
            'A line',
            'Another line',
            'Yet another line',
        ])

    def test_symbolize_log_no_process_id(self):
        self.symbolize_helper(
            [
                'A line',
                'Another line',
                FuzzerTest.MUTATION_SEQUENCE,
                'Yet another line',
                'artifact_prefix=\'data/\'; Test unit written to data/crash-aaaa',
            ])
        self.assertScpFrom(
            self.ns.data_abspath('crash-aaaa'), self.fuzzer.output)

    def test_symbolize_log_pid_from_stacktrace(self):
        self.symbolize_helper(
            [
                'A line',
                'Another line',
                '==12345== INFO: libfuzzer stack trace:',
                FuzzerTest.MUTATION_SEQUENCE,
                'Yet another line',
                'artifact_prefix=\'data/\'; Test unit written to data/crash-bbbb',
            ])
        self.assertScpFrom(
            self.ns.data_abspath('crash-bbbb'), self.fuzzer.output)

    def test_symbolize_log_pid_from_deadly_signal(self):
        self.symbolize_helper(
            [
                'A line',
                'Another line',
                '==67890== libFuzzer: deadly signal',
                FuzzerTest.MUTATION_SEQUENCE,
                'Yet another line',
                'artifact_prefix=\'data/\'; Test unit written to data/crash-cccc',
            ])
        self.assertScpFrom(
            self.ns.data_abspath('crash-cccc'), self.fuzzer.output)

    def test_symbolize_log_with_job_num(self):
        self.symbolize_helper(
            [
                '==67890== libFuzzer: deadly signal',
                FuzzerTest.MUTATION_SEQUENCE,
            ],
            job_num=13)

    def test_symbolize_log_with_echo(self):
        self.symbolize_helper(
            [
                '==67890== libFuzzer: deadly signal',
                FuzzerTest.MUTATION_SEQUENCE,
            ],
            echo=True)

        self.assertLogged(
            '==67890== libFuzzer: deadly signal',
            FuzzerTest.SYMBOLIZER_OUTPUT,
            FuzzerTest.MUTATION_SEQUENCE,
        )

    def test_monitor(self):
        self.create_fuzzer('start', 'fake-package1/fake-target2')
        self.set_running(self.fuzzer.executable_url, duration=15)

        # Make the file that scp grabs
        self.host.mkdir(self.fuzzer.output)

        logname = os.path.join(self.fuzzer.output, 'fuzz-0.log')
        with self.host.open(logname, 'w') as log:
            log.write('==67890== libFuzzer: deadly signal\n')
            log.write('MS: 1 SomeMutation; base unit: foo\n')

        # Make another log file to simulate the results of a previous run
        old_log = os.path.join(self.fuzzer.output, 'fuzz-1234-56-78-9012-0.log')
        self.host.touch(old_log)

        # Monitor the fuzzer until it exits
        self.fuzzer.monitor()
        self.assertGreaterEqual(self.host.elapsed, 15)

        # Verify we grabbed the logs and symbolized them.
        self.assertScpFrom(
            self.ns.data_abspath('fuzz-[0-9].log'), self.fuzzer.output)
        cmd = ['rm', '-f', self.ns.data_abspath('fuzz-[0-9].log')]
        self.assertSsh(*cmd)

        cmd = self.symbolize_cmd()
        self.assertRan(*cmd)

        # Log from scp should have been deleted
        self.assertFalse(self.host.isfile(logname))

        # Old log should be untouched
        self.assertTrue(self.host.isfile(old_log))
        self.assertFalse(self.host.isfile(self.fuzzer.logfile(1)))

        # New log and symlink should exist
        self.assertTrue(self.host.isfile(self.fuzzer.logfile(0)))
        self.assertTrue(
            self.host.isfile(
                os.path.join(self.fuzzer.output, 'fuzz-latest.log')))

    def test_stop(self):
        # Stopping when stopped has no effect
        self.create_fuzzer('stop', 'fake-package1/fake-target2')
        self.assertFalse(self.fuzzer.is_running())
        self.fuzzer.stop()
        self.assertFalse(self.fuzzer.is_running())

        self.set_running(self.fuzzer.executable_url)
        self.fuzzer.stop()
        self.assertSsh('killall', self.fuzzer.executable + '.cmx')

    def test_repro(self):
        # Globs should work, but only if they match files
        self.create_fuzzer(
            'repro',
            'package1/target2',
            'crash-*',
            'oom-feedface',
        )

        # Invalid units
        self.assertError(
            lambda: self.fuzzer.repro(),
            'No matching files: "crash-* oom-feedface".')

        # Valid units, but already running
        self.host.touch('crash-deadbeef')
        self.host.touch('crash-deadfa11')
        self.host.touch('oom-feedface')
        self.set_running(self.fuzzer.executable_url, duration=60)
        self.assertError(
            lambda: self.fuzzer.repro(),
            'fake-package1/fake-target2 is running and must be stopped first.')
        self.host.sleep(60)

        # Valid
        run_cmd = [
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            'data/crash-deadbeef',
            'data/crash-deadfa11',
            'data/oom-feedface',
        ]

        # Set a crashing return code
        self.set_outputs(run_cmd, [], returncode=77, ssh=True)

        # Set a symbolizer output
        self.set_outputs(self.symbolize_cmd(), ["symbolized"], returncode=0)

        # Because FakeProcess uses StringIO for synchronous simulation of stdio,
        # we need to preload the stderr so it is picked up by `symbolize_log`. (The
        # result of `set_outputs` wouldn't show up until a call to wait/poll).
        proc = self.get_process(run_cmd, ssh=True)._popen
        proc._stderr.write('==123== INFO: libFuzzer starting.\n')
        proc._stderr.seek(0)

        self.fuzzer.repro()
        self.assertSsh(*run_cmd)

        # Check that appropriate logs were fetched
        log_cmd = ['log_listener', '--dump_logs', '--pid', '123']
        self.assertSsh(*log_cmd)

        # Check that both the libFuzzer and symbolizer outputs were echoed to the
        # host
        self.assertLogged('==123== INFO: libFuzzer starting.', 'symbolized')

    def test_analyze(self):
        self.set_running(self.fuzzer.executable_url, duration=10)
        with self.assertRaises(SystemExit):
            self.fuzzer.analyze()
        self.host.sleep(10)

        # Make the log file appear right away. 'touch_on_device' is a bit of a
        # misleading name here, all it really does is prepare a canned response
        # to an 'ls' command over SSH. This uses the wildcarded log pattern,
        # since that is what is passed to `ls` when the fuzzer is starting.
        self.touch_on_device(self.ns.data_abspath('fuzz-[0-9].log'))

        # Make the fuzzer run for 60 "seconds".
        cmd = [
            'run',
            self.fuzzer.executable_url,
            '-artifact_prefix=data/',
            '-jobs=1',
            '-max_total_time=60',
            '-print_coverage=1',
            'data/corpus',
        ]
        process = self.get_process(cmd, ssh=True)
        process.duration = 60

        self.fuzzer.analyze()
        self.assertSsh(*cmd)

        # Round to the nearest microsecond
        self.assertEqual(round(self.host.elapsed, 6), 70)

    def test_add_corpus_to_buildfile_no_matching_target(self):
        # Missing BUILD.gn file and/or fuzzer target.
        label_parts = self.fuzzer.label.split(':')
        build_gn = self.buildenv.abspath(label_parts[0], 'BUILD.gn')
        self.assertFalse(self.fuzzer.add_corpus_to_buildfile('corpus/label'))
        self.assertLogged('No such file: ' + build_gn)

        lines_out = [
            '# No targets here',
        ]
        with self.host.open(build_gn, 'w') as f:
            f.write('\n'.join(lines_out))

        self.assertFalse(self.fuzzer.add_corpus_to_buildfile('corpus/label'))
        self.assertLogged(
            'Unable to find \'fuzzer("{}")\' in {}'.format(
                label_parts[1], build_gn))

    def test_add_corpus_to_buildfile_add_new(self):
        # Add a new 'corpus = "..."' to a fuzzer declaration.
        label_parts = self.fuzzer.label.split(':')
        build_gn = self.buildenv.abspath(label_parts[0], 'BUILD.gn')
        lines_out = [
            'fuzzer("{}") {{'.format(label_parts[1]),
            '  sources = [ "fuzzer.cc" ]',
            '  deps = [ ":my-lib" ]',
            '}',
        ]
        with self.host.open(build_gn, 'w') as f:
            f.write('\n'.join(lines_out))

        # Path is interpreted as relative to cwd as defined in FakeFactory
        self.assertTrue(self.fuzzer.add_corpus_to_buildfile('../corpus/label'))

        with self.host.open(build_gn) as f:
            self.assertEqual(
                f.read().split('\n'), [
                    'fuzzer("{}") {{'.format(label_parts[1]),
                    '  sources = [ "fuzzer.cc" ]',
                    '  deps = [ ":my-lib" ]',
                    '  corpus = "//corpus/label"',
                    '}',
                ])

    def test_add_corpus_to_buildfile_replace_existing(self):
        # Replace an existing 'corpus = "..."'.in a fuzzer declaration.
        label_parts = self.fuzzer.label.split(':')
        build_gn = self.buildenv.abspath(label_parts[0], 'BUILD.gn')
        lines_out = [
            'fuzzer("{}") {{'.format(label_parts[1]),
            '  sources = [ "fuzzer.cc" ]',
            '  deps = [ ":my-lib" ]',
            '  corpus = "//corpus/label"',
            '}',
        ]
        with self.host.open(build_gn, 'w') as f:
            f.write('\n'.join(lines_out))

        self.host.cwd = os.path.dirname(build_gn)
        self.assertTrue(self.fuzzer.add_corpus_to_buildfile('relative/path'))

        with self.host.open(build_gn) as f:
            self.assertEqual(
                f.read().split('\n'), [
                    'fuzzer("{}") {{'.format(label_parts[1]),
                    '  sources = [ "fuzzer.cc" ]',
                    '  deps = [ ":my-lib" ]',
                    '  corpus = "relative/path"',
                    '}',
                ])

    def test_generate_coverage_report_local(self):
        # Prerequisites
        fuzzer = self.create_fuzzer('1/1')

        # Assert that we need an ssh identity.
        # This cannot be tested since device is instantiated as part of the
        # factory workflow, and thus will have an ssh identity set.
        # self.assertError(
        #     lambda: self.fuzzer.generate_coverage_report(local=True),
        #     'Unable to determine ssh identity.'
        # )

        # Assert that we need a device address.
        # This cannot be tested since device is instantiated as part of the
        # factory workflow, and thus will have an address set.
        # self.assertError(
        #     lambda: self.fuzzer.generate_coverage_report(local=True),
        #     'Unable to determine device address.'
        # )

        # Assert that not building with profile variant causes an error.
        with self.host.open(self.buildenv.abspath(self.buildenv.build_dir,
                                                  'args.json'), 'w') as f:
            json.dump({'select_variant': ['not-profile']}, f)
        self.assertError(
            lambda: self.fuzzer.generate_coverage_report(local=True),
            'Not built with profile variant.')

        with self.host.open(self.buildenv.abspath(self.buildenv.build_dir,
                                                  'args.json'), 'w') as f:
            json.dump({'select_variant': ['profile']}, f)

        self._setup_coverage(fuzzer)

        self.fuzzer.generate_coverage_report(local=True, input_dirs=None)

    def test_generate_coverage_report_with_clusterfuzz(self):
        # Prerequisites
        fuzzer = self.create_fuzzer('1/1')
        fuzzer._is_test = True

        with self.host.open(self.buildenv.abspath(self.buildenv.build_dir,
                                                  'args.json'), 'w') as f:
            json.dump({'select_variant': ['profile']}, f)

        self._setup_coverage(fuzzer)

        with self.host.temp_dir() as temp_dir:
            # Fake retrieving something from gcs
            gcs_file = os.path.join(temp_dir.pathname, 'gcs_file')
            self.host.touch(gcs_file)

            self.fuzzer.generate_coverage_report(local=False, input_dirs=None)

            # Note: this takes advantage of the fact that the FakeCLI always returns
            # the same name for temp_dir().
            cmd = [
                'gsutil', '-m', 'cp', fuzzer.clusterfuzz_gcs_url + '/*',
                temp_dir.pathname
            ]
            self.assertRan(*cmd)

            # Assert that the corpus data file was copied to the device.
            device_file_path = '/data/r/sys/r/{}/fuchsia.com:{}:0#meta:{}_test.cmx/{}'.format(
                'coverage', fuzzer.package, fuzzer.executable, 'corpus')
            self.assertScpTo(gcs_file, device_file_path)

    def test_generate_coverage_report_with_local_corpus(self):
        # Prerequisites
        fuzzer = self.create_fuzzer('1/1')
        fuzzer._is_test = True

        with self.host.open(self.buildenv.abspath(self.buildenv.build_dir,
                                                  'args.json'), 'w') as f:
            json.dump({'select_variant': ['profile']}, f)

        self._setup_coverage(fuzzer)

        with self.host.temp_dir() as temp_dir:
            # Fake out a corpus on local disk
            input_dir = os.path.join(temp_dir.pathname, 'input_dir')
            self.host.mkdir(input_dir)
            host_corpus_file = os.path.join(input_dir, 'corpus1')
            self.host.touch(host_corpus_file)

            self.fuzzer.generate_coverage_report(
                local=True, input_dirs=[input_dir])

            # Assert that the corpus data file was copied to the device.
            device_file_path = '/data/r/sys/r/{}/fuchsia.com:{}:0#meta:{}_test.cmx/{}'.format(
                'coverage', fuzzer.package, fuzzer.executable, 'corpus')
            self.assertScpTo(host_corpus_file, device_file_path)

    def _setup_coverage(self, fuzzer):
        # Provide a sharded test definition with the fuzzer name turned back into a test
        testsharder_out_file = os.path.join(
            fuzzer.output, 'testsharder_out.json')
        test_executable_url = re.sub(
            r'.cmx', '_test.cmx', fuzzer.executable_url)
        shard_name = 'AEMU-unittest'
        tests = [{
            'name': test_executable_url,
            'meta': 'foo',
            'meta1': 'bar',
        }]
        with self.host.open(testsharder_out_file, 'w') as f:
            json.dump([{'name': shard_name, 'tests': tests}], f)

        # Create a bunch of files to allow the buildenv fns to pass its tests.
        # These fns are tested individually in buildenv_test.py and so we don't
        # do any verification aside from that fuzzer.generate_coverage_report is
        # passing these values around as expected.
        shard_file = os.path.join(
            fuzzer.output, 'shard_{}_tests.json'.format(shard_name))
        symbolize_file = os.path.join(fuzzer.output, 'symbolize_out')
        testrunner_dir = os.path.join(fuzzer.output, 'testrunner_out')
        summary_json_file = os.path.join(testrunner_dir, 'summary.json')
        self.host.touch(symbolize_file)
        self.host.touch(summary_json_file)

        # Capture the testrunner cmd to parse a validated pid from the output
        cmd = self.infra_testrunner_cmd(testrunner_dir, shard_file)
        fake_pid = 101
        self.set_outputs(
            cmd, [
                ('[123.456][{}][102][foo.cmx] INFO: [fuzzer_test.cc(35)] ' \
                'Fuzzer built as test: foo/bar').format(fake_pid)
            ],
            returncode=0,
            reset=True)


if __name__ == '__main__':
    unittest.main()
