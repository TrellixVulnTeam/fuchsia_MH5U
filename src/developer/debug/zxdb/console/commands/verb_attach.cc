// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_attach.h"

#include "src/developer/debug/zxdb/client/filter.h"
#include "src/developer/debug/zxdb/client/job.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/console/string_util.h"
#include "src/developer/debug/zxdb/console/verbs.h"

namespace zxdb {

namespace {

const char kAttachShortHelp[] = "attach: Attach to a running process/job.";
const char kAttachHelp[] =
    R"(attach <what>

  Attaches to a current or future process.

Atttaching to a specific process

  To attach to a specific process, supply the process' koid (process ID).
  For example:

    attach 12345

  Use the "ps" command to view the active processes, their names, and koids.

Attaching to processes by name

  Non-numeric arguments will be interpreted as a filter. A filter is a substring
  that matches any part of the process name. The filter "t" will match any
  process with the letter "t" in its name. Filters are not regular expressions.

  Filters are applied to processes launched in jobs the debugger is attached to,
  both current processes and future ones.

  More on jobs:

    • See the currently attached jobs with the "job" command.

    • Attach to a new job with the "attach-job" command.

  More on filters:

    • See the current filters with the "filter" command.

    • Delete a filter with "filter [X] rm" where X is the filter index from the
      "filter" list. If no filter index is provided, the current filter will be
      deleted.

    • Change a filter's pattern with "filter [X] set pattern = <newvalue>".

    • Attach to all processes in a job with "job attach *". Note that * is a
      special string for filters, regular expressions are not supported.

  If a job prefix is specified, only processes launched in that job matching the
  pattern will be attached to:

    job attach foo      // Uses the current job context.
    job 2 attach foo    // Specifies job context #2.

  If you have a specific job koid (12345) and want to watch "foo" processes in
  it, a faster way is:

    attach-job 12345 foo

Examples

  attach 2371
      Attaches to the process with koid 2371.

  process 4 attach 2371
      Attaches process context 4 to the process with koid 2371.

  attach foobar
      Attaches to any process that spawns under any job the debugger is attached
      to with "foobar" in the name.

  job 3 attach foobar
      Attaches to any process that spawns under job 3 with "foobar" in the
      name.
)";

// This should match ZX_MAX_NAME_LEN, but we don't want to include zircon headers here.
constexpr size_t kZirconMaxNameLength = 32;

bool HasAttachedJob(const System* system) {
  for (const Job* job : system->GetJobs()) {
    if (job->state() == Job::State::kAttached)
      return true;
  }
  return false;
}

Err RunVerbAttach(ConsoleContext* context, const Command& cmd, CommandCallback callback) {
  // Only a process can be attached.
  if (Err err = cmd.ValidateNouns({Noun::kProcess, Noun::kJob}); err.has_error())
    return err;

  uint64_t koid = 0;
  if (ReadUint64Arg(cmd, 0, "process koid", &koid).ok()) {
    // Check for duplicate koids before doing anything else to avoid creating a container target
    // in this case. It's easy to hit enter twice which will cause a duplicate attach. The
    // duplicate target is the only reason to check here, the attach will fail later if there's
    // a duplicate (say, created in a race condition).
    if (context->session()->system().ProcessFromKoid(koid))
      return Err("Process " + std::to_string(koid) + " is already being debugged.");

    // Attach to a process by KOID.
    auto err_or_target = GetRunnableTarget(context, cmd);
    if (err_or_target.has_error())
      return err_or_target.err();
    err_or_target.value()->Attach(
        koid, [callback = std::move(callback)](fxl::WeakPtr<Target> target, const Err& err,
                                               uint64_t timestamp) mutable {
          // Don't display a message on success because the ConsoleContext will print the new
          // process information when it's detected.
          ProcessCommandCallback(target, false, err, std::move(callback));
        });
    return Err();
  }

  // Not a number, make a filter instead. This only supports only "job" nouns.
  if (cmd.ValidateNouns({Noun::kJob}).has_error()) {
    return Err(
        "Attaching by process name (a non-numeric argument)\nonly supports the \"job\" noun.");
  }
  if (cmd.args().size() != 1)
    return Err("Wrong number of arguments to attach.");

  Job* job = cmd.HasNoun(Noun::kJob) && cmd.job() ? cmd.job() : nullptr;
  std::string pattern = cmd.args()[0];
  if (!job && pattern == Filter::kAllProcessesPattern) {
    // Bad things happen if we try to attach to all processes in the system, try to make this
    // more difficult by preventing attaching to * with no specific job.
    return Err("Use a specific job (\"job 3 attach *\") when attaching to all processes.");
  }

  // Display a warning if there are no attached jobs. The debugger tries to attach to the root job
  // by default but if this fails (say there is more than one debug agent), attach will surprisingly
  // fail.
  if (!HasAttachedJob(&context->session()->system())) {
    OutputBuffer warning;
    warning.Append(Syntax::kWarning, GetExclamation());
    warning.Append(
        " There are currently no attached jobs. This could be because you\n"
        "haven't attached to any, or because auto-attaching to the default jobs\n"
        "failed (this can happen if there are more than one debug agents running).\n"
        "Since attaching by name only applies to attached jobs, nothing will happen\n"
        "until you attach to a job (\"attach-job <job-koid>\").\n\n");
    Console::get()->Output(warning);
  }

  if (pattern.size() > kZirconMaxNameLength) {
    Console::get()->Output(OutputBuffer(
        Syntax::kWarning,
        "The filter is trimmed to " + std::to_string(kZirconMaxNameLength) +
            " characters because it's the maximum length for a process name in Zircon."));
    pattern.resize(kZirconMaxNameLength);
  }

  Filter* filter = context->session()->system().CreateNewFilter();
  filter->SetJob(job);
  filter->SetPattern(pattern);

  context->SetActiveFilter(filter);

  // This doesn't use the default filter formatting to try to make it friendlier for people
  // that are less familiar with the debugger and might be unsure what's happening (this is normally
  // one of the first things people do in the debugger. The filter number is usually not relevant
  // anyway.
  Console::get()->Output("Waiting for process matching \"" + pattern +
                         "\".\n"
                         "Type \"filter\" to see the current filters.");
  if (callback) {
    callback(Err());
  }
  return Err();
}

}  // namespace

VerbRecord GetAttachVerbRecord() {
  VerbRecord attach(&RunVerbAttach, {"attach"}, kAttachShortHelp, kAttachHelp,
                    CommandGroup::kProcess);
  return attach;
}

}  // namespace zxdb
