// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/cmdline/args_parser.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/zx/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <memory>
#include <thread>

#include "src/developer/debug/debug_agent/debug_agent.h"
#include "src/developer/debug/debug_agent/fidl_server.h"
#include "src/developer/debug/debug_agent/socket_connection.h"
#include "src/developer/debug/debug_agent/unwind.h"
#include "src/developer/debug/debug_agent/zircon_system_interface.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/platform_message_loop.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {
namespace {

// Valid options for the --unwind flag.
const char kAospUnwinder[] = "aosp";
const char kNgUnwinder[] = "ng";
const char kFuchsiaUnwinder[] = "fuchsia";

struct CommandLineOptions {
  int port = 0;
  bool debug_mode = false;
  bool channel_mode = false;
  std::string unwind = kAospUnwinder;
};

const char kHelpIntro[] = R"(debug_agent --port=<port> [ <options> ]

  The debug_agent provides the on-device stub for the ZXDB frontend to talk
  to. Once you launch the debug_agent, connect zxdb to the same port you
  provide on the command-line.

Options

)";

const char kHelpHelp[] = R"(  --help
  -h
      Prints all command-line switches.)";

const char kPortHelp[] = R"(  --port=<port>
    [Required] TCP port number to listen to incoming connections on.)";

const char kDebugModeHelp[] = R"(  --debug-mode
  -d
      Run the agent on debug mode. This will enable conditional logging
      messages and timing profiling. Mainly useful for people developing zxdb.)";

const char kChannelModeHelp[] = R"(  --channel-mode
      Run the agent on in channel mode. The agent will listen for channels through the
      fuchsia.debugger.DebugAgent API. This is necessary for overnet.)";

const char kUnwindHelp[] = R"(  --unwind=[aosp|ng|fuchsia]
      Force using a specific unwinder for generating stack traces.)";

cmdline::Status ParseCommandLine(int argc, const char* argv[], CommandLineOptions* options) {
  cmdline::ArgsParser<CommandLineOptions> parser;

  parser.AddSwitch("port", 0, kPortHelp, &CommandLineOptions::port);
  parser.AddSwitch("debug-mode", 'd', kDebugModeHelp, &CommandLineOptions::debug_mode);
  parser.AddSwitch("channel-mode", 0, kChannelModeHelp, &CommandLineOptions::channel_mode);
  parser.AddSwitch("unwind", 0, kUnwindHelp, &CommandLineOptions::unwind);

  // Special --help switch which doesn't exist in the options structure.
  bool requested_help = false;
  parser.AddGeneralSwitch("help", 'h', kHelpHelp, [&requested_help]() { requested_help = true; });

  std::vector<std::string> params;
  cmdline::Status status = parser.Parse(argc, argv, options, &params);
  if (status.has_error())
    return status;

  // Handle --help switch since we're the one that knows about the switches.
  if (requested_help)
    return cmdline::Status::Error(kHelpIntro + parser.GetHelp());

  return cmdline::Status::Ok();
}

}  // namespace
}  // namespace debug_agent

// main --------------------------------------------------------------------------------------------

int main(int argc, const char* argv[]) {
  debug_agent::CommandLineOptions options;
  cmdline::Status status = ParseCommandLine(argc, argv, &options);
  if (status.has_error()) {
    FX_LOGS(ERROR) << status.error_message();
    return 1;
  }

  // Decode the unwinder type.
  if (options.unwind == debug_agent::kAospUnwinder) {
    debug_agent::SetUnwinderType(debug_agent::UnwinderType::kAndroid);
  } else if (options.unwind == debug_agent::kNgUnwinder) {
    debug_agent::SetUnwinderType(debug_agent::UnwinderType::kNgUnwind);
  } else if (options.unwind == debug_agent::kFuchsiaUnwinder) {
    debug_agent::SetUnwinderType(debug_agent::UnwinderType::kFuchsia);
  } else {
    FX_LOGS(ERROR) << "Invalid option for --unwind. See debug_agent --help.";
    return 1;
  }

  debug::SetLogCategories({debug::LogCategory::kAll});
  if (options.debug_mode) {
    syslog::LogSettings settings = {.min_log_level = syslog::LOG_TRACE};
    syslog::SetLogSettings(settings);
    debug::SetDebugMode(true);
    FX_LOGS(DEBUG) << "Running the debug agent in debug mode.";
  }

  if (options.channel_mode || options.port) {
    auto services = sys::ServiceDirectory::CreateFromNamespace();

    zx::channel exception_channel;
    auto message_loop = std::make_unique<debug::PlatformMessageLoop>();
    std::string init_error_message;
    if (!message_loop->Init(&init_error_message)) {
      FX_LOGS(ERROR) << init_error_message;
      return 1;
    }

    // The scope ensures the objects are destroyed before calling Cleanup on the MessageLoop.
    {
      // The debug agent is independent of whether it's connected or not.
      // DebugAgent::Disconnect is called by ~SocketConnection is called by ~SocketServer, so the
      // debug agent must be destructed after the SocketServer.
      debug_agent::DebugAgent debug_agent(std::make_unique<debug_agent::ZirconSystemInterface>());

      debug_agent::SocketServer server;
      if (!server.Init(options.port)) {
        message_loop->Cleanup();
        return 1;
      }

      // The following loop will attempt to patch a stream to the debug agent in order to enable
      // communication.
      while (true) {
        if (options.channel_mode) {
          // Connect to FIDL service which will wait for an incoming connection from a client.
          // This happens within the message_loop so it does not need a new thread.
          debug_agent::DebugAgentImpl fidl_agent(&debug_agent);
          fidl::BindingSet<fuchsia::debugger::DebugAgent> bindings;
          auto context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
          context->outgoing()->AddPublicService(bindings.GetHandler(&fidl_agent));

          FX_LOGS(INFO) << "Start listening on FIDL fuchsia::debugger::DebugAgent.";
          message_loop->Run();
        } else {
          // Start a new thread that will listen on a socket from an incoming connection from a
          // client. In the meantime, the main thread will block waiting for something to be posted
          // to the main thread.
          //
          // When the connection thread effectively receives a connection, it will post a task to
          // the loop to create the agent and begin normal debugger operation. Once the application
          // quits the loop, the code will clean the connection thread and create another or exit
          // the loop, according to the current agent's configuration.
          debug_agent::SocketServer::ConnectionConfig conn_config;
          conn_config.message_loop = message_loop.get();
          conn_config.debug_agent = &debug_agent;
          conn_config.port = options.port;
          std::thread conn_thread(&debug_agent::SocketServer::Run, &server, std::move(conn_config));

          FX_LOGS(INFO) << "Start listening on port " << options.port;
          message_loop->Run();

          DEBUG_LOG(Agent) << "Joining connection thread.";
          conn_thread.join();
        }

        // See if the debug agent was told to exit.
        if (debug_agent.should_quit())
          break;

        // Prepare for another connection.
        // The resources need to be freed on the message loop's thread.
        server.Reset();
      }
    }
    message_loop->Cleanup();
  } else {
    FX_LOGS(ERROR) << "--port=<port-number> required. See debug_agent --help.";
    return 1;
  }

  // It's very useful to have a simple message that informs the debug agent
  // exited successfully.
  FX_LOGS(INFO) << "See you, Space Cowboy...";
  return 0;
}
