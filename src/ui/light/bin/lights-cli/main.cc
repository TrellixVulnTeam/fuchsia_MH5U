// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.hardware.light/cpp/wire.h>
#include <lib/fdio/fdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fbl/unique_fd.h>

#include "lights-cli.h"

constexpr char kLightsDevicePath[] = "/dev/class/light/000";
constexpr char kUsageMessage[] = R"""(Usage: lights-cli <command> <index> <value>
    Example:
    lights-cli print 0
    lights-cli set 0 <val>
    lights-cli summary
)""";

zx_status_t GetDeviceHandle(const char* path, zx::channel* handle) {
  fbl::unique_fd fd(open(path, O_RDWR));
  if (fd.get() < -1) {
    fprintf(stderr, "Failed to open lights device: %d\n", fd.get());
    return ZX_ERR_IO;
  }

  zx_status_t status = fdio_get_service_handle(fd.release(), handle->reset_and_get_address());
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to get FDIO handle for lights device: %d\n", status);
  }
  return status;
}

int main(int argc, char** argv) {
  zx::channel channel;
  zx_status_t status = GetDeviceHandle(kLightsDevicePath, &channel);
  if (status != ZX_OK) {
    printf("Failed to open lights device at '%s'\n", kLightsDevicePath);
    return 1;
  }

  if (argc <= 1) {
    printf("%s expects at least 1 argument\n", argv[0]);
    printf(kUsageMessage);
    return 1;
  }

  LightsCli lights_cli(std::move(channel));

  if (strcmp(argv[1], "print") == 0 && argc == 3) {
    status = lights_cli.PrintValue(atoi(argv[2]));
  } else if (strcmp(argv[1], "set") == 0 && argc == 4) {
    status = lights_cli.SetValue(atoi(argv[2]), atof(argv[3]));
  } else if (strcmp(argv[1], "summary") == 0 && argc == 2) {
    status = lights_cli.Summary();
  } else {
    printf("%s", kUsageMessage);
    return 1;
  }

  if (status != ZX_OK) {
    return 1;
  }
  return 0;
}
