// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/tests/guest_console.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <zircon/status.h>

#include <iostream>

#include "logger.h"
#include "src/lib/fxl/strings/string_printf.h"

static constexpr zx::duration kSerialStableDelay = zx::msec(800);

// This is the maximum line length of dash in both zircon_guest and
// debian_guest.
static constexpr size_t kMaximumLineLength = 4096;

static std::string command_hash(const std::string& command) {
  std::hash<std::string> hash;
  return fxl::StringPrintf("%zu", hash(command));
}

static std::string normalize_new_lines(const std::string& s) {
  std::string result;
  // Strip carriage returns to normalise both guests on newlines only.
  for (const char c : s) {
    if (c != '\r') {
      result.push_back(c);
    }
  }
  return result;
}

GuestConsole::GuestConsole(std::unique_ptr<SocketInterface> socket) : socket_(std::move(socket)) {}

zx_status_t GuestConsole::Start(zx::time deadline) {
  zx_status_t status;

  // Wait for something to be sent over serial. Both Zircon and Debian will send
  // at least a command prompt. For Debian, this is necessary since any commands
  // we send will be ignored until the guest is ready.
  status = WaitForAny(deadline);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed waiting for any output on the serial console: "
                   << zx_status_get_string(status);
    return status;
  }

  // Wait for output to stabilize.
  //
  // In particular, we wait for a duration of kSerialStableDelay to pass
  // without any output on the line before we consider the output stable.
  do {
    status = WaitForAny(zx::deadline_after(kSerialStableDelay));
    if (status != ZX_OK && status != ZX_ERR_TIMED_OUT) {
      FX_LOGS(ERROR) << "Failed waiting for serial console to stabilize: "
                     << zx_status_get_string(status);
      return status;
    }

    // If we've exceeded our deadline, abort.
    if (zx::clock::get_monotonic() >= deadline) {
      return ZX_ERR_TIMED_OUT;
    }
  } while (status == ZX_OK);

  return ZX_OK;
}

// Sends a command and waits for the response. We capture output by echoing a
// header and footer before and after the command. Then we wait for the command
// to be written back to the serial, then the header, then finally we capture
// everything until the footer.
zx_status_t GuestConsole::ExecuteBlocking(const std::string& command, const std::string& prompt,
                                          zx::time deadline, std::string* result) {
  std::string header = command_hash(command);
  std::string footer = header;
  std::reverse(footer.begin(), footer.end());

  std::string full_command = "echo " + header + "; " + command + "; echo " + footer;
  if (full_command.size() > kMaximumLineLength) {
    FX_LOGS(ERROR) << "Command is too long";
    return ZX_ERR_OUT_OF_RANGE;
  }

  zx_status_t status = SendBlocking(full_command + "\n", deadline);
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "Failed to send command: " << zx_status_get_string(status);
    return status;
  }

  std::string intermediate_result;
  status = WaitForMarker(full_command, deadline, &intermediate_result);
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "Failed to wait for command echo: " << zx_status_get_string(status);
    FX_LOGS(INFO) << "Received: \"" << intermediate_result << "\"";
    return status;
  }

  status = WaitForMarker(header + "\n", deadline, &intermediate_result);
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "Failed to wait for command header: " << zx_status_get_string(status);
    FX_LOGS(INFO) << "Received: \"" << intermediate_result << "\"";
    return status;
  }

  status = WaitForMarker(footer + "\n", deadline, result);
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "Failed to wait for command footer: " << zx_status_get_string(status);
    if (result != nullptr && !result->empty()) {
      FX_LOGS(INFO) << "Received: \"" << *result << "\"";
    }
    return status;
  }

  status = WaitForMarker(prompt, deadline, &intermediate_result);
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "Failed to wait for command prompt: " << zx_status_get_string(status);
    FX_LOGS(INFO) << "Received: \"" << intermediate_result << "\"";
    return status;
  }

  return ZX_OK;
}

zx_status_t GuestConsole::RepeatCommandTillSuccess(const std::string& command,
                                                   const std::string& prompt,
                                                   const std::string& success, zx::time deadline,
                                                   zx::duration repeat_rate) {
  do {
    std::string response;
    zx::time command_timeout = std::min(zx::deadline_after(repeat_rate), deadline);
    zx_status_t status = ExecuteBlocking(command, prompt, command_timeout, &response);
    if (status == ZX_OK && response.find(success) != std::string::npos) {
      return ZX_OK;
    }

    // In case the command failed early, wait till at least the repeat_rate deadline has passed
    // before trying agian.
    zx::nanosleep(std::min(command_timeout, deadline));
  } while (zx::clock::get_monotonic() < deadline);

  return ZX_ERR_TIMED_OUT;
}

zx_status_t GuestConsole::SendBlocking(const std::string& message, zx::time deadline) {
  return socket_->Send(deadline, message);
}

zx_status_t GuestConsole::WaitForMarker(const std::string& marker, zx::time deadline,
                                        std::string* result) {
  std::string output = buffer_;
  buffer_.erase();
  while (true) {
    // Check if the marker is already in our buffer.
    auto marker_loc = output.rfind(marker);
    if (marker_loc != std::string::npos) {
      // If we have read the socket past the end of the marker, make sure
      // what's left is kept in the buffer for the next read.
      if (marker_loc + marker.size() < output.size()) {
        buffer_ = output.substr(marker_loc + marker.size());
      }
      if (result == nullptr) {
        return ZX_OK;
      }
      output.erase(marker_loc);
      *result = output;
      return ZX_OK;
    }

    // Marker is not present: read some more data into the buffer.
    std::string buff;
    zx_status_t status = socket_->Receive(deadline, &buff);
    if (status != ZX_OK) {
      if (result != nullptr) {
        *result = output;
      }
      return status;
    }
    Logger::Get().Write(buff);
    output.append(normalize_new_lines(buff));
  }
}

zx_status_t GuestConsole::WaitForSocketClosed(zx::time deadline) {
  return socket_->WaitForClosed(deadline);
}

zx_status_t GuestConsole::Drain() {
  std::string result;
  zx_status_t status = DrainSocket(socket_.get(), &result);
  Logger::Get().Write(result);
  return status;
}

zx_status_t GuestConsole::WaitForAny(zx::time deadline) {
  std::string result;
  zx_status_t status = socket_->Receive(deadline, &result);
  if (status != ZX_OK) {
    return status;
  }
  Logger::Get().Write(result);

  Drain();
  return ZX_OK;
}
