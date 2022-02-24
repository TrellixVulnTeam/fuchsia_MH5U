#!/bin/bash
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Tests that femu is able to correctly interact with the fx emu command and
# its dependencies like fvm and aemu. These tests do not actually start up
# the emulator, but check the arguments are as expected.

set -e

# Specify a simulated CIPD instance id for prebuilts
AEMU_VERSION="bid:unknown"
AEMU_LABEL="$(echo "${AEMU_VERSION}" | tr ':/' '_')"
GRPCWEBPROXY_VERSION="git_revision:unknown"
GRPCWEBPROXY_LABEL="$(echo "${GRPCWEBPROXY_VERSION}" | tr ':/' '_')"

function run_femu_wrapper() {
  # femu.sh will run "fvm decompress" to convert the given fvm image format into
  # an intermediate raw image suffixed by ".decompress". The image is then used for
  # extension. Since the fvm tool is faked and does nothing in the test, we need
  # to fake the intermediate decompressed image.
  cp "${FUCHSIA_WORK_DIR}/image/storage-full.blk" \
    "${FUCHSIA_WORK_DIR}/image/storage-full.blk.decompressed"
  gn-test-run-bash-script "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/femu.sh" "$@"
}

# Verifies that the correct emulator command is run by femu, do not activate the network interface
TEST_femu_standalone() {

  # Run command.
  BT_EXPECT run_femu_wrapper

  # Verify that the image first goes through a decompress process by fvm.
  # shellcheck disable=SC1090
  source "${MOCKED_FVM}.mock_state.1"
  gn-test-check-mock-args _ANY_ _ANY_ decompress --default _ANY_

  # Verify that the image will be extended to double the size.
  # shellcheck disable=SC1090
  source "${MOCKED_FVM}.mock_state.2"
  gn-test-check-mock-args _ANY_ _ANY_ extend --length 2048 --length-is-lowerbound

  # Check that fpave.sh was called to download the needed system images
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/fpave.sh.mock_state"
  gn-test-check-mock-args _ANY_ --prepare --image qemu-x64 --bucket fuchsia --work-dir "${FUCHSIA_WORK_DIR}"

  # Verify that zbi was called to add the authorized_keys
  # shellcheck disable=SC1090
  source "${MOCKED_ZBI}.mock_state"
  gn-test-check-mock-args _ANY_ -o _ANY_ "${FUCHSIA_WORK_DIR}/image/zircon-a.zbi" --entry "data/ssh/authorized_keys=${HOME}/.ssh/fuchsia_authorized_keys"

  # Verify some of the arguments passed to the emulator binary
  # shellcheck disable=SC1090
  source "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}/emulator.mock_state"
  gn-test-check-mock-partial -fuchsia
}

# Verifies that the --experiment-arm64 option selects arm64 support
TEST_femu_arm64() {
  BT_EXPECT run_femu_wrapper \
    --experiment-arm64 \
    --image qemu-arm64 \
    --software-gpu

  # Check that fpave.sh was called to download the needed system images
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/fpave.sh.mock_state"
  gn-test-check-mock-args _ANY_ --prepare --image qemu-arm64 --bucket fuchsia --work-dir "${FUCHSIA_WORK_DIR}"

  # Verify some of the arguments passed to the emulator binary
  # shellcheck disable=SC1090
  source "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}/emulator.mock_state"
  gn-test-check-mock-partial -fuchsia
  gn-test-check-mock-partial -avd-arch arm64
  gn-test-check-mock-partial -cpu cortex-a53
  gn-test-check-mock-partial -gpu swiftshader_indirect
}

# Verifies that if /dev/kvm is available, that KVM support is configured automatically
TEST_femu_kvm_on() {
  # Run command, the DEV_KVM_FAKE file is created by default in BT_INIT_TEMP_DIR
  BT_EXPECT run_femu_wrapper

  # Verify that the correct KVM arguments are passed to the emulator binary
  # shellcheck disable=SC1090
  source "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}/emulator.mock_state"
  gn-test-check-mock-partial -fuchsia
  if is-mac; then
      gn-test-check-mock-partial -enable-hvf -cpu Haswell
      # TODO(fxbug.dev/74233): This is a workaround to solve an emulator crash
      # when using HVF accelerator. We need to remove this once the fix is
      # landed on host emulator.
      gn-test-check-mock-arg-has-substr kernel.page-scanner.page-table-eviction-policy=never
  else
      gn-test-check-mock-partial -enable-kvm -cpu host,migratable=no,+invtsc
  fi
}

# Verifies that if we manually override kvm, that appropriate emulation is configured
TEST_femu_kvm_off() {
  # Run command, but temporarily remove the DEV_KVM_FAKE file to force KVM to not be available,
  # and then specify no acceleration to check we get the right emulation.
  rm "${DEV_KVM_FAKE}"
  BT_EXPECT run_femu_wrapper -a off
  touch "${DEV_KVM_FAKE}"

  # Verify that the correct CPU emulation arguments are passed to the emulator binary
  # shellcheck disable=SC1090
  source "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}/emulator.mock_state"
  gn-test-check-mock-partial -fuchsia
  gn-test-check-mock-partial -cpu Haswell,+smap,-check,-fsgsbase
}

# Verifies that the correct emulator command is run by femu, along with the image setup.
# This tests the -N option for networking.
TEST_femu_networking() {

  # Create fake "ip tuntap show" command to let fx emu know the network is configured with some mocked output
  cat >"${PATH_DIR_FOR_TEST}/ip.mock_side_effects" <<INPUT
echo "qemu: tap persist user 238107"
INPUT

  # OSX may not have the tun/tap driver installed, and you cannot bypass the
  # network checks, so need to work around this for the test. Linux does not
  # need a fake network because we use a fake ip command.
  if is-mac && [[ ! -c /dev/tap0 || ! -w /dev/tap0 ]]; then
    NETWORK_ARGS=( -N -I fakenetwork )
  else
    NETWORK_ARGS=( -N )
  fi

  # Run command.
  BT_EXPECT run_femu_wrapper \
    "${NETWORK_ARGS[*]}" \
    --unknown-arg1-to-qemu \
    --authorized-keys "${BT_TEMP_DIR}/scripts/sdk/gn/base/testdata/authorized_keys" \
    --unknown-arg2-to-qemu

  # Verify that the image first goes through a decompress process by fvm.
  # shellcheck disable=SC1090
  source "${MOCKED_FVM}.mock_state.1"
  gn-test-check-mock-args _ANY_ _ANY_ decompress --default _ANY_

  # Verify that the image will be extended to double the size.
  # shellcheck disable=SC1090
  source "${MOCKED_FVM}.mock_state.2"
  gn-test-check-mock-args _ANY_ _ANY_ extend --length 2048 --length-is-lowerbound

  # Check that fpave.sh was called to download the needed system images
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/fpave.sh.mock_state"
  gn-test-check-mock-args _ANY_ --prepare --image qemu-x64 --bucket fuchsia --work-dir "${FUCHSIA_WORK_DIR}"

  # Verify that zbi was called to add the authorized_keys
  # shellcheck disable=SC1090
  source "${MOCKED_ZBI}.mock_state"
  gn-test-check-mock-args _ANY_ -o _ANY_ "${FUCHSIA_WORK_DIR}/image/zircon-a.zbi" --entry "data/ssh/authorized_keys=${BT_TEMP_DIR}/scripts/sdk/gn/base/testdata/authorized_keys"

  # Verify some of the arguments passed to the emulator binary
  # shellcheck disable=SC1090
  source "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}/emulator.mock_state"
  # The mac address is computed with a hash function in fx emu based on the device name.
  # We test the generated mac address since other scripts hard code this to SSH into the device.
  gn-test-check-mock-partial -fuchsia
  if is-mac; then
    if [[ -c /dev/tap0 && -w /dev/tap0 ]]; then
      gn-test-check-mock-partial -netdev type=tap,ifname=tap0,id=net0,script="${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/devshell/lib/emu-ifup-macos.sh",downscript=no
      gn-test-check-mock-partial -device virtio-net-pci,vectors=8,netdev=net0,mac=52:54:00:4d:27:96
    else
      gn-test-check-mock-partial -netdev type=tap,ifname=fakenetwork,id=net0,script="${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/devshell/lib/emu-ifup-macos.sh",downscript=no
      gn-test-check-mock-partial -device virtio-net-pci,vectors=8,netdev=net0,mac=52:54:00:95:03:66
    fi
  else
    gn-test-check-mock-partial -netdev type=tap,ifname=qemu,id=net0,script=no,downscript=no
    gn-test-check-mock-partial -device virtio-net-pci,vectors=8,netdev=net0,mac=52:54:00:63:5e:7a
  fi
  gn-test-check-mock-partial --unknown-arg1-to-qemu
  gn-test-check-mock-partial --unknown-arg2-to-qemu
}

TEST_femu_unsecure_internet() {
  # Most of the commands used go through a sudo wrapper
  cat >"${PATH_DIR_FOR_TEST}/ip.mock_side_effects" <<INPUT
echo "qemu: tap persist user 238107"
INPUT
  cat >"${PATH_DIR_FOR_TEST}/sudo.mock_side_effects" <<INPUT
echo "\$@"
INPUT
  cat >"${PATH_DIR_FOR_TEST}/hostname.mock_side_effects" <<INPUT
echo "test.nongoogle.com"
INPUT
  cat >"${PATH_DIR_FOR_TEST}/lsb_release.mock_side_effects" <<INPUT
echo "unknown"
INPUT

  # For all start-unsecure-internet.sh tests, make sure we use --cleanup so
  # we don't try to run the whole script. All the sudo commands are mocked,
  # but there is a cat /proc/sys/net/ipv4/ip_forward which is not mocked and
  # if a machine doesn't have this set the script will fail.

  # Run the upscript with a fake qemu network interface argument
  BT_EXPECT "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/start-unsecure-internet.sh" "--cleanup" "fakenetwork" >/dev/null 2>/dev/null

  # Test for some of the commands being run. There are a lot of commands that
  # are run through sudo, and the .N values for each command differ between
  # machines, so these are difficult to test reliably for. So only check the
  # 'ip' command, which is run directly without sudo.
  source "${PATH_DIR_FOR_TEST}/ip.mock_state"
  gn-test-check-mock-partial --oneline address show to 172.16.243.1
}

# Check that hostname checks work properly
TEST_femu_unsecure_internet_fail_hostname() {
  cat >"${PATH_DIR_FOR_TEST}/hostname.mock_side_effects" <<INPUT
echo "test.domain.google.com"
INPUT
  cat >"${PATH_DIR_FOR_TEST}/lsb_release.mock_side_effects" <<INPUT
echo "unknown"
INPUT
  BT_EXPECT_FAIL "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/start-unsecure-internet.sh" "--cleanup" "fakenetwork" >/dev/null 2>/dev/null
}

# Check that rodete checks work properly
TEST_femu_unsecure_internet_fail_hostname() {
  cat >"${PATH_DIR_FOR_TEST}/hostname.mock_side_effects" <<INPUT
echo "test.non-google.com"
INPUT
  cat >"${PATH_DIR_FOR_TEST}/lsb_release.mock_side_effects" <<INPUT
echo "rodete"
INPUT
  BT_EXPECT_FAIL "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/start-unsecure-internet.sh" "--cleanup" "fakenetwork" >/dev/null 2>/dev/null
}

# Verifies that fx emu starts up grpcwebproxy correctly
TEST_femu_grpcwebproxy() {

  # Search and replace the existing kill grpcwebproxy command with a test we control. Normally
  # bash provides a builtin kill but this cannot be intercepted without adding "enable -n kill"
  # somewhere.
  cat >"${PATH_DIR_FOR_TEST}/fakekill.mock_side_effects" <<INPUT
echo "$@"
INPUT
  sed -i'.bak' "s/ kill / fakekill /g" "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/devshell/emu"

  # Create fake ZIP file download so femu.sh doesn't try to download it
  touch "${FUCHSIA_WORK_DIR}/emulator/grpcwebproxy-${PLATFORM}-${GRPCWEBPROXY_LABEL}.zip"

  if is-mac; then
    # grpcwebproxy does not work on OSX, so check there is an error message
    BT_EXPECT_FAIL run_femu_wrapper \
      -x 1234 \
      > femu_error_output.txt 2>&1
  else
    # Run command with the default grpcwebproxy.
    BT_EXPECT run_femu_wrapper \
      -x 1234

    # Verify that the default grpcwebproxy binary is called correctly
    # shellcheck disable=SC1090
    source "${FUCHSIA_WORK_DIR}/emulator/grpcwebproxy-${PLATFORM}-${GRPCWEBPROXY_LABEL}/grpcwebproxy.mock_state"
    gn-test-check-mock-partial --backend_addr localhost:5556
    gn-test-check-mock-partial --server_http_debug_port 1234

    # Run command and test the -X for a manually provided grpcwebproxy.
    BT_EXPECT run_femu_wrapper \
      -x 1234 -X "${BT_TEMP_DIR}/mocked/grpcwebproxy-dir"

    # Verify that the grpcwebproxy binary is called correctly
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/mocked/grpcwebproxy-dir/grpcwebproxy.mock_state"
    gn-test-check-mock-partial --backend_addr localhost:5556
    gn-test-check-mock-partial --server_http_debug_port 1234
  fi
}

# Verifies that the --setup-ufw option runs ufw
TEST_femu_ufw() {

  if is-mac; then
    BT_EXPECT_FAIL run_femu_wrapper \
      --setup-ufw > femu_ufw_error.txt 2>&1
  else
    BT_EXPECT run_femu_wrapper \
      --setup-ufw

    # Check that ufw was called via sudo
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/_isolated_path_for/sudo.mock_state.1"
    gn-test-check-mock-args _ANY_ ufw allow proto _ANY_ from _ANY_ to any port _ANY_ comment _ANY_
  fi
}

TEST_femu_help() {
  BT_EXPECT "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/femu.sh" --help > "${BT_TEMP_DIR}/usage.txt"

readonly EXPECTED_HELP="Usage: femu.sh
  [--work-dir <directory to store image assets>]
    Defaults to ${BT_TEMP_DIR}/test-home/.fuchsia
  [--bucket <fuchsia gsutil bucket>]
    Default is read using \`fconfig get fuchsia-5254-0063-5e7a.bucket\` if set. Otherwise defaults to fuchsia.
  [--image <image name>]
    Default is read using \`fconfig get fuchsia-5254-0063-5e7a.image\` if set. Otherwise defaults to qemu-x64.
  [--authorized-keys <file>]
    The authorized public key file for securing the device.  Defaults to
    ${BT_TEMP_DIR}/test-home/.ssh/fuchsia_authorized_keys, which is generated if needed.
  [--version <version>]
    Specify the CIPD version of AEMU to download.
    Defaults to aemu.version file with bid:unknown
  [--experiment-arm64]
    Override FUCHSIA_ARCH to arm64, instead of the default x64.
    This is required for *-arm64 system images, and is not auto detected.
  [--setup-ufw]
    Set up ufw firewall rules needed for Fuchsia device discovery
    and package serving, then exit. Only works on Linux with ufw
    firewall, and requires sudo.
  [--help] [-h]
    Show command line options for femu.sh and emu subscript

Remaining arguments are passed to emu wrapper and emulator:
  -a <mode> acceleration mode (auto, off, kvm, hvf, hax), default is auto
  -c <text> append item to kernel command line
  -ds <size> extends the fvm image size to <size> bytes. Default is twice the original size
  -N run with emulated nic via tun/tap
  -I <ifname> uses the tun/tap interface named ifname
  -u <path> execute emu if-up script, default: linux: no script, macos: tap ifup script.
  -v <build_variant> uses the build system variant to modify the total memory.
  -e <directory> location of emulator, defaults to looking in prebuilt/third_party/android/aemu/release/PLATFORM
  -g <port> enable gRPC service on port to control the emulator, default is 5556 when WebRTC service is enabled
  -r <fps> webrtc frame rate when using gRPC service, default is 30
  -t <cmd> execute the given command to obtain turn configuration
  -x <port> enable WebRTC HTTP service on port
  -X <directory> location of grpcwebproxy, defaults to looking in prebuilt/third_party/grpcwebproxy
  -w <size> window size, default is 1280x800
  -s <cpus> number of cpus, 1 for uniprocessor
  -m <mb> total memory, in MB
  -k <authorized_keys_file> SSH authorized keys file, otherwise defaults to ~/.ssh/fuchsia_authorized_keys
  -K <kernel_image> path to image to use as kernel, defaults to kernel generated by the current build.
  -z <zbi_image> path to image to use as ZBI, defaults to zircon-a generated by the current build.
  -Z <zbi tool> path to a version of the \`zbi\` tool to use; defaults to the one in the current build directory.
  -f <fvm_image> path to full FVM image, defaults to image generated by the current build.
  -F <fvm tool> path to a version of the \`fvm\` tool to use; defaults to the one in the current build directory.
  -A <arch> architecture of images (x64, arm64), default is the current build.
  -p mouse|touch set pointing device used on emulator: mouse or multitouch screen; defaults to mouse.
  -H|--headless run in headless mode
  --audio run with audio hardware added to the virtual machine
  --hidpi-scaling enable pixel scaling on HiDPI devices
  --host-gpu run with host GPU acceleration
  --software-gpu run without host GPU acceleration
  --debugger pause on launch and wait for a debugger process to attach before resuming
  --no-build do not attempt to build the fvm and zbi tools and the zbi image

Invalid argument names are not flagged as errors, and are passed on to emulator"

  BT_EXPECT_FILE_CONTAINS "${BT_TEMP_DIR}/usage.txt" "${EXPECTED_HELP}"
}

TEST_femu_with_props() {
    cat >"${MOCKED_FCONFIG}.mock_side_effects" <<"EOF"

  if [[ "$1" == "get" ]]; then
    if [[ "${2}" == "fuchsia-5254-0063-5e7a.bucket" ]]; then
      echo "test-bucket"
      return 0
    elif [[ "${2}" == "fuchsia-5254-0063-5e7a.image" ]]; then
      echo "test-image"
      return 0
    fi
    echo ""
  fi
EOF

  # Run command.
  BT_EXPECT run_femu_wrapper

  # Check that fpave.sh was called to download the needed system images
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/fpave.sh.mock_state"
  gn-test-check-mock-args _ANY_ --prepare --image test-image --bucket test-bucket --work-dir "${FUCHSIA_WORK_DIR}"

}


# Test initialization. Note that we copy various tools/devshell files and need to replicate the
# behavior of generate.py by copying these files into scripts/sdk/gn/base/bin/devshell
# shellcheck disable=SC2034
BT_FILE_DEPS=(
  scripts/sdk/gn/base/bin/femu.sh
  scripts/sdk/gn/base/bin/devshell/lib/image_build_vars.sh
  scripts/sdk/gn/base/bin/fuchsia-common.sh
  scripts/sdk/gn/bash_tests/gn-bash-test-lib.sh
  tools/devshell/emu
  tools/devshell/lib/fvm.sh
  tools/devshell/lib/emu-ifup-macos.sh
  scripts/start-unsecure-internet.sh
)
# shellcheck disable=SC2034
BT_MOCKED_TOOLS=(
  # mock both mac and linux so the test runs successfully on both.
  test-home/.fuchsia/emulator/aemu-linux-amd64-"${AEMU_LABEL}"/emulator
  test-home/.fuchsia/emulator/aemu-mac-amd64-"${AEMU_LABEL}"/emulator
  test-home/.fuchsia/emulator/grpcwebproxy-mac-amd64-"${GRPCWEBPROXY_LABEL}"/grpcwebproxy
  test-home/.fuchsia/emulator/grpcwebproxy-linux-amd64-"${GRPCWEBPROXY_LABEL}"/grpcwebproxy
  scripts/sdk/gn/base/bin/fpave.sh
  scripts/sdk/gn/base/tools/x64/fconfig
  scripts/sdk/gn/base/tools/arm64/fconfig
  scripts/sdk/gn/base/tools/x64/zbi
  scripts/sdk/gn/base/tools/arm64/zbi
  scripts/sdk/gn/base/tools/x64/fvm
  scripts/sdk/gn/base/tools/arm64/fvm
  mocked/grpcwebproxy-dir/grpcwebproxy
  _isolated_path_for/ip
  _isolated_path_for/fakekill
  _isolated_path_for/sudo
  _isolated_path_for/hostname
  _isolated_path_for/lsb_release
  # Create fake "stty sane" command so that fx emu test succeeds when < /dev/null is being used
  _isolated_path_for/stty
)

BT_SET_UP() {
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/bash_tests/gn-bash-test-lib.sh"

  # Make "home" directory in the test dir so the paths are stable."
  mkdir -p "${BT_TEMP_DIR}/test-home"
  export HOME="${BT_TEMP_DIR}/test-home"
  FUCHSIA_WORK_DIR="${HOME}/.fuchsia"

  if is-mac; then
    PLATFORM="mac-amd64"
  else
    PLATFORM="linux-amd64"
  fi

  MOCKED_FCONFIG="${BT_TEMP_DIR}/scripts/sdk/gn/base/$(gn-test-tools-subdir)/fconfig"
  MOCKED_FVM="${BT_TEMP_DIR}/scripts/sdk/gn/base/$(gn-test-tools-subdir)/fvm"
  MOCKED_ZBI="${BT_TEMP_DIR}/scripts/sdk/gn/base/$(gn-test-tools-subdir)/zbi"

  # Specify the name of a fake /dev/kvm device that we control, modify fx emu to use this
  DEV_KVM_FAKE="${BT_TEMP_DIR}/dev-kvm-fake"
  touch "${DEV_KVM_FAKE}"
  # Need to create .bak file for this to work on OSX and Linux, and use # to avoid conflicts with test filenames containing /
  sed -i'.bak' "s#/dev/kvm#${DEV_KVM_FAKE}#g" "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/devshell/emu"

  PATH_DIR_FOR_TEST="${BT_TEMP_DIR}/_isolated_path_for"
  export PATH="${PATH_DIR_FOR_TEST}:${PATH}"

  # Create a small disk image to avoid downloading, and test if it is doubled in size as expected
  mkdir -p "${FUCHSIA_WORK_DIR}/image"
  dd if=/dev/zero of="${FUCHSIA_WORK_DIR}/image/storage-full.blk" bs=1024 count=1  > /dev/null 2>/dev/null

  # Create fake ZIP file download so femu.sh doesn't try to download it, and
  # later on provide a mocked emulator script so it doesn't try to unzip it.
  touch "${FUCHSIA_WORK_DIR}/emulator/aemu-${PLATFORM}-${AEMU_LABEL}.zip"

  # Need to configure a DISPLAY so that we can get past the graphics error checks
  export DISPLAY="fakedisplay"
}

BT_INIT_TEMP_DIR() {

  # Generate the prebuilt version file based on the simulated version string
  echo "${AEMU_VERSION}" > "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/aemu.version"
  echo "${GRPCWEBPROXY_VERSION}" > "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/grpcwebproxy.version"

  # Create empty authorized_keys file to add to the system image, but the contents are not used.
  mkdir -p "${BT_TEMP_DIR}/scripts/sdk/gn/base/testdata"
  echo ssh-ed25519 00000000000000000000000000000000000000000000000000000000000000000000 \
    >"${BT_TEMP_DIR}/scripts/sdk/gn/base/testdata/authorized_keys"

  # Stage the files we copy from the fx emu implementation, replicating behavior of generate.py
  cp -r "${BT_TEMP_DIR}/tools/devshell" "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/"
  cp -r "${BT_TEMP_DIR}/scripts/start-unsecure-internet.sh" "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/"
}

BT_RUN_TESTS "$@"
