#!/usr/bin/env python3.8
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import datetime
import json
import os
import re
import subprocess

from .corpus import Corpus
from .dictionary import Dictionary
from .host import Host
from .namespace import Namespace


class Fuzzer(object):
    """Represents a Fuchsia fuzz target.

    This represents a binary fuzz target produced the Fuchsia build, referenced
    by a component manifest, and included in a fuzz package.  It provides an
    interface for running the fuzzer in different common modes, allowing
    specific command line arguments to libFuzzer to be abstracted.

    Attributes:
        device:             The associated Device object.
        buildenv:           Alias for device.buildenv.
        host:               Alias for device.buildenv.host.
        package:            The GN fuzzers_package name (or package_name).
        executable:         The GN fuzzers name (or output_name).
        libfuzzer_opts:     "-key=val" options to pass to libFuzzer
        libfuzzer_inputs:   Files and directories to pass to libFuzzer.
        subprocess_args:    Arguments to pass to the fuzzer process.
        output:             Path under which the results of fuzzing are written.
        foreground:         Flag indicating whether to echo output.
        debug:              Flag indicating whether to allow debugging.
  """

    # Matches the prefixes in libFuzzer passed to |Fuzzer::DumpCurrentUnit| or
    # |Fuzzer::WriteUnitToFileWithPrefix|.
    ARTIFACT_PREFIXES = [
        'crash', 'leak', 'mismatch', 'oom', 'slow-unit', 'timeout'
    ]

    # Matches the options that cause libFuzzer to attach to the fuzzing process
    # as a debugger
    DEBUG_OPTIONS = [
        'handle_segv', 'handle_bus', 'handle_ill', 'handle_fpe', 'handle_abrt'
    ]

    # Wildcard pattern for fuzzer logs (only matches up to job 9, due to glob
    # limitations).
    LOG_PATTERN = 'fuzz-[0-9].log'

    # Default path for the ssh private key used to connect to a device.
    DEFAULT_SSH_KEY_PATH = '~/.ssh/fuchsia_ed25519'

    def __init__(self, factory, fuzz_spec):
        assert factory, 'Factory not set.'
        self._factory = factory
        self._label = fuzz_spec['label']
        self._package = fuzz_spec['package']
        self._package_url = fuzz_spec['package_url']
        self._package_path = None
        if 'fuzzer' in fuzz_spec:
            self._executable = fuzz_spec['fuzzer']
            manifest = fuzz_spec['manifest']
            self._is_test = False
        elif 'fuzzer_test' in fuzz_spec:
            # Infer the associated fuzzer metadata if it is currently being built as a fuzzer test.
            self._executable = re.sub(r'_test$', '', fuzz_spec['fuzzer_test'])
            manifest = re.sub(
                r'_test\.cmx$', '.cmx', fuzz_spec['test_manifest'])
            self._is_test = True

        self._executable_url = '{}#meta/{}'.format(self._package_url, manifest)
        self._ns = Namespace(self)
        self._corpus = Corpus(self, fuzz_spec.get('corpus'))
        self._dictionary = Dictionary(self)
        self._options = {'artifact_prefix': self.ns.data()}
        self._libfuzzer_opts = {}
        self._libfuzzer_inputs = []
        self._subprocess_args = []
        self._debug = False
        self._foreground = False
        self._output = None
        self._logbase = None
        self._last_known_pid = 0
        self._clusterfuzz_gcs_url = \
            'gs://corpus.internal.clusterfuzz.com/libFuzzer/fuchsia_{}-{}'.format(
                self._package, self._executable)
        self._realm_label = ''

    def __str__(self):
        return '{}/{}'.format(self.package, self.executable)

    def __lt__(self, other):
        return str(self) < str(other)

    @property
    def device(self):
        """The associated Device object."""
        return self._factory.device

    @property
    def buildenv(self):
        """Alias for device.buildenv."""
        return self._factory.buildenv

    @property
    def host(self):
        """Alias for device.buildenv.host."""
        return self._factory.host

    @property
    def label(self):
        """The GN fuzzer label."""
        return self._label

    @property
    def package(self):
        """The GN fuzzers_package name (or package_name)."""
        return self._package

    @property
    def package_url(self):
        return self._package_url

    @property
    def package_path(self):
        if not self._package_path:
            self.resolve()
        return self._package_path

    @property
    def executable(self):
        """The GN fuzzers name (or output_name)."""
        return self._executable

    @property
    def executable_url(self):
        return self._executable_url

    @property
    def is_test(self):
        return self._is_test

    @property
    def libfuzzer_opts(self):
        """"-key=val" options to pass to libFuzzer"""
        return self._libfuzzer_opts

    @libfuzzer_opts.setter
    def libfuzzer_opts(self, libfuzzer_opts):
        self._libfuzzer_opts = libfuzzer_opts

    @property
    def libfuzzer_inputs(self):
        """Additional files and directories to pass to libFuzzer"""
        return self._libfuzzer_inputs

    @libfuzzer_inputs.setter
    def libfuzzer_inputs(self, libfuzzer_inputs):
        self._libfuzzer_inputs = libfuzzer_inputs

    @property
    def subprocess_args(self):
        """Additional arguments to pass to the fuzzer process."""
        return self._subprocess_args

    @subprocess_args.setter
    def subprocess_args(self, subprocess_args):
        self._subprocess_args = subprocess_args

    @property
    def ns(self):
        return self._ns

    @property
    def corpus(self):
        return self._corpus

    @property
    def dictionary(self):
        return self._dictionary

    @property
    def output(self):
        """Path under which to write the results of fuzzing."""
        if not self._output:
            self._output = self.buildenv.abspath(
                '//local/{}_{}'.format(self._package, self._executable))
        return self._output

    @output.setter
    def output(self, output):
        if not output or not self.host.isdir(output):
            raise ValueError('Invalid output directory: {}'.format(output))
        self._output = output

    @property
    def foreground(self):
        """Flag indicating whether to echo output."""
        return self._foreground

    @foreground.setter
    def foreground(self, foreground):
        self._foreground = foreground

    @property
    def debug(self):
        """Flag indicating whether to allow a debugger to attach."""
        return self._debug

    @debug.setter
    def debug(self, debug):
        self._debug = debug

    @property
    def clusterfuzz_gcs_url(self):
        """Path to the GCS bucket which contains clusterfuzz corpora for this fuzzer."""
        return self._clusterfuzz_gcs_url

    @property
    def realm_label(self):
        """The realm-label to apply to test fuzzers when building coverage."""
        return self._realm_label

    @realm_label.setter
    def realm_label(self, realm_label):
        if not self.is_test:
            raise ValueError('Cannot set realm_label for a non-test fuzzer.')
        self._realm_label = realm_label

    def update(self, args):
        """Updates the properties of this fuzzer from matching arguments."""
        keys = [
            key for key, val in vars(Fuzzer).items()
            if isinstance(val, property) and val.fset
        ]
        for key, val in vars(args).items():
            if key in keys and val is not None:
                setattr(self, key, val)

    def matches(self, name):
        """Indicates if the given name matches part or all of this fuzzer's name.

        Parameters:
            name    The name to match. If the name is of the form 'x/y', it will match if 'x' is a
                    substring of `package` and y is a substring of `executable`. It will also
                    match if this is a fuzzer test, and y is a substring of `executable` + '_test'.
                    Otherwise it will match if name is a substring of either `package` or
                    `executable` (or again, `executable` + '_test' if a fuzzer test). A blank or
                    empty name always matches.

        Returns:
            A boolean indicating if the name matches this instance.
        """
        if not name or name == '':
            return True

        # If this object is currently built as a fuzzer test, check against that, too.
        executable = self.executable + '_test' if self.is_test else self.executable
        if '/' not in name:
            return name in self.package or name in executable
        names = name.split('/')
        if len(names) == 2:
            return names[0] in self.package and names[1] in executable
        raise ValueError('Malformed fuzzer name: ' + name)

    def is_resolved(self):
        if not self.device.reachable:
            return False
        if self._package_path:
            return True

        # Check if fuzzer built using '--with-base'.
        base_cmx = self.ns.base_abspath('meta/{}.cmx'.format(self.executable))
        if self.device.isfile(base_cmx):
            self._package_path = self.ns.base_abspath()
            return True

        # Check if fuzzer built using '--with'.
        cmd = ['pkgctl', 'pkg-status', self.package_url]
        process = self.device.ssh(cmd)
        process.stdout = subprocess.PIPE
        p = process.popen()
        out, _ = p.communicate()
        # Exit code 2 means "pkg in tuf repo but not on disk", which is not an
        # error for us (see sys/args.rs in pkgctl)
        if p.returncode not in (0, 2):
            raise subprocess.CalledProcessError(
                p.returncode, ' '.join(cmd), output=out)

        match = re.search(r'Package on disk: yes \(path=(.*)\)', str(out))
        if not match:
            return False
        self._package_path = match.group(1)
        return True

    def resolve(self):
        if self.is_resolved():
            return
        cmd = ['pkgctl', 'resolve', self.package_url]
        self.device.ssh(cmd, stdout=Host.DEVNULL).check_call()
        if self.is_resolved():
            return
        self.host.error('Failed to resolve package: {}'.format(self.package))

    def list_artifacts(self):
        """Returns a list of test unit artifacts in the output directory."""
        artifacts = []
        for prefix in Fuzzer.ARTIFACT_PREFIXES:
            artifacts += self.host.glob(
                os.path.join(self.output, '{}-*'.format(prefix)))
        return artifacts

    def is_running(self, refresh=False):
        """Checks the device and returns whether the fuzzer is running.

           See the note about "refresh" on Device.v1_component_is_running().
        """
        return self.device.v1_component_is_running(
            self.executable_url, refresh=refresh)

    def require_stopped(self):
        """Raise an exception if the fuzzer is running."""
        if self.is_running(refresh=True):
            self.host.error(
                '{} is running and must be stopped first.'.format(self))

    def _launch(self):
        """Launches and returns a running fuzzer process."""
        if not self.foreground:
            self._options['jobs'] = '1'
        for option in Fuzzer.DEBUG_OPTIONS:
            if self.debug:
                self._options[option] = '0'
        self._options.update(self._libfuzzer_opts)
        fuzz_cmd = ['run', self.executable_url]
        for key, value in sorted(self._options.items()):
            fuzz_cmd.append('-{}={}'.format(key, value))
        fuzz_cmd += self._libfuzzer_inputs
        if self._subprocess_args:
            fuzz_cmd += ['--']
            fuzz_cmd += self._subprocess_args

        # Start the process
        self.host.mkdir(self.output)
        cmd = self.device.ssh(fuzz_cmd)
        if self.foreground:
            cmd.stderr = subprocess.PIPE
        self._last_known_pid = 0
        proc = cmd.popen()
        if self.foreground:
            return proc

        # Wait for either the fuzzer to start and open its log, or exit.
        logs = self.ns.data(Fuzzer.LOG_PATTERN)
        while proc.poll() == None and not self.ns.ls(logs):
            self.host.sleep(0.5)
        if not self.ns.ls(logs):
            self.host.error('{} failed to start.'.format(self))
        return proc

    def logfile(self, job_num):
        """Returns the path to the symbolized log for a fuzzing job."""
        if not self._logbase:
            now = datetime.datetime.now().replace(microsecond=0)
            self._logbase = now.strftime('%Y-%m-%d-%H%M')
        logfile = 'fuzz-{}-{}.log'.format(self._logbase, job_num)
        return os.path.join(self.output, logfile)

    def start(self):
        """Runs the fuzzer.

      Executes a fuzzer in the "normal" fuzzing mode. If the fuzzer is being
      run in the foreground, it will block until the fuzzer exits. If the
      fuzzer is being run in the background, it will return immediately after
      the fuzzer has been started, and callers should subsequently call
      Fuzzer.monitor().

      The command will be like:
      run fuchsia-pkg://fuchsia.com/<pkg>#meta/<executable>.cmx \
        -artifact_prefix=data/ \
        -dict=pkg/data/<executable>/dictionary \
        data/corpus/

      See also: https://llvm.org/docs/LibFuzzer.html#running

      Returns:
        The fuzzer's process.
    """
        self.require_stopped()

        # Add dictionary
        if self.dictionary.nspath:
            self._options['dict'] = self.dictionary.nspath

        # Add corpus
        self._libfuzzer_inputs = self.corpus.nspaths

        # Prep output
        logs = self.ns.data(Fuzzer.LOG_PATTERN)
        self.ns.remove(logs)

        # When running in the foreground, interpret the output as coming from
        # fuzzing job 0. This makes the rest of the plumbing look the same.
        proc = self._launch()
        if self.foreground:
            self.symbolize_log(proc.stderr, 0, echo=True)
            proc.wait()
        return proc

    def symbolize_log(self, fd_in, job_num, echo=False):
        """Constructs a symbolized fuzzer log from a device.

        Merges the provided fuzzer log with the symbolized system log for the
        fuzzer process.

        Args:
          fd_in:        An object supporting readline(), such as a file or pipe.
          job_num:      The job number of the corresponding fuzzing job.
          echo:         If true, display text being written to fd_out.
        """
        filename_out = self.logfile(job_num)
        with self.host.open(filename_out, 'w') as fd_out:
            found = self._symbolize_log_impl(fd_in, fd_out, echo)
        self.host.link(
            filename_out, os.path.join(self.output, 'fuzz-latest.log'))
        return found

    def _symbolize_log_impl(self, fd_in, fd_out, echo):
        """Implementation of symbolize_log that takes file-like objects."""
        pid = -1
        sym = None
        artifacts = []
        pid_pattern = re.compile(r'^==([0-9]+)==')
        mut_pattern = re.compile(r'^MS: [0-9]*')  # Fuzzer::DumpCurrentUnit
        art_pattern = re.compile(r'Test unit written to (data/\S*)')
        for line in iter(fd_in.readline, ''):
            pid_match = pid_pattern.search(line)
            mut_match = mut_pattern.search(line)
            art_match = art_pattern.search(line)
            if pid_match:
                pid = int(pid_match.group(1))
                self._last_known_pid = pid
            if mut_match:
                if pid <= 0:
                    pid = self.device.guess_pid()
                if not sym:
                    raw = self.device.dump_log('--pid', str(pid))
                    sym = self.buildenv.symbolize(raw)
                    fd_out.write(sym)
                    if echo:
                        self.host.echo(sym.strip())
            if art_match:
                artifacts.append(art_match.group(1))
            fd_out.write(line)
            if echo:
                self.host.echo(line.strip())

        if artifacts:
            self.ns.fetch(self.output, *artifacts)

        return sym != None

    def monitor(self):
        """Waits for a fuzzer to complete and symbolizes its logs.

        Polls the device to determine when the fuzzer stops. Retrieves,
        combines and symbolizes the associated fuzzer and kernel logs. Fetches
        any referenced test artifacts, e.g. crashes.
        """
        while self.is_running(refresh=True):
            self.host.sleep(2)

        logs = self.ns.data(Fuzzer.LOG_PATTERN)
        self.ns.fetch(self.output, logs)
        self.ns.remove(logs)

        logs = self.host.glob(os.path.join(self.output, Fuzzer.LOG_PATTERN))
        for job_num, log in enumerate(logs):
            with self.host.open(log) as fd_in:
                self.symbolize_log(fd_in, job_num, echo=False)
            self.host.remove(log)

    def stop(self):
        """Stops any processes with a matching component manifest on the device."""
        if self.is_running():
            self.device.ssh(['killall', self.executable + '.cmx']).check_call()

    def repro(self):
        """Runs the fuzzer with test input artifacts.

        Executes a command like:
        run fuchsia-pkg://fuchsia.com/<package>#meta/<executable>.cmx \
        -artifact_prefix=data -jobs=1 data/<artifact>...

        The specified artifacts will be copied to the device instance and used.

        See also: https://llvm.org/docs/LibFuzzer.html#options
        """
        if not self._libfuzzer_inputs:
            self.host.error('No units provided.', 'Try "fx fuzz help".')

        # Device.store will glob patterns like 'crash-*'.
        self.require_stopped()
        self._libfuzzer_inputs = self.ns.store(
            self.ns.data(), *self._libfuzzer_inputs)

        # Default to repro-ing in the foreground
        self.foreground = True
        proc = self._launch()
        self.symbolize_log(proc.stderr, 0, echo=True)
        if proc.wait() == 0:
            return

        # Unlike in normal fuzzing mode, when reproing there is no reliable way
        # to detect a crash by grepping the output log. However, if we see that
        # the process exited uncleanly, we should go looking for a stacktrace.
        pid = self._last_known_pid
        if pid <= 0:
            pid = self.device.guess_pid()
        raw = self.device.dump_log('--pid', str(pid))
        sym = self.buildenv.symbolize(raw)
        self.host.echo(sym.strip())

    def analyze(self):
        """Collects coverage data for a finite amount of time."""

        # Run in the background for 1 minute, then print results.
        self._options['max_total_time'] = '60'
        self._options['print_coverage'] = '1'
        self.foreground = False
        proc = self.start()

        self.host.echo('Analyzing fuzzer...')
        delay = float(self._options['max_total_time']) / 79
        for i in range(79):
            self.host.echo('\r[' + ('#' * i).ljust(78) + ']', end='')
            self.host.sleep(delay)
        self.host.echo('')
        proc.wait()
        self.monitor()

    def add_corpus_to_buildfile(self, corpus_label):
        """Modifies this fuzzer's BUILD.gn to add or replace a corpus definition.

        Parameters:
            corpus_label    A GN label to a corpus.

        Returns:
            True if the fuzzer's BUILD.gn file and was updated with the corpus, else False.
        """
        if self._label.startswith('//generated'):
            self.host.echo(
                'Fuzzer metadata out of date; please re-run `fx set`.')
            return False
        parts = self._label.split(':')
        build_gn_dir = self.buildenv.abspath(parts[0])
        build_gn = os.path.join(build_gn_dir, 'BUILD.gn')
        if not self.host.isfile(build_gn):
            self.host.echo('No such file: ' + build_gn)
            return False

        corpus_dir = self.buildenv.abspath(corpus_label)
        if corpus_dir.startswith(build_gn_dir):
            corpus_dir = os.path.relpath(corpus_dir, build_gn_dir)
        else:
            corpus_dir = '//' + os.path.relpath(
                corpus_dir, self.buildenv.fuchsia_dir)

        found = False
        matching_target = False
        fuzzer_target = 'fuzzer("{}")'.format(parts[1])
        lines_out = []
        nested_scopes = 0
        with self.host.open(build_gn, 'r') as gn:
            for line in gn:
                line = line.rstrip()
                if not found and fuzzer_target in line:
                    matching_target = True
                if not matching_target:
                    lines_out.append(line)
                    continue
                nested_scopes += line.count('{')
                nested_scopes -= line.count('}')
                if nested_scopes == 0:
                    indent = len(line) - len(line.lstrip(' '))
                    indent = ' ' * indent
                    lines_out.append(
                        '{}  corpus = "{}"'.format(indent, corpus_dir))
                    matching_target = False
                    found = True
                if 'corpus =' not in line:
                    lines_out.append(line)

        if found:
            with self.host.open(build_gn, 'w') as gn:
                gn.write('\n'.join(lines_out))
            return True
        else:
            self.host.echo(
                'Unable to find \'{}\' in {}'.format(fuzzer_target, build_gn))
            return False

    def generate_coverage_report(self, local=False, input_dirs=None):
        """Replicates the steps in the fuchsia/coverage infra build recipe and
        runs them locally in order to build a more targetted coverage report."""

        # Ensure envvars are set for testrunner
        if self.device.ssh_identity:
            self.host.putenv('FUCHSIA_SSH_KEY', self.device.ssh_identity)
        else:
            self.host.error('Unable to determine ssh identity.')
        if self.device.addr:
            self.host.putenv('FUCHSIA_DEVICE_ADDR', self.device.addr)
        else:
            self.host.error('Unable to determine device address.')

        # TODO fxb/60971
        args_json_file = os.path.join(self.buildenv.build_dir, 'args.json')
        with self.host.open(args_json_file) as f:
            args = json.load(f)
            if (not 'select_variant' in args) or (not 'profile'
                                                  in args['select_variant']):
                self.host.error('Not built with profile variant.')

        # Set the realm_label so we can push corpus data to the device.
        if not local or input_dirs:
            self.realm_label = 'coverage'

        # Fetch and push clusterfuzz data.
        if not local:
            self.host.echo('Including corpus elements from clusterfuzz...')
            self.corpus.add_from_gcs(self.clusterfuzz_gcs_url)

        # Push local corpus data
        if input_dirs:
            for input_dir in input_dirs:
                if self.host.isdir(input_dir):
                    self.corpus.add_from_host(input_dir)
                else:
                    self.host.error(
                        '{} is not a directory, and cannot be used as corpus input.'
                    )

        # Ensure the output directory is created.
        self.host.mkdir(self.output)

        # Since the fuzzer is built as test, turn it back into the test form for testsharder
        test_executable_url = re.sub(r'.cmx', '_test.cmx', self.executable_url)
        shard_file = self.buildenv.testsharder(
            test_executable_url, self.output, self.realm_label)
        runner_dir, log_dump_file = self.buildenv.testrunner(
            shard_file, self.output, self.device)
        symbolize_out_file = os.path.join(self.output, 'symbolize_out')
        with self.host.open(log_dump_file) as f:
            self.buildenv.symbolize(f.read(), json_output=symbolize_out_file)

        coverage_dir = self.buildenv.covargs(
            runner_dir, symbolize_out_file, self.output)
        self.host.echo(
            'Generated coverage report, viewable at {}.'.format(
                '{}/index.html'.format(coverage_dir)))
