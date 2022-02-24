// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/console_impl.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef __Fuchsia__
#include <signal.h>
#include <termios.h>
#endif

#include <lib/syslog/cpp/macros.h>

#include <filesystem>

#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/system.h"
#include "src/developer/debug/zxdb/client/target.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_parser.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/split_string.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/fxl/strings/trim.h"

namespace zxdb {

namespace {

const char* kHistoryFilename = ".zxdb_history";

#ifndef __Fuchsia__

termios stdout_saved_termios;
struct sigaction saved_abort;
struct sigaction saved_segv;

void TerminalRestoreSignalHandler(int sig, siginfo_t* info, void* ucontext) {
  struct sigaction _ignore;

  if (sig == SIGABRT) {
    sigaction(SIGABRT, &saved_abort, &_ignore);
  } else if (sig == SIGSEGV) {
    sigaction(SIGSEGV, &saved_segv, &_ignore);
  } else {
    // Weird, but I'm not about to assert inside a signal handler.
    return;
  }

  tcsetattr(STDOUT_FILENO, TCSAFLUSH, &stdout_saved_termios);
  raise(sig);
}

void PreserveStdoutTermios() {
  if (!isatty(STDOUT_FILENO))
    return;

  if (tcgetattr(STDOUT_FILENO, &stdout_saved_termios) < 0)
    return;

  struct sigaction restore_handler;

  restore_handler.sa_sigaction = TerminalRestoreSignalHandler;
  restore_handler.sa_flags = SA_SIGINFO;

  sigaction(SIGABRT, &restore_handler, &saved_abort);
  sigaction(SIGSEGV, &restore_handler, &saved_segv);
}

#else

void PreserveStdoutTermios() {}

#endif  // !__Fuchsia__

}  // namespace

ConsoleImpl::ConsoleImpl(Session* session, line_input::ModalLineInput::Factory line_input_factory)
    : Console(session), line_input_(std::move(line_input_factory)), impl_weak_factory_(this) {
  line_input_.Init([this](std::string s) { ProcessInputLine(s); }, "[zxdb] ");

  // Set the line input completion callback that can know about our context. OK to bind |this| since
  // we own the line_input object.
  FillCommandContextCallback fill_command_context([this](Command* cmd) {
    context_.FillOutCommand(cmd);  // Ignore errors, this is for autocomplete.
  });
  line_input_.SetAutocompleteCallback([fill_command_context = std::move(fill_command_context)](
                                          std::string prefix) -> std::vector<std::string> {
    return GetCommandCompletions(prefix, fill_command_context);
  });

  // Cancel (ctrl-c) handling.
  line_input_.SetCancelCallback([this]() {
    if (line_input_.GetLine().empty()) {
      // Stop program execution. Do this by visibly typing the stop command so the user knows
      // what is happening.
      line_input_.SetCurrentInput("pause --clear-state");
      line_input_.OnInput(line_input::SpecialCharacters::kKeyEnter);
    } else {
      // Control-C with typing on the line just clears the current state.
      line_input_.SetCurrentInput(std::string());
    }
  });

  // EOF (ctrl-d) should exit gracefully.
  line_input_.SetEofCallback([this]() { Quit(); });

  // Set stdin to async mode or OnStdinReadable will block.
  fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
}

ConsoleImpl::~ConsoleImpl() {
  if (!SaveHistoryFile())
    Console::Output(Err("Could not save history file to $HOME/%s.\n", kHistoryFilename));
}

fxl::WeakPtr<ConsoleImpl> ConsoleImpl::GetImplWeakPtr() { return impl_weak_factory_.GetWeakPtr(); }

void ConsoleImpl::Init() {
  PreserveStdoutTermios();

  stdio_watch_ =
      debug::MessageLoop::Current()->WatchFD(debug::MessageLoop::WatchMode::kRead, STDIN_FILENO,
                                             [this](int fd, bool readable, bool, bool error) {
                                               if (error)  // EOF
                                                 Quit();

                                               if (!readable)
                                                 return;

                                               char ch;
                                               while (read(STDIN_FILENO, &ch, 1) > 0)
                                                 line_input_.OnInput(ch);
                                             });

  LoadHistoryFile();
  line_input_.Show();
}

void ConsoleImpl::LoadHistoryFile() {
  std::filesystem::path path(getenv("HOME"));
  if (path.empty())
    return;
  path /= kHistoryFilename;

  std::string data;
  if (!files::ReadFileToString(path, &data))
    return;

  auto history = fxl::SplitStringCopy(data, "\n", fxl::kTrimWhitespace, fxl::kSplitWantNonEmpty);

  for (const std::string& cmd : history)
    line_input_.AddToHistory(cmd);
}

bool ConsoleImpl::SaveHistoryFile() {
  char* home = getenv("HOME");
  if (!home)
    return false;

  // We need to invert the order the deque has the entries.
  std::string history_data;
  const auto& history = line_input_.GetHistory();
  for (auto it = history.rbegin(); it != history.rend(); it++) {
    auto trimmed = fxl::TrimString(*it, " ");
    // We ignore empty entries or quit commands.
    if (trimmed.empty() || trimmed == "quit" || trimmed == "q" || trimmed == "exit") {
      continue;
    }

    history_data.append(trimmed).append("\n");
  }

  auto filepath = std::filesystem::path(home) / kHistoryFilename;
  return files::WriteFile(filepath, history_data.data(), history_data.size());
}

void ConsoleImpl::Output(const OutputBuffer& output) {
  // Since most operations are asynchronous, we have to hide the input line before printing anything
  // or it will get appended to whatever the user is typing on the screen.
  //
  // TODO(brettw) This can cause flickering. A more advanced system would do more fancy console
  // stuff to output above the input line so we'd never have to hide it.

  // Make sure stdout is in blocking mode since normal output won't expect non-blocking mode. We can
  // get in this state if stdin and stdout are the same underlying handle because the constructor
  // sets stdin to O_NONBLOCK so we can asynchronously wait for input.
  int old_bits = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (old_bits & O_NONBLOCK)
    fcntl(STDOUT_FILENO, F_SETFL, old_bits & ~O_NONBLOCK);

  line_input_.Hide();
  output.WriteToStdout();
  line_input_.Show();

  if (old_bits & O_NONBLOCK)
    fcntl(STDOUT_FILENO, F_SETFL, old_bits);
}

void ConsoleImpl::ModalGetOption(const line_input::ModalPromptOptions& options,
                                 OutputBuffer message, const std::string& prompt,
                                 line_input::ModalLineInput::ModalCompletionCallback cb) {
  // Print the message from within the "will show" callback to ensure proper serialization if there
  // are multiple prompts pending.
  //
  // Okay to capture |this| because we own the line_input_.
  line_input_.ModalGetOption(options, prompt, std::move(cb),
                             [this, message = std::move(message)]() { Output(message); });
}

void ConsoleImpl::Quit() {
  line_input_.Hide();
  debug::MessageLoop::Current()->QuitNow();
}

void ConsoleImpl::Clear() {
  // We write directly instead of using Output because WriteToStdout expects to append '\n' to
  // outputs and won't flush it explicitly otherwise.
  line_input_.Hide();
  const char ff[] = "\033c";  // Form feed.
  write(STDOUT_FILENO, ff, sizeof(ff));
  line_input_.Show();
}

void ConsoleImpl::ProcessInputLine(const std::string& line, CommandCallback callback,
                                   bool add_to_history) {
  Command cmd;
  Err err;
  if (line.empty()) {
    // Repeat the previous command, don't add to history.
    err = ParseCommand(previous_line_, &cmd);
  } else {
    err = ParseCommand(line, &cmd);
    if (add_to_history) {
      line_input_.AddToHistory(line);
      previous_line_ = line;
    }
  }

  if (err.ok()) {
    err = context_.FillOutCommand(&cmd);
    if (!err.has_error()) {
      err = DispatchCommand(&context_, cmd, std::move(callback));

      if (cmd.thread() && cmd.verb() != Verb::kNone) {
        // Show the right source/disassembly for the next listing.
        context_.SetSourceAffinityForThread(cmd.thread(),
                                            GetVerbRecord(cmd.verb())->source_affinity);
      }
    }
  }

  if (err.has_error()) {
    OutputBuffer out;
    out.Append(err);
    Output(out);
  }
}

}  // namespace zxdb
