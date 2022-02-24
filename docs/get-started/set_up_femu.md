# Start the Fuchsia emulator

This guide provides instructions on how to set up and launch the
Fuchsia emulator (FEMU) on your machine.

The steps are:

1. [Prerequisites](#prerequisites).
1. [Build Fuchsia for FEMU](#build-fuchsia-for-femu).
1. [Enable VM acceleration (Optional)](#enable-vm-acceleration).
1. [Start FEMU](#start-femu).
1. [Discover FEMU](#discover-femu).

## 1. Prerequisites {#prerequisites}

Running FEMU requires that you've completed the following guides:

 * [Download the Fuchsia source code][get-fuchsia-source]
 * [Configure and build Fuchsia][build-fuchsia]

## 2. Build Fuchsia for FEMU {#build-fuchsia-for-femu}

To run FEMU, you first need to build a Fuchsia system image that supports
the emulator environment. This guide uses `qemu-x64` for the board
and `workstation` for the product as an example.

To build a FEMU Fuchsia image, do the following:

1. Set the Fuchsia build configuration:

   ```posix-terminal
   fx set workstation.qemu-x64 --release
   ```

2. Build Fuchsia:

   ```posix-terminal
   fx build
   ```

For more information on supported boards and products, see the
[Fuchsia emulator (FEMU)][femu-overview] overview page.

## 3. Enable VM acceleration (Optional) {#enable-vm-acceleration}

(**Linux only**) Most Linux machines support VM acceleration through
KVM, which greatly improves the performance and usability of the emulator.

If KVM is available on your machine, update your group permission to
enable KVM.

* {Linux}

  To enable KVM on your machine, do the following:

  Note: You only need to do this once per machine.

  1.  Add yourself to the `kvm` group on your machine:

      ```posix-terminal
      sudo usermod -a -G kvm ${USER}
      ```

  1.  Log out of all desktop sessions to your machine and then log in again.

  1.  To verify that KVM is configured correctly, run the following command:

      ```posix-terminal
      if [[ -r /dev/kvm ]] && grep '^flags' /proc/cpuinfo | grep -qE 'vmx|svm'; then echo 'KVM is working'; else echo 'KVM not working'; fi
      ```

      Verify that this command prints the following line:

      ```none {:.devsite-disable-click-to-copy}
      KVM is working
      ```

      If you see `KVM not working`, you may need to reboot your machine for
      the permission change to take effect.

* {macOS}

  No additional setup is required for macOS.

  Instead of KVM, the Fuchsia emulator on macOS uses the
  [Hypervisor framework][hypervisor-framework]{: .external}.

## 4. Start FEMU {#start-femu}

Before you start the Fuchsia emulator, make sure you start the package server in another terminal:

Note: Alternatively you can background the `fx serve` process.

```posix-terminal
fx serve
```

Start the Fuchsia emulator on your machine.

* {Linux}

  The command below allows FEMU to access external networks:

  ```posix-terminal
  fx vdl start -N -u {{ '<var>' }}FUCHSIA_ROOT{{ '</var>' }}/scripts/start-unsecure-internet.sh
  ```

  Replace the following:

  * `FUCHSIA_ROOT`: The path to the Fuchsia checkout on your machine (for example, `~/fuchsia`).

  This command opens a new window with the title "Fuchsia Emulator".
  After the Fuchsia emulator is launched successfully, the terminal starts a
  SSH console. You can run shell commands on this console, as you would on a
  Fuchsia device.

  However, if you want to run FEMU without access to external network,
  run the following command instead:

  ```posix-terminal
  fx vdl start -N
  ```

  The `-N` option enables the `fx` tool to discover this FEMU instance as a device
  on your machine.

* {macOS}

  To start FEMU on macOS, do the following:

  1. Start FEMU:

     ```posix-terminal
     fx vdl start
     ```

     If you launch FEMU for the first time on your macOS (including after a reboot),
     a window pops up asking if you want to allow the process `aemu` to run on your
     machine. Click **Allow**.

     This command opens a new window with the title "Fuchsia Emulator".
     After the Fuchsia emulator is launched successfully, the terminal starts a
     SSH console. You can run shell commands on this console, as you would on a
     Fuchsia device.

     Additionally, the command's output includes an instruction that
     asks you to run `fx set-device`. Make a note of this instruction for the next step.

  2. Open a new terminal and run the `fx set-device` command to specify
     the launched Fuchsia emulator SSH port:

     ```posix-terminal
     fx set-device 127.0.0.1:{{ '<var>' }}SSH_PORT{{ '</var>' }}
     ```

     Replace the following:

     * `SSH_PORT`: Use the value from the `fx vdl start` command's output in
     Step 1.

## 5. Discover FEMU {#discover-femu}

To discover the Fuchsia emulator as a running Fuchsia device, run the
following command:

```posix-terminal
ffx target list
```

This command prints output similar to the following:

```none {:.devsite-disable-click-to-copy}
$ ffx target list
NAME                      SERIAL       TYPE                    STATE      ADDRS/IP                            RCS
fuchsia-5254-0063-5e7a    <unknown>    workstation.qemu-x64    Product    [fe80::866a:a5ea:cd9e:69f6%qemu]    N
```

`fuchsia-5254-0063-5e7a` is the default node name of the Fuchsia emulator.

## Next steps

To learn more about Fuchsia device commands and Fuchsia workflows, see
[Explore Fuchsia][explore-fuchsia].

## Appendices

This section provides additional FEMU options.

### See all available flags

To see a full list of supported flags:

```posix-terminal
fx vdl start --help
```

### Input options

By default FEMU uses multi-touch input. You can add the argument
`--pointing-device mouse` for mouse cursor input instead.

```posix-terminal
fx vdl start --pointing-device mouse
```

### Run FEMU without GUI support

If you don't need graphics or working under the remote workflow,
you can run FEMU in headless mode:

```posix-terminal
fx vdl start --headless
```

### Specify GPU used by FEMU

By default, FEMU launcher uses software rendering using
[SwiftShader][swiftshader]{: .external}. To force FEMU to use a specific
graphics emulation method, use the parameters `--host-gpu` or
`--software-gpu` to the `fx vdl start` command.

These are the valid commands and options:

<table><tbody>
  <tr>
   <th>GPU Emulation method</th>
   <th>Explanation</th>
   <th>Flag</th>
  </tr>
  <tr>
   <td>Hardware (host GPU)</td>
   <td>Uses the host machine’s GPU directly to perform GPU processing.</td>
   <td><code>fx vdl start --host-gpu</code></td>
  </tr>
  <tr>
   <td>Software (host CPU)</td>
   <td>Uses the host machine’s CPU to simulate GPU processing.</td>
   <td><code>fx vdl start --software-gpu</code></td>
  </tr>
</tbody></table>

### Reboot FEMU {#reboot-femu}

To reboot FEMU, run the following `ffx` command:

```posix-terminal
ffx target reboot
```

### Exit FEMU {#exit-femu}

To exit FEMU, run the following `ffx` command:

```posix-terminal
ffx target off
```

### Configure IPv6 network {#configure-ipv6-network}

This section provides instructions on how to configure an IPv6 network
for FEMU on Linux machine using [TUN/TAP][tuntap]{: .external}.

* {Linux}

  Note: The `fx vdl` command automatically performs the instructions below.

  To enable networking in FEMU using
  [tap networking][tap-networking]{: .external}, do the following:

  1. Set up `tuntap`:

     ```posix-terminal
     sudo ip tuntap add dev qemu mode tap user $USER
     ```

  1. Enable the network for `qemu`:

     ```posix-terminal
     sudo ip link set qemu up
     ```

* {macOS}

  No additional IPv6 network setup is required for macOS.

  [User Networking (SLIRP)][slirp]{: .external} is the default network setup
  for FEMU on macOS – while this setup does not support Fuchsia device
  discovery, you can still use `fx` tools (for example,`fx ssh`) to
  interact with your FEMU instance.

<!-- Reference links -->

[get-fuchsia-source]: /docs/get-started/get_fuchsia_source.md
[build-fuchsia]: /docs/get-started/build_fuchsia.md
[femu-overview]: /docs/development/build/emulator.md
[hypervisor-framework]: https://developer.apple.com/documentation/hypervisor
[explore-fuchsia]: /docs/get-started/explore_fuchsia.md
[swiftshader]: https://swiftshader.googlesource.com/SwiftShader/
[tuntap]: https://en.wikipedia.org/wiki/TUN/TAP
[tap-networking]: https://wiki.qemu.org/Documentation/Networking#Tap
[slirp]: https://wiki.qemu.org/Documentation/Networking#User_Networking_.28SLIRP.29
