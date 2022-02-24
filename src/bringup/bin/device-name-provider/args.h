// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_DEVICE_NAME_PROVIDER_ARGS_H_
#define SRC_BRINGUP_BIN_DEVICE_NAME_PROVIDER_ARGS_H_

#include <fidl/fuchsia.io/cpp/wire.h>

#include <string>

constexpr char kDefaultDevdir[] = "/dev";

struct DeviceNameProviderArgs {
  // This is the string value of `netsvc.interface`.
  // It is overridden by the string value of `--interface` on the binary commandline.
  std::string interface;
  // This is the string value of `zircon.nodename`.
  // It is overridden by the string value of `--nodename` on the binary commandline.
  std::string nodename;
  // This defaults to "/dev"
  // BUT it overridden by `--devdir` on the binary commandline.
  std::string devdir;
  // This is the integer value of `zircon.namegen`
  // It is overridden by the value of `--namegen` on the commandline.
  // `--namegen 0` enables wordnames, any other value is treated as 1.
  // It has no effect if `nodename` is non-empty.
  uint32_t namegen = 1;
};

// Parses DeviceNameProviderArgs via the kernel commandline and the binary commandline (argv).
// If ParseArgs returns < 0, an error string will be returned in |error|.
int ParseArgs(int argc, char** argv, fidl::UnownedClientEnd<fuchsia_io::Directory> svc_root,
              const char** error, DeviceNameProviderArgs* out);

#endif  // SRC_BRINGUP_BIN_DEVICE_NAME_PROVIDER_ARGS_H_
