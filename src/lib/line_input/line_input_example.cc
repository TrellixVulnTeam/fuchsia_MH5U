// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include "src/lib/line_input/modal_line_input.h"

line_input::ModalLineInput* g_line_input = nullptr;
bool g_should_quit = false;

// Callback for quit prompt.
void OnQuitAccept(const std::string& line) {
  g_line_input->Hide();  // Always hide before quitting to put the terminal back.
  if (line == "y" || line == "Y") {
    g_should_quit = true;
    return;
  }

  printf("Not exiting.\n");
  g_line_input->Show();
}

void OnAccept(const std::string& line) {
  g_line_input->AddToHistory(line);
  if (line == "quit" || line == "q") {
    line_input::ModalPromptOptions opts;
    opts.require_enter = false;
    opts.options.push_back("y");
    opts.options.push_back("n");

    g_line_input->ModalGetOption(opts, "(y/n) ", &OnQuitAccept,
                                 []() { printf("Are you sure you want to exit?\n"); });
  } else if (line == "prompt" || line == "p") {
    // This creates two prompts at the same time to test that we can handle sequential asynchonous
    // prompts.
    g_line_input->BeginModal(
        "[1] ",
        [](const std::string& line) {
          if (line == "n")
            g_line_input->EndModal();
        },
        []() { printf("Type a \"n\" to advance to the next prompt.\n"); });
    g_line_input->BeginModal(
        "[2] ",
        [](const std::string& line) {
          if (line == "n")
            g_line_input->EndModal();
        },
        []() { printf("Type a \"n\" to finish.\n"); });
  } else {
    printf("Got the input:\n  %s\n", line.c_str());
  }
}

void OnEof() {
  g_should_quit = true;
  g_line_input->Hide();  // Always hide before quitting to put the terminal back.
}

int main(int argc, char** argv) {
  line_input::ModalLineInput input;
  input.Init(&OnAccept, "C:\\> ");
  input.SetEofCallback(&OnEof);
  g_line_input = &input;

  printf(
      "Type some lines, nonempty lines will be added to history.\n"
      "\"quit\"/\"q\" will exit with prompt, or Control-D will exit without one.\n"
      "\"prompt\"/\"p\" will run two nested prompts.\n");

  input.Show();

  // This example does simple blocking input.
  while (!g_should_quit)
    input.OnInput(getc(stdin));

  return 0;
}
