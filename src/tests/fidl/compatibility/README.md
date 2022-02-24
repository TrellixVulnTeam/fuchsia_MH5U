# FIDL compatibility test

The FIDL compatibility test is an integration test for compatibility of
different FIDL bindings.

## How it works

- A "host" server, written in HLCPP, starts "language servers" in each of the
  languages that have FIDL bindings for.
- The host server is only implemented in HLCPP.

How `compatibility_test` does testing:

  1. The host server sends a message to language server 1.
  2. Language server 1 forwards that message to language server 2.
  3. Language server 2 echoes the message back to language server 1.
  4. Language server 1 sends the message back to the host.

In short, `compatibility_test` sends messages in the following way:

host to language server 1, then to language server 2, then back to language
server 1, and finally back to the host

The message that is sent is a huge FIDL struct that is designed to exercise
    encoding and decoding of every FIDL data type. See
    `compatibility_service.test.fidl` for the entry point.

By default, compatibility_test will set up every possible combination of pairs
of language servers defined.

For a test instance, there is:

  - HLCPP host server
  - Language server 1
  - Language server 2

So, if the language list is Go, Dart, and Rust, the following combinations of
servers will be tested:

  - HLCPP and Go, Go and Dart
  - HLCPP and Dart, Dart and Go
  - HLCPP and Go, Go and Rust
  - HLCPP and Rust, Rust and Go
  - HLCPP and Dart, Dart and Rust
  - HLCPP and Rust, Rust and Dart

The basic logic for the test is along the lines of:

```python
servers = ['go_server', 'cc_server', ...]

for proxy_name in servers:
  for server_name in servers:
    proxy = <connect to proxy>
    struct = <construct complicated struct>
    resp = proxy.EchoStruct(struct, server_name)
    assert_equal(struct, resp)
```

Servers should implement the service defined in
`compatibility_service.test.fidl` with logic along the lines of:

```python
def EchoStruct(
    Struct value, string forward_to_server, EchoStructCallback callback):
  if value.forward_to_server:
    other_server = <start server with LaunchPad>
    # set forward_to_server to "" to prevent recursion
    other_server.EchoStruct(value, "", callback)
  else:
    callback(value)
```

The logic for `EchoStructNoRetVal()` is similar. Instead of waiting for a
response directly, the test waits to receive an `EchoEvent()`. And instead of
calling the client back directly, the server sends the `EchoEvent()`.

The code for the compatibility tests are located at
`src/tests/fidl/compatibility`. It contains FIDL definitions and the server
implementations for Dart, HLCPP, LLCPP, Rust, and Go.

The test runner and all of the test server components are in a single package,
`fuchsia-pkg://fuchsia.com/fidl-compatibility-test`.

## Running

Beacuse individual tests have timeouts the test is split across 5 different 
components in the same package:

- `fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_struct.cmx`
- `fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_array.cmx`
- `fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_vector.cmx`
- `fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_table.cmx`
- `fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_union.cmx`

They all invoke the same test runner but use a `--gtest_filter` argument to filter which
tests are run.

To run all of the tests:

```sh
fx test fidl_compatibility_test
```

Or to run a specific test:

```sh
fx test "fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_struct.cmx"
```

And if you want to run a specific test case:

```sh
fx test "fuchsia-pkg://fuchsia.com/fidl-compatibility-test#meta/fidl_compatibility_test_struct.cmx" \
  -- --gtest_filter=Struct.EchoStructNoRetval
```

## Debugging

There are virtually no debugging messages apart from "failed" when one of the
language bindings fails to decode a message, which means that a lot of patching
in language bindings to dump the raw wire format bytes or a tool like
`fidlcat` is required to troubleshoot. A language binding failing decoding may
be due to a bug in the decoder of the receiver or a bug in the encoder in the
sender.

To use `fidlcat`:

```
fx fidlcat --remote-name=fidl
```

Then, run `compatibility_test`. Note: `fidlcat` will block until
`compatibility_test` is launched.
