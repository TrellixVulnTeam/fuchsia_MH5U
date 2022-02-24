#!/bin/bash
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

devshell_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
FUCHSIA_DIR="$(dirname $(dirname $(dirname "${devshell_lib_dir}")))"

if [[ -d "${FUCHSIA_DIR}/.jiri_root/bin" ]]; then
  rm -f "${FUCHSIA_DIR}/.jiri_root/bin/fx"
  ln -s "../../scripts/fx" "${FUCHSIA_DIR}/.jiri_root/bin/fx"

  rm -f "${FUCHSIA_DIR}/.jiri_root/bin/ffx"
  ln -s "../../src/developer/ffx/scripts/ffx" "${FUCHSIA_DIR}/.jiri_root/bin/ffx"

  rm -f "${FUCHSIA_DIR}/.jiri_root/bin/hermetic-env"
  ln -s "../../scripts/hermetic-env" "${FUCHSIA_DIR}/.jiri_root/bin/hermetic-env"

  rm -f "${FUCHSIA_DIR}/.jiri_root/bin/fuchsia-vendored-python"
  ln -s "../../scripts/fuchsia-vendored-python" "${FUCHSIA_DIR}/.jiri_root/bin/fuchsia-vendored-python"
fi
