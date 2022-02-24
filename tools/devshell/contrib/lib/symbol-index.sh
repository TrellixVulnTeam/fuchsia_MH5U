# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This library is used by:
# * symbol-index
# * debug
# * fidlcat
# * symbolize
#
# This file is not self-contained! ../../lib/vars.sh must be sourced before this file.

function symbol-index {
  local symbol_index="${HOST_OUT_DIR}/symbol-index"

  if [[ ! -e ${symbol_index} ]]; then
    fx-error "${symbol_index} is not found, make sure you have //bundles:tools (or //tools) in " \
             "your 'fx set' command and 'fx build' was executed successfully."
    exit 1
  fi

  "${symbol_index}" "$@"
  return $?
}

function ensure-symbol-index-registered {
  symbol-index add-all <<EOF || return $?
${FUCHSIA_BUILD_DIR}/.build-id     ${FUCHSIA_BUILD_DIR}
${FUCHSIA_DIR}/prebuilt/.build-id
${PREBUILT_CLANG_DIR}/lib/debug/.build-id
${PREBUILT_RUST_DIR}/lib/debug/.build-id
EOF
  fx-command-run ffx debug symbol-index add "${FUCHSIA_BUILD_DIR}/.symbol-index.json" || return $?
}
