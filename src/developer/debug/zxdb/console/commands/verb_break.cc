// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_break.h"

#include "src/developer/debug/zxdb/client/breakpoint.h"
#include "src/developer/debug/zxdb/client/breakpoint_location.h"
#include "src/developer/debug/zxdb/client/breakpoint_settings.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/console_context.h"
#include "src/developer/debug/zxdb/console/input_location_parser.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/console/verbs.h"

namespace zxdb {

namespace {

constexpr int kSizeSwitch = 1;
constexpr int kStopSwitch = 2;
constexpr int kDisabledSwitch = 3;
constexpr int kTypeSwitch = 4;
constexpr int kOneShotSwitch = 5;
constexpr int kMultSwitch = 6;

const char kBreakShortHelp[] = "break / b: Create a breakpoint.";
const char kBreakHelp[] =
    R"(break [ <location> ]

  Alias: "b"

  Creates or modifies a breakpoint. Not to be confused with the "breakpoint" /
  "bp" noun which lists breakpoints and modifies the breakpoint context. See
  "help bp" for more.

  The new breakpoint will become the active breakpoint so future breakpoint
  commands will apply to it by default.

Location arguments

  Current frame's address (no input)
      break

)" LOCATION_ARG_HELP("break") LOCATION_EXPRESSION_HELP("break")
        R"(  You can also specify the magic symbol "$main" to break on the process'
  entrypoint:
      break $main

Options

  --disabled
  -d
      Creates the breakpoint as initially disabled. Otherwise, it will be
      enabled.

  --hit-mult=<count>
  -m <count>
      Only breaks the execution every <count> times the breakpoint is hit.

  --one-shot
  -o
      Creates a one-shot breakpoint. One-shot breakpoints are automatically
      deleted after they are hit once.

  --size=<byte-size>
  -s <byte-size>
      Size in bytes for hardware write and read-write breakpoints. This will
      default to 4 if unspecified. Not valid for hardware or software execution
      breakpoints. The address will need to be aligned to an even multiple of
      its size.

  --stop=[ all | process | thread | none ]
  -p [ all | process | thread | none ]
      Controls what execution is stopped when the breakpoint is hit. By
      default all threads of all debugged process will be stopped ("all") when
      a breakpoint is hit. But it's possible to only stop the threads of the
      current process ("process") or the thread that hit the breakpoint
      ("thread").

      If "none" is specified, any threads hitting the breakpoint will
      immediately resume, but the hit count will continue to accumulate.

  --type=<type>
  -t <type>
      The type of the breakpoint. Defaults to "software". Possible values are:

)" BREAKPOINT_TYPE_HELP("      ")
            R"(
Scoping to processes and threads

  Explicit context can be provided to scope a breakpoint to a single process
  or a single thread. To do this, provide that process or thread as context
  before the break command:

    t 1 b *0x614a19837
    thread 1 break *0x614a19837
        Breaks on only this thread in the current process.

    pr 2 b *0x614a19837
    process 2 break *0x614a19837
        Breaks on all threads in the given process.

  When the thread of a thread-scoped breakpoint is destroyed, the breakpoint
  will be converted to a disabled process-scoped breakpoint. When the process
  context of a process-scoped breakpoint is destroyed, the breakpoint will be
  converted to a disabled global breakpoint.

ELF PLT breakpoints for system calls

  Breakpoints can be set in the code in the ELF Procedure Linkage Table. This
  code is the tiny stub that the dynamic linker fixes up to resolve each
  function call imported from other ELF objects.

  This allows is setting breakpoints on system calls without using hardware
  breakpoints. The Zircon vDSO is mapped read-only which prevents the debugger
  from inserting hardware breakpoints. But each library's calls to vDSO
  functions goes through that library's PLT which is writable by the debugger.

  To indicate a PLT breakpoint, use the form $plt(...):

    [zxdb] break $plt(zx_debug_write)

  This will apply the breakpoint to every library's PLT entry for
  "zx_debug_write".

  The supplied string must be the exact name in the ELF binary. This means C++
  symbols must be mangled.

Breakpoints on overloaded functions

  If a named function has multiple overloads, the debugger will set a breakpoint
  on all of them. Specifying an individual overload by name is not supported
  (bug 41928).

  To refer to an individual overload, either refer to the location by file:line
  or by address. To get the addresses of each overload, use the command
  "sym-info FunctionName".

Editing breakpoint attributes

  Individual breakpoint attributes can be accessed with the "get" and "set"
  commands. To list all attributes on the current breakpoint:

    bp get

  To get a specific value along with help for what the setting means, give the
  specific attribute:

    bp get stop

  And to set the attribute:

    bp set stop = thread

Other breakpoint commands

  "breakpoint" / "bp": List or select breakpoints.
  "clear": To delete breakpoints.
  "disable": Disable a breakpoint without deleting it.
  "enable": Enable a previously-disabled breakpoint.
  "watch": Create a hardware write breakpoint.

Examples

  break
      Set a breakpoint at the current frame's address.

  frame 1 break
      Set a breakpoint at the specified frame's address. Since frame 1 is
      always the current function's calling frame, this command will set a
      breakpoint at the current function's return.

  break MyClass::MyFunc
      Breakpoint in all processes that have a function with this name.

  break 0x123c9df
  break *$rip + 0x10
      Process-specific breakpoint at the given address.

  process 3 break MyClass::MyFunc
      Process-specific breakpoint at the given function.

  thread 1 break foo.cpp:34
      Thread-specific breakpoint at the give file/line.

  break 23
      Break at line 23 of the file referenced by the current frame.

  frame 3 break 23
      Break at line 23 of the file referenced by frame 3.

  break --type execute 23
      Break at line 23 of the file referenced by the current frame and use a
      hardware execution breakpoint.
)";

void OutputCreatedMessage(ConsoleContext* context, Breakpoint* breakpoint) {
  OutputBuffer out("Created ");
  out.Append(FormatBreakpoint(context, breakpoint, true));
  Console::get()->Output(out);
}

Err RunVerbBreak(ConsoleContext* context, const Command& cmd, CommandCallback cb) {
  Err err = cmd.ValidateNouns({Noun::kProcess, Noun::kThread, Noun::kFrame, Noun::kBreakpoint});
  if (err.has_error())
    return err;

  // Get existing settings (or defaults for new one).
  BreakpointSettings settings;

  // Disabled flag.
  if (cmd.HasSwitch(kDisabledSwitch))
    settings.enabled = false;

  // One-shot.
  if (cmd.HasSwitch(kOneShotSwitch))
    settings.one_shot = true;

  // Stop mode.
  if (cmd.HasSwitch(kStopSwitch)) {
    auto stop_mode = BreakpointSettings::StringToStopMode(cmd.GetSwitchValue(kStopSwitch));
    if (!stop_mode) {
      return Err(
          "--%s requires \"%s\", \"%s\", \"%s\", or \"%s\".", ClientSettings::Breakpoint::kStopMode,
          ClientSettings::Breakpoint::kStopMode_All, ClientSettings::Breakpoint::kStopMode_Process,
          ClientSettings::Breakpoint::kStopMode_Thread, ClientSettings::Breakpoint::kStopMode_None);
    }
    settings.stop_mode = *stop_mode;
  }

  // Type.
  settings.type = BreakpointSettings::Type::kSoftware;
  if (cmd.HasSwitch(kTypeSwitch)) {
    if (auto opt_type = BreakpointSettings::StringToType(cmd.GetSwitchValue(kTypeSwitch)))
      settings.type = *opt_type;
    else
      return Err("Unknown breakpoint type.");
  }

  // Size. Track if this is set or not si we can change the default based on the expression result.
  bool has_explicit_size = false;
  if (cmd.HasSwitch(kSizeSwitch)) {
    has_explicit_size = true;

    if (!BreakpointSettings::TypeHasSize(settings.type))
      return Err("Breakpoint size is only supported for write and read-write breakpoints.");
    // TODO(dangyi): settings.byte_size should be validated by BreakpointSettings::ValidateSize.
    if (Err err = StringToUint32(cmd.GetSwitchValue(kSizeSwitch), &settings.byte_size);
        err.has_error())
      return err;
  } else if (BreakpointSettings::TypeHasSize(settings.type)) {
    settings.byte_size = 4;  // Default size.
  }

  // Scope.
  settings.scope = ExecutionScopeForCommand(cmd);

  // Hit mult.
  if (cmd.HasSwitch(kMultSwitch)) {
    int hit_mult;
    if (Err err = StringToInt(cmd.GetSwitchValue(kMultSwitch), &hit_mult); err.has_error())
      return err;
    // TODO(dangyi): Unify validation logics with settings.
    if (hit_mult <= 0)
      return Err("hit-mult must be positive.");

    settings.hit_mult = hit_mult;
  }

  if (cmd.args().empty()) {
    // Creating a breakpoint with no location implicitly uses the current frame's current
    // location.
    if (!cmd.frame()) {
      return Err(ErrType::kInput,
                 "There isn't a current frame to take the breakpoint location from.");
    }

    // Use the file/line of the frame if available. This is what a user will generally want to see
    // in the breakpoint list, and will persist across restarts. Fall back to an address
    // otherwise. Sometimes the file/line might not be what they want, though.
    const Location& frame_loc = cmd.frame()->GetLocation();
    if (frame_loc.has_symbols())
      settings.locations.emplace_back(frame_loc.file_line());
    else
      settings.locations.emplace_back(cmd.frame()->GetAddress());

    // New breakpoint.
    Breakpoint* breakpoint = context->session()->system().CreateNewBreakpoint();
    context->SetActiveBreakpoint(breakpoint);

    breakpoint->SetSettings(settings);
    OutputCreatedMessage(context, breakpoint);
    if (cb)
      cb(err);
    return Err();
  }

  // Parse the given input location in args[0]. This may require async evaluation.
  EvalLocalInputLocation(
      GetEvalContextForCommand(cmd), cmd.frame(), cmd.args()[0],
      [settings, has_explicit_size, cb = std::move(cb)](ErrOr<std::vector<InputLocation>> locs,
                                                        std::optional<uint32_t> expr_size) mutable {
        if (locs.has_error()) {
          Console::get()->Output(locs.err());
          if (cb)
            cb(locs.err());
          return;
        }

        // New breakpoint.
        ConsoleContext* context = &Console::get()->context();
        Breakpoint* breakpoint = context->session()->system().CreateNewBreakpoint();
        context->SetActiveBreakpoint(breakpoint);

        if (BreakpointSettings::TypeHasSize(settings.type) && !has_explicit_size && expr_size) {
          // Input expression has a size we should default to.
          settings.byte_size = *expr_size;
        }

        settings.locations = locs.take_value();
        breakpoint->SetSettings(settings);

        OutputCreatedMessage(context, breakpoint);
        if (cb)
          cb(Err());
      });

  return Err();
}

}  // namespace

VerbRecord GetBreakVerbRecord() {
  SwitchRecord disabled_switch(kDisabledSwitch, false, "disabled", 'd');
  SwitchRecord one_shot_switch(kOneShotSwitch, false, ClientSettings::Breakpoint::kOneShot, 'o');
  SwitchRecord size_switch(kSizeSwitch, true, ClientSettings::Breakpoint::kSize, 's');
  SwitchRecord stop_switch(kStopSwitch, true, ClientSettings::Breakpoint::kStopMode, 'p');
  SwitchRecord type_switch(kTypeSwitch, true, ClientSettings::Breakpoint::kType, 't');
  SwitchRecord mult_switch(kMultSwitch, true, ClientSettings::Breakpoint::kHitMult, 'm');

  VerbRecord break_record(&RunVerbBreak, &CompleteInputLocation, {"break", "b"}, kBreakShortHelp,
                          kBreakHelp, CommandGroup::kBreakpoint);
  break_record.param_type = VerbRecord::kOneParam;  // Don't require quoting for expressions.

  break_record.switches.push_back(disabled_switch);
  break_record.switches.push_back(one_shot_switch);
  break_record.switches.push_back(size_switch);
  break_record.switches.push_back(stop_switch);
  break_record.switches.push_back(type_switch);
  break_record.switches.push_back(mult_switch);
  return break_record;
}

}  // namespace zxdb
