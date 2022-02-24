# Run an end-to-end test

This guide provides instructions on how to run an end-to-end test for testing a
Fuchsia product.

This guide uses the Fuchsia emulator ([FEMU](/docs/get-started/set_up_femu.md)) to
emulate a device that runs Fuchsia. As for the end-to-end test, the guide uses
the
[`screen_is_not_black`](/src/tests/end_to_end/screen_is_not_black/)
end-to-end test.

The `screen_is_not_black` end-to-end test reboots a device under test
(DUT), waits 100 seconds, and takes a snapshot of the device’s screen. If the
snapshot image is not a black screen, the test concludes that Fuchsia is
successfully up and running on the device after reboot.

To run this end-to-end test, the steps are:

1.  [Prerequisites](#prerequisites).
1.  [Build a Fuchsia image to include the end-to-end test](#build-a-fuchsia-image-to-include-the-end-to-end-test).
1.  [Start the emulator with the Fuchsia image](#start-the-emulator-with-the-fuchsia-image).
1.   [Run the end-to-end test](#run-the-end-to-end-test).

Also, to run any end-to-end test, see the [Appendices](#appendices) section.

## 1. Prerequisites {#prerequisites}

This guide requires that you've completed the following guides:

*   [Download the Fuchsia source code](/docs/get-started/get_fuchsia_source.md).
*   [Start the Fuchsia emulator](/docs/get-started/set_up_femu.md).

## 2. Build a Fuchsia image to include the end-to-end test {#build-a-fuchsia-image-to-include-the-end-to-end-test}

Before you can run the `screen_is_not_black` end-to-end test, you first
need to build your Fuchsia image to include the test in the build artifacts:

Note: The examples in this guide use the `workstation` product. End-to-end tests work with most
products except `core`.

1.  To add the end-to-end test, run the `fx set` command with the following
    `--with` option:

    ```posix-terminal
    fx set workstation.qemu-x64 --with //src/tests/end_to_end/screen_is_not_black
    ```

    `//src/tests/end_to_end/screen_is_not_black` is a test directory in the
    Fuchsia source tree. The
    <code>[BUILD.gn](/src/tests/end_to_end/screen_is_not_black/BUILD.gn)</code>
    file in this directory defines the <code>screen_is_not_black</code> target
    to include the <code>screen_is_not_black</code> end-to-end test in the build
    artifacts.

1.  Build your Fuchsia image:

    ```posix-terminal
    fx build
    ```

    When the `fx build` command completes, the build artifacts now include the
    `screen_is_not_black` end-to-end test, which you can run from your host
    machine.

## 3. Start the emulator with the Fuchsia image {#start-the-emulator-with-the-fuchsia-image}

Start the emulator with your Fuchsia image and run a
[package repository server](/docs/development/build/fx.md#serve-a-build):

Note: The steps in this section assume that you don't have any terminals
currently running FEMU or the `fx serve` command.

1.  Configure an IPv6 network for the emulator (you only need to do this once):

    ```posix-terminal
    sudo ip tuntap add dev qemu mode tap user $USER && sudo ip link set qemu up
    ```

1.  In a new terminal, start the emulator:

    ```posix-terminal
    fx vdl start -N -u <PATH_TO_UPSCRIPT>
    ```

    Replace the following:

    * `PATH_TO_UPSCRIPT`: The path to a FEMU network setup script; for example,
    `~/fuchsia/scripts/start-unsecure-internet.sh`.

1.  Set the emulator to be your device:

    ```posix-terminal
    fx set-device
    ```

    If you have multiple devices, select `fuchsia-5254-0063-5e7a` (the emulator’s
    default device name), for example:

    <pre>
    $ fx set-device

    Multiple devices found, please pick one from the list:
    1) fuchsia-4407-0bb4-d0eb
    2) fuchsia-5254-0063-5e7a
    #? <b>2</b>
    New default device: fuchsia-5254-0063-5e7a
    </pre>

## 4. Run the end-to-end test {#run-the-end-to-end-test}

Run the `screen_is_not_black` end-to-end test:

```posix-terminal
fx test --e2e screen_is_not_black
```

When the test passes, this command prints output similar to the
following:

```none {:.devsite-disable-click-to-copy}
Saw a screen that is not black; waiting for 0:00:59.924543 now.

...

[FINE]: Running over ssh: killall sl4f.cmx
01:46 +1: All tests passed!
```

## Appendices {#appendices}

### Run any end-to-end test {#run-any-end-to-end-test}

Use the `fx test --e2e` command to run an end-to-end test from your host
machine:

```posix-terminal
fx test --e2e <TEST_NAME>
```

Some product configurations may include a set of end-to-end tests by default
(see [Examine product configuration files](#examine-product-configuration-files)).
However, if you want to run an end-to-end test that is not part of your
product configuration, configure your Fuchsia image to include the specific
test:

```posix-terminal
fx set <PRODUCT>.<BOARD> --with <TEST_DIRECTORY>:<TARGET>
```

For example, the following commands configure and build your Fuchsia image
with all the end-to-end tests in the
<code>[//src/tests/end_to_end/perf](/src/tests/end_to_end/perf/)</code> test
directory:

```none {:.devsite-disable-click-to-copy}
$ fx set workstation.qemu-x64 --with //src/tests/end_to_end/perf:test
$ fx build
```

Note: Some end-to-end tests are designed to run only on specific product
configurations.

For the list of all available end-to-end tests in the Fuchsia repository, see
the [//src/tests/end\_to\_end](/src/tests/end_to_end/) directory.

### Examine product configuration files {#examine-product-configuration-files}

To find out which end-to-end tests are included in a
specific product configuration, examine product configuration files (`.gni`) in
the Fuchsia repository's <code>[//products][products-dir]</code> directory.

The following example shows the product configurations files in the
`//products` directory:

```none {:.devsite-disable-click-to-copy}
~/fuchsia/products$ ls *.gni
bringup.gni  core.gni  terminal.gni  workstation.gni
```
To see the list of all available product configurations, you can run the
following command:

```posix-terminal
fx list-products
```

Among these product configurations, <code>[terminal][terminal-gni]</code> and
<code>[workstation][workstation-gni]</code> include end-to-end tests by
default. The following example shows the end-to-end tests included
in `terminal.gni`:

```none {:.devsite-disable-click-to-copy}
cache_package_labels += [
  ...
  "//src/tests/end_to_end/bundles:end_to_end_deps",
  "//src/tests/end_to_end/bundles:terminal_end_to_end_deps",
]

...

universe_package_labels += [
  "//src/tests/end_to_end/screen_is_not_black",
  "//src/tests/end_to_end/sl4f:test",
  "//src/tests/end_to_end/perf:test",
  ...
]
```

<!-- Reference links -->

[products-dir]: /products/
[terminal-gni]: /products/terminal.gni
[workstation-gni]: /products/workstation.gni
