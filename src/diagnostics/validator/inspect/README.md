# Inspect Validator

Reviewed on: 2019-09-24

Inspect Validator exercises libraries that write Inspect VMOs and evaluates
the resulting VMOs for correctness and memory efficiency.

## Building

To add this project to your build, append `--with //src/diagnostics/validator/inspect:tests`
to the `fx set` invocation.

For example:

```
fx set core.chromebook-x64 --with '//topaz/bundles:buildbot' --with //src/diagnostics/validator/inspect:tests
```

The Rust puppet is `--with //src/diagnostics/validator/inspect/lib/rust:tests`.

The C++puppet is `--with //src/diagnostics/validator/inspect/lib/cpp:tests`.

## Running

Inspect Validator will be run as part of CQ/CI. To run manually, see "Testing".

Invoke with at least one "--url fuchsia-pkg://...." argument.
Also valid: "--output text" or "--output json" (defaults to json).

## Testing
To run unit tests:
```
--with //src/diagnostics/validator/inspect:tests
fx test inspect-validator-test
```
```
fx build && fx shell run_test_component fuchsia-pkg://fuchsia.com/inspect_validator_tests#meta/validator_bin_test.cmx && echo Success!
```

To run an integration test to evaluate the Rust Inspect library:
```
--with //src/diagnostics/validator/inspect/lib/rust:tests
fx test inspect-validator-test-rust
```
```
fx build && fx shell run fuchsia-pkg://fuchsia.com/inspect_validator_test_rust#meta/validator.cmx && echo Success!
```

To manually run one or more puppets by specifying their URLs (in this case, the Rust puppet):
```
--with //src/diagnostics/validator/inspect
fx build && fx shell run fuchsia-pkg://fuchsia.com/inspect_validator#meta/validator.cmx --url fuchsia-pkg://fuchsia.com/inspect_validator_test_rust#meta/inspect_validator_rust_puppet.cmx
```

## Source layout

The test entrypoint is located in `src/client.rs`. It connects to and controls
one or more "puppet" programs located under lib/(language) such as
lib/rust/src/main.rs. Since Dart is not currently supported in //src, its
puppet will be located at //sdk/dart/fuchsia_inspect/test/validator_puppet.
