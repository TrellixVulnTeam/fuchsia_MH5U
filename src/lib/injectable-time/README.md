# `injectable-time`

`injectable-time` is a library to support dependency-injecting a time source.

It provides a trait `TimeSource` with one function, `now()`.  The library uses
i64 instead of an os-dependent time type so it can run host-side and in Fuchsia.

It provides three structs, `UtcTime`, `MonotonicTime`, and `FakeTime`,
which implement `TimeSource`. `FakeTime` has functions `set_ticks`
and `add_ticks` which can be used in tests.

The `now()` function of `FakeTime` returns the last number that was set_ticks by
`set_ticks()`. It does not increment on multiple calls to `now()`.

The `now()` function of `UtcTime` returns the number of nanoseconds since
the Unix epoch.

The `now()` function of `MonotonicTime` returns a number of nanoseconds
which monotonically increases at approximately a 1:1 rate with the wall clock.

Any struct that needs an injectable time source can store a
`&'a dyn TimeSource`. Note the lack of `mut`.

A struct can also simply store a T: TimeSource. FakeTime can be clone()'d.

See the unit tests in injectable_time.rs for a usage example.

## Building

This project should be automatically included in builds.

## Using

`injectable-time` can be used by depending on the
`//src/lib/injectable-time` gn target and then using
the `injectable-time` crate in a Rust project.

`injectable-time` is not available in the sdk and is intended to be used only by
diagnostics binaries.

## Testing

Unit tests for `injectable-time` are available in the
`injectable-time` package:

```
$ fx test injectable_time_lib_test
```

You'll need to include `//src/lib/injectable-time:tests` in your
build, either by using `fx args` to put it under `universe_package_labels`, or
by `fx set_ticks [....] --with //src/lib/injectable-time:tests`.
