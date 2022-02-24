# Debian Guest

The `debian_guest` package provides a basic Linux environment based on the
Debian Linux distribution.

## Building

These steps will walk through building a package with the root filesystem
bundled as a package resource. The root filesystem will appear writable but
all writes are volatile and will disappear when the guest shuts down.

```
$ cd $FUCHSIA_DIR
$ ./src/virtualization/packages/debian_guest/build-image.sh prebuilt/virtualization/packages/debian_guest x64
$ fx set core.x64 --with-base "//src/virtualization,//src/virtualization/packages/debian_guest"
$ fx build
$ fx pave
```

To boot on an ARM64 device, replace `x64` with `arm64`.

## Running `debian_guest`

Once booted:

```
guest launch debian_guest
```

## Telnet shell

The Debian system exposes a simple telnet interface over vsock port 23. You can
use the `guest` CLI to connect to this socket to open a shell. First we need to
identify the environment ID and the guest context ID (CID) to use:

```
$ guest list
env:0             debian_guest
 guest:3          debian_guest
```

The above indicates the debian guest is CID 3 in environment 0. Open a shell
with:

```
$ guest socat 0 3 23
```

## CIPD (Googlers only)

All of the images constructed by `build-image.sh` (see above) are available on
CIPD. To update and upload those images run the following. The scripts will
prompt for a CIPD auth token and for sudo access.

```
$ cd $FUCHSIA_DIR
$ ./src/virtualization/packages/debian_guest/mkcipd.sh x64
$ ./src/virtualization/packages/debian_guest/mkcipd.sh arm64
```
