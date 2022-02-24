#!/bin/bash
# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# See usage() for description.

script="$0"
script_dir="$(dirname "$script")"

remote_action_wrapper="$script_dir"/fuchsia-rbe-action.sh

# The project_root must cover all inputs, prebuilt tools, and build outputs.
# This should point to $FUCHSIA_DIR for the Fuchsia project.
# ../../ because this script lives in build/rbe.
# The value is an absolute path.
project_root="$(readlink -f "$script_dir"/../..)"

# This is where the working directory happens to be in remote execution.
# This assumed constant is only needed for a few workarounds elsewhere
# in this script.
remote_project_root="/b/f/w"

# Script to check (local) determinism.
check_determinism_command=(
  ../../build/tracer/output_cacher.py
  --check-repeatability
)

function usage() {
cat <<EOF
This wrapper script helps dispatch a remote Rust compilation action by inferring
the necessary inputs for uploading based on the command-line.

$script [options] -- rustc-command...

Options:
  --help|-h: print this help and exit
  --local: disable remote execution and run the original command locally.
    The --remote-disable fake rust flag (passed after the -- ) has the same
    effect, and is removed from the executed command.
  --verbose|-v: print debug information, including details about uploads.
  --dry-run: print remote execution command without executing (remote only).

  --project-root: location of source tree which also encompasses outputs
      and prebuilt tools, forwarded to --exec-root in the reclient tools.
      [default: inferred based on location of this script]

  --source FILE: the Rust source root for the crate being built
      [default: inferred as the first .rs file in the rustc command]
  --depfile FILE: the dependency file that the command is expected to write.
      This file lists all source files needed for this crate.
      [default: this is inferred from --emit=dep-info=FILE]

  --fsatrace:
      for --local execution: record files accessed at \$output.fsatrace.
      for remote execution: record files accessed at \$output.remote-fsatrace.
      This will also trace the depfile generation step.

  --compare: In this mode, build locally and remotely (sequentially) and
      compare the outputs, failing if there are any differences.
      On comparison failure, if --fsatrace is enabled, compare file accesses.

  --check-determinism: [requires --local]
      Locally run the same command twice and compare outputs.

  There are two ways to forward options to $remote_action_wrapper,
  most of which are forwarded to 'rewrapper':

    Before -- : all unhandled flags are forwarded to $remote_action_wrapper.

    After -- : --remote-flag=* will be forwarded to $remote_action_wrapper
      and removed from the remote command.

  See '$remote_action_wrapper --help' for additional debug features.

If the rust-command contains --remote-inputs=..., those will be interpreted
as extra --inputs to upload, and removed from the command prior to local and
remote execution.
The option argument is a comma-separated list of files, relative to
\$project_root.
Analogously, --remote-outputs=... will be interpreted as extra --output_files
to download, and removed from the command prior to local and remote execution.

Detected parameters:
  project_root: $project_root
EOF
}

local_only=0
trace=0
dry_run=0
verbose=0
compare=0
rewrapper_options=()
check_determinism=0

# Extract script options before --
for opt
do
  # handle --option arg
  if test -n "$prev_opt"
  then
    eval "$prev_opt"=\$opt
    prev_opt=
    shift
    continue
  fi
  # Extract optarg from --opt=optarg
  case "$opt" in
    *=?*) optarg=$(expr "X$opt" : '[^=]*=\(.*\)') ;;
    *=) optarg= ;;
  esac
  case "$opt" in
    --help|-h) usage ; exit ;;
    --dry-run) dry_run=1 ;;
    --local) local_only=1 ;;
    --fsatrace) trace=1 ;;
    --verbose|-v) verbose=1 ;;
    --compare) compare=1 ;;
    --check-determinism) check_determinism=1 ;;
    --project-root=*) project_root="$optarg" ;;
    --project-root) prev_opt=project_root ;;
    --source=*) top_source="$optarg" ;;
    --source) prev_opt=top_source ;;
    --depfile=*) depfile="$optarg" ;;
    --depfile) prev_opt=depfile ;;
    # stop option processing
    --) shift; break ;;
    # Forward all other options to rewrapper
    *) rewrapper_options+=( "$opt" ) ;;
  esac
  shift
done
test -z "$prev_out" || { echo "Option is missing argument to set $prev_opt." ; exit 1;}

build_subdir="$(realpath --relative-to="$project_root" . )"
project_root_rel="$(realpath --relative-to=. "$project_root")"

# For debugging, trace the files accessed.
fsatrace="$project_root_rel"/prebuilt/fsatrace/fsatrace

detail_diff="$script_dir"/detail-diff.sh

# Modify original command to extract dep-info only (fast).
# Start with `env` in case command starts with environment variables.
# Note: `env` is not $root_build_dir/env, which comes from
# //third_party/sbase:env.  This assumes that this tool path is
# available on the remote executor, and avoids a dependency on the host-built
# `env` binary.  Since /usr/bin/env lies outside of $exec_root,
# reproxy will not consider this as an input (for uploading/caching).
env=/usr/bin/env
dep_only_command=("$env")

# Infer the source_root.
first_source=

# C toolchain linker
linker=()
link_arg_files=()
link_sysroot=()

# input files referenced in environment variables
envvar_files=()

rust_lld=()

# Remove these temporary files on exit.
cleanup_files=()
function cleanup() {
  rm -f "${cleanup_files[@]}"
}
trap cleanup EXIT

print_var() {
  # Prints variable values to stdout.
  # $1 is name of variable to display.
  # The rest are array values.
  if test "$#" -le 2
  then
    echo "$1: $2"
  else
    echo "$1:"
    shift
    for f in "$@"
    do echo "  $f"
    done
  fi
}

debug_var() {
  # With --verbose, prints variable values to stdout.
  test "$verbose" = 0 || print_var "$@"
}

# Examine the rustc compile command
comma_remote_inputs=
comma_remote_outputs=

# Compute a rustc command suitable for local and remote execution.
# Prefix command with `env` in case it starts with local environment variables.
rustc_command=("$env")

# arch-vendor-os
target_triple=

extra_filename=
llvm_ir_output="no"
llvm_bc_output="no"

# Paths to direct dependencies.
extern_paths=()

save_analysis=0
llvm_time_trace=0

extra_linker_outputs=()

prev_opt=
for opt in "$@"
do
  # Copy most command tokens.
  dep_only_token="$opt"
  # handle --option arg
  if test -n "$prev_opt"
  then
    eval "$prev_opt"=\$opt
    case "$prev_opt" in
      remote_flag) rewrapper_options+=( "$opt" ) ;;
      comma_remote_inputs) ;;  # Remove this optarg.
      comma_remote_outputs) ;;  # Remove this optarg.
      # Copy this opt, but also append its value to extern_paths.
      extern)
        extern_path=$(expr "X$opt" : '[^=]*=\(.*\)')
        # Sometimes, --extern names only a single crate without =path.
        test -z "$extern_path" || extern_paths+=("$build_subdir/$extern_path")
        dep_only_command+=( "$dep_only_token" )
        rustc_command+=( "$opt" )
        ;;
      # Copy all others.
      *) dep_only_command+=( "$dep_only_token" )
        rustc_command+=( "$opt" )
        ;;
    esac
    prev_opt=
    shift
    continue
  fi

  # Extract optarg from --opt=optarg
  case "$opt" in
    *=?*) optarg=$(expr "X$opt" : '[^=]*=\(.*\)') ;;
    *=) optarg= ;;
  esac

  # Reject absolute paths, for the sake of build artifact portability,
  # and remote-action cache hit benefits.
  case "$opt" in
    *"$project_root"*)
      cat <<EOF
Absolute paths are not remote-portable.  Found:
  $opt
Please rewrite the command without absolute paths.
EOF
      exit 1
      ;;
  esac

  case "$opt" in
    # This is the (likely prebuilt) rustc binary.
    */rustc) rustc="$opt" ;;

    # This is equivalent to --local, but passed as a rustc flag,
    # instead of wrapper script flag (before the -- ).
    --remote-disable)
      local_only=1
      shift
      continue
      ;;

    # --remote-inputs signals to the remote action wrapper,
    # and not the actual rustc command.
    --remote-inputs=*)
      comma_remote_inputs="$optarg"
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;
    --remote-inputs)
      prev_opt=comma_remote_inputs
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;

    # --remote-outputs signals to the remote action wrapper,
    # and not the actual rustc command.
    --remote-outputs=*)
      comma_remote_outputs="$optarg"
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;
    --remote-outputs) prev_opt=comma_remote_outputs
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;

    # Redirect these flags to rewrapper.
    --remote-flag=*)
      rewrapper_options+=( "$optarg" )
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;
    --remote-flag) prev_opt=remote_flag
      # Remove this from the actual command to be executed.
      shift
      continue
      ;;

    # -o path/to/output.rlib
    -o) prev_opt=output ;;

    # Capture arch-vendor-os triple.
    --target) prev_opt=target_triple ;;

    -Zsave-analysis=yes | save-analysis=yes) save_analysis=1 ;;
    -Zllvm-time-trace | llvm-time-trace) llvm_time_trace=1 ;;

    # Rewrite this token to only generate dependency information (locally),
    # and do no other compilation/linking.
    # Write to a renamed depfile because the remote command will also
    # produce the originally named depfile.
    --emit=*)
      test -n "$depfile" || {
        # Parse and split --emit=...,...
        IFS=, read -ra emit_args <<< "$optarg"
        for emit_arg in "${emit_args[@]}"
        do
          # Extract VALUE from dep-info=VALUE
          case "$emit_arg" in
            *=?*) emit_value=$(expr "X$emit_arg" : '[^=]*=\(.*\)') ;;
            *=) emit_value= ;;
          esac
          case "$emit_arg" in
            dep-info=*) depfile="$emit_value" ;;
            llvm-ir) llvm_ir_output="yes" ;;
            llvm-bc) llvm_bc_output="yes" ;;
          esac
        done
      }
      depfile="${depfile#./}"
      dep_only_token="--emit=dep-info=$depfile.nolink"
      # Tell rustc to report all transitive *library* dependencies,
      # not just the sources, because these all need to be uploaded.
      # This includes (prebuilt) system libraries as well.
      # TODO(https://fxbug.dev/78292): this -Z flag is not known to be stable yet.
      dep_only_command+=( "-Zbinary-dep-depinfo" )
      ;;

    -Cextra-filename=*) extra_filename="$optarg" ;;
    -Cextra-filename) prev_opt=extra_filename ;;

    # --crate-type cdylib needs rust-lld (hard-coding this is a hack)
    cdylib)
      _rust_lld_rel="$(dirname "$rustc")"/../lib/rustlib/x86_64-unknown-linux-gnu/bin/rust-lld
      rust_lld=("$(realpath --relative-to="$project_root" "$_rust_lld_rel")")
      ;;

    # Detect custom linker, preserve symlinks
    -Clinker=*)
        linker_local="$optarg"
        linker=("$(realpath -s --relative-to="$project_root" "$linker_local")")
        debug_var "[from -Clinker]" "${linker[@]}"
        ;;

    -Clink-arg=* )
        case "$optarg" in
          # sysroot is a directory with system libraries
          --sysroot=* )
            sysroot="$(expr "X$optarg" : '[^=]*=\(.*\)')"
            sysroot_relative="$(realpath --relative-to="$project_root" "$sysroot")"
            debug_var "[from -Clink-arg=--sysroot]" "$sysroot_relative"
            link_sysroot=("$sysroot_relative")
            ;;

          # Link arguments that reference .o or .a files need to be uploaded.
          *.o | *.a | *.so | *.so.debug | *.ld )
            link_arg="$build_subdir/$optarg"
            debug_var "[from -Clink-arg]" "$link_arg"
            link_arg_files+=("$link_arg")
          ;;
        esac
        ;;

    # Linker can produce a .map output file.
    -Clink-args=--Map=* | link-args=--Map=*)
        map_output="$(expr "X$optarg" : '[^=]*=\(.*\)')"
        map_output_stripped="${map_output#./}"
        extra_linker_outputs+=( "$map_output_stripped" )
        ;;

    # This flag informs the linker where to search for libraries.
    -Lnative=* )
        if test -d "$optarg"
        then
          # Rather than grab the entire directory (whose contents are not stable
          # due to temporary files being written during a build), list specific
          # shared objects and archives.  Observed temporary files include
          # .o.tmp files.
          #
          # Caveat: if the same dir is home to more than one archive produced by
          # parallel build actions, the partial directory listing may not be
          # stable throughout the entire build, and be subject to race
          # conditions.
          #
          # Some of these directories contain both .a and .o files.
          # It is not yet clear whether the .o files are needed when there is a
          # .a archive.
          # There are also directories that contain only .o files.
          #
          # Over-specifying inputs comes with a danger: referring to inputs that
          # are not actually used/needed in remote execution could reference
          # files that are being overwritten somewhere else in the build.
          #
          objs_unfiltered=( "$optarg"/*.{so,a,o} )
          objs_rel=()
          # Remove elements with a literal "*" in them, which occurs when *.x
          # matches nothing.
          for f in "${objs_unfiltered[@]}"
          do case "$f" in
            *\** ) ;;
            # prefix each element with "$build_subdir/"
            *) objs_rel+=("$build_subdir/$f") ;;
          esac
          done
          debug_var "[from -Lnative (dir:$optarg) (excluded from upload)]" "${objs_rel[@]}"
          # We found that builds work without -Lnative, so until we find
          # otherwise, we ignore files found in these directories.
          # link_arg_files+=("${objs_rel[@]}")
        else
          link_arg="$build_subdir/$optarg"
          debug_var "[from -Lnative (file:$optarg)]" "$link_arg"
          link_arg_files+=("$link_arg")
        fi
        ;;

    # Paths of direct dependency rlibs/dylibs: --extern CRATE=LOCATION
    # Normally, we rely solely on the depfile as complete source of files
    # needed, but this was added to workaround a bug in depfile generation.
    # There may be redundant entries between this and the depfiles, but the
    # reclient tools de-dupe for us.
    --extern ) prev_opt=extern ;;

    --*=* ) ;;  # forward

    # Forward other environment variables (or similar looking).
    *=*) ;;

    # Capture the first named source file as the source-root.
    *.rs) test -n "$first_source" || first_source="$opt" ;;

    *.a | *.o | *.so | *.so.debug)
        link_arg_files+=("$build_subdir/$opt")
        ;;

    # Preserve all other tokens.
    *) ;;
  esac
  # Copy tokens to craft a local command for dep-info.
  dep_only_command+=( "$dep_only_token" )
  # Copy tokens to craft a command for local and remote execution.
  rustc_command+=("$opt")
  shift
done
test -z "$prev_out" || { echo "Option is missing argument to set $prev_opt." ; exit 1;}


local_trace_prefix=()
test "$trace" = 0 || {
  local_trace_prefix=(
    env FSAT_BUF_SIZE=5000000
    "$fsatrace" ertwdmq "$output".fsatrace --
  )
}

# Specify the rustc binary to be uploaded.
rustc_relative="$(realpath --relative-to="$project_root" "$rustc")"

# Collect extra inputs to upload for remote execution.
# Note: these paths are relative to the current working dir ($build_subdir),
# so they need to be adjusted relative to $project_root below, before passing
# them to rewrapper.
extra_inputs=()
IFS=, read -ra extra_inputs <<< "$comma_remote_inputs"

# Collect extra outputs to download after remote execution.
extra_outputs=()
IFS=, read -ra extra_outputs <<< "$comma_remote_outputs"

# TODO(fangism): if possible, determine these shlibs statically to avoid `ldd`-ing.
# TODO(fangism): for host-independence, use llvm-otool and `llvm-readelf -d`,
#   which requires uploading more tools.
function nonsystem_shlibs() {
  # $1 is a binary
  ldd "$1" | grep "=>" | cut -d\  -f3 | \
    grep -v -e '^/lib' -e '^/usr/lib' | \
    xargs -n 1 realpath --relative-to="$project_root"
}

function depfile_inputs_by_line() {
  # From a depfile arrange deps one per line.
  # This looks at the phony deps "FILE:".
  grep ":$" "$1" | cut -d: -f1
}

# Workaround reclient bug: strip prefix ./ from all outputs.
output="${output#./}"

# The rustc binary might be linked against shared libraries.
# Exclude system libraries in /usr/lib and /lib.
# convert to paths relative to $project_root for rewrapper.
mapfile -t rustc_shlibs < <(nonsystem_shlibs "$rustc")

# Rust standard libraries.
rust_stdlib_dir="prebuilt/third_party/rust/linux-x64/lib/rustlib/$target_triple/lib"
# The majority of stdlibs already appear in dep-info and are uploaded as needed.
# However, libunwind.a is not listed, but is directly needed by code
# emitted by rustc.  Listing this here works around a missing upload issue,
# and adheres to the guidance of listing files instead of whole directories.
extra_rust_stdlibs=()
if test -f "$project_root/$rust_stdlib_dir"/libunwind.a
then extra_rust_stdlibs+=("$rust_stdlib_dir"/libunwind.a)
fi

# At this time, the linker we pass is known to be statically linked itself
# and doesn't need to be accompanied by any shlibs.

# If --source was not specified, infer it from the command-line.
# This source file is likely to appear as the first input in the depfile.
test -n "$top_source" || top_source="$first_source"
top_source="$(realpath --relative-to="$project_root" "$top_source")"

# Locally generate a depfile only and read it as list of files to upload.
# These inputs appear relative to the build/output directory, but need to be
# relative to the $project_root for rewrapper.
depfile_trace="$depfile.nolink.trace"
trace_depfile_scanning_prefix=(
  env FSAT_BUF_SIZE=5000000
  "$fsatrace" erwmdtq "$depfile_trace" --
)
"${dep_only_command[@]}" || {
  status=$?
  echo "Depfile generation failed.  Aborting."
  echo "Re-run with --verbose or --fsatrace for more details."

  # If depfile generation fails, and tracing is requested,
  # re-run it with tracing to examine the files it accessed.
  test "$trace" = 0 || {
    "${trace_depfile_scanning_prefix[@]}" "${dep_only_command[@]}" > /dev/null 2>&1
    echo "File access trace [$depfile_trace]:"
    cat "$depfile_trace"
    echo
  }

  # Show the dep-info command only when --verbose is requested.
  debug_var "[$script: dep-info]" "${dep_only_command[@]}"

  exit "$status"
}

# TEMPORARY WORKAROUND until upstream fix lands:
#   https://github.com/pest-parser/pest/pull/522
# Remove redundant occurences of the current working dir absolute path.
# Paths should be relative to the root_build_dir.
sed -i -e "s|$PWD/||g" "$depfile.nolink"

# Convert depfile absolute paths to relative.
mapfile -t depfile_inputs < <(depfile_inputs_by_line "$depfile.nolink" | \
  xargs -n 1 realpath --relative-to="$project_root" )
# Done with temporary depfile, remove it.
rm -f "$depfile.nolink"

# Some Rust libraries come with both .rlib and .so (like libstd), however,
# the depfile generator fails to list the .so file in some cases,
# which causes the build to silently fallback to static linking when
# dynamic linking is requested and intended.  This can result in a mismatch
# between local and remote building.
# See https://github.com/rust-lang/rust/issues/90106
# Workaround (https://fxbug.dev/86896): check for existence of .so and include it.
depfile_shlibs=()
for f in "${depfile_inputs[@]}"
do
  case "$f" in
    *.rlib)
      basename="$(basename "$f" .rlib)"
      dirname="$(dirname "$f")"
      shlib="$project_root/$dirname/$basename".so
      if test -r "$shlib"
      then depfile_shlibs+=( "$dirname/$basename".so )
      fi
      ;;
  esac
done
depfile_inputs+=( "${depfile_shlibs[@]}" )

extra_outputs+=( "${extra_linker_outputs[@]}" )

test "$llvm_ir_output" = "no" || {
  # Expect a llvm-ir .ll file when building a .rlib crate or executable.
  extra_outputs+=( "$(dirname "$output")/$(basename "$output" .rlib)$extra_filename".ll )
}
test "$llvm_bc_output" = "no" || {
  # Expect a llvm-bc .bc file when building a .rlib crate or executable.
  extra_outputs+=( "$(dirname "$output")/$(basename "$output" .rlib)$extra_filename".bc )
}

test "$save_analysis" = 0 || {
  analysis_file=save-analysis-temp/"$(basename "$output" .rlib)".json
  analysis_file_stripped="${analysis_file#./}"
  extra_outputs+=( "$analysis_file_stripped" )
}

test "$llvm_time_trace" = 0 || {
  llvm_trace_file="${output%.rlib}".llvm_timings.json
  extra_outputs+=( "$llvm_trace_file" )
}

# When using the linker, also grab the necessary libraries.
clang_dir=()
lld=()
libcxx=()
rt_libdir=()
test "${#linker[@]}" = 0 || {
  # Assuming the linker is found in $clang_dir/bin/
  clang_dir=("$(dirname "$(dirname "${linker[0]}")")")
  clang_dir_local=("$(dirname "$(dirname "${linker_local[0]}")")")

  # ld.lld -> lld, but the symlink is required for the clang linker driver
  # to be able to use lld.
  lld=( "$(dirname "${linker[0]}")"/ld.lld )

  objdump="$clang_dir_local"/bin/llvm-objdump
  readelf="$clang_dir_local"/bin/llvm-readelf
  dwarfdump="$clang_dir_local"/bin/llvm-dwarfdump

  # These mappings were determined by examining the options available
  # in the clang lib dir, and verifying against traces of libraries accessed
  # by local builds.
  case "$target_triple" in
    aarch64-fuchsia | aarch64-*-fuchsia)
      clang_lib_triple="aarch64-unknown-fuchsia" ;;
    aarch64-linux-gnu | aarch64-*-linux-gnu)
      clang_lib_triple="aarch64-unknown-linux-gnu" ;;
    x86_64-fuchsia | x86_64-*-fuchsia)
      clang_lib_triple="x86_64-unknown-fuchsia" ;;
    x86_64-linux-gnu | x86_64-*-linux-gnu)
      clang_lib_triple="x86_64-unknown-linux-gnu" ;;
    *) echo "[$script]: unhandled case for clang lib dir: $target_triple"
      exit 1
      ;;
  esac

  # Linking with clang++ generally requires libc++.
  libcxx=( "${clang_dir[0]}"/lib/"$clang_lib_triple"/libc++.a )

  # Location of clang_rt.crt{begin,end}.o and libclang_rt.builtins.a
  # * is a version number like 14.0.0.
  # For now, we upload the entire rt lib dir.
  rt_libdir_local=( "$clang_dir_local"/lib/clang/*/lib/"$clang_lib_triple" )
  rt_libdir=( "$(realpath --relative-to="$project_root" "${rt_libdir_local[@]}" )" )
}

extra_inputs_rel_project_root=()
for f in "${extra_inputs[@]}"
do
  extra_inputs_rel_project_root+=( "$(realpath --relative-to="$project_root" "$f" )" )
done

case "$target_triple" in
  aarch64-*-linux*) sysroot_triple=aarch64-linux-gnu ;;
  x86_64-*-linux*) sysroot_triple=x86_64-linux-gnu ;;
  *-fuchsia) sysroot_triple="" ;;
  wasm32-*) sysroot_triple="" ;;
  *) echo "[$script]: unhandled case for sysroot target subdir: $target_triple"
    exit 1
    ;;
esac

# The sysroot dir contains thousands of files, but only some essential libs and
# shlibs are needed.  Include related symlinks as well.
sysroot_files=()
test "${#link_sysroot[@]}" = 0 || {
  sysroot_dir="${link_sysroot[0]}"
  if test -n "$sysroot_triple"
  then
    # Find the correct architecture ld.so.
    case "$sysroot_triple" in
      aarch64-linux*) sysroot_files+=( "$sysroot_dir"/lib/"$sysroot_triple"/ld-linux-aarch64.so.1 ) ;;
      x86_64-linux*) sysroot_files+=( "$sysroot_dir"/lib/"$sysroot_triple"/ld-linux-x86-64.so.2 ) ;;
    esac
    sysroot_files+=(
      "$sysroot_dir"/lib/"$sysroot_triple"/libc.so.6
      "$sysroot_dir"/lib/"$sysroot_triple"/libpthread.so.0
      "$sysroot_dir"/lib/"$sysroot_triple"/libm.so.6
      "$sysroot_dir"/lib/"$sysroot_triple"/libmvec.so.1
      "$sysroot_dir"/lib/"$sysroot_triple"/librt.so.1
      "$sysroot_dir"/lib/"$sysroot_triple"/libutil.so.1
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libc.so
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libc_nonshared.a
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libpthread.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libpthread_nonshared.a
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libm.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libmvec.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libmvec_nonshared.a
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/librt.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libdl.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/libutil.{a,so}
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/Scrt1.o
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/crt1.o
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/crti.o
      "$sysroot_dir"/usr/lib/"$sysroot_triple"/crtn.o
    )
  else
    sysroot_files+=(
      "$sysroot_dir"/lib/libc.so
      "$sysroot_dir"/lib/libdl.so
      "$sysroot_dir"/lib/libm.so
      "$sysroot_dir"/lib/libpthread.so
      "$sysroot_dir"/lib/librt.so
      "$sysroot_dir"/lib/libzircon.so
      "$sysroot_dir"/lib/Scrt1.o
    )
  fi
}

# Inputs to upload include (all relative to $project_root):
#   * rust tool(s) [$rustc_relative]
#     * rust tool shared libraries [$rustc_shlibs]
#   * rust standard libraries [$extra_rust_stdlibs]
#   * direct source files [$top_source]
#   * indirect source files [$depfile.nolink]
#   * direct dependent libraries [$depfile.nolink]
#     * also from --extern CRATE=LOCATION
#   * transitive dependent libraries [$depfile.nolink]
#   * objects and libraries used as linker arguments [$link_arg_files]
#   * system rust libraries [$depfile.nolink]
#   * clang++ linker driver [$linker]
#     * libc++ [$libcxx]
#   * linker binary (called by the driver) [$lld]
#       For example: -Clinker=.../lld
#   * run-time libraries [$rt_libdir]
#   * sysroot libraries [$sysroot_files]
#   * additional data dependencies [$extra_inputs_rel_project_root]

remote_inputs=(
  "$rustc_relative"
  "${rust_lld[@]}"
  "${rustc_shlibs[@]}"
  "${extra_rust_stdlibs[@]}"
  "$top_source"
  "${depfile_inputs[@]}"
  "${extern_paths[@]}"
  "${envvar_files[@]}"
  "${linker[@]}"
  "${lld[@]}"
  "${libcxx[@]}"
  "${rt_libdir[@]}"
  "${link_arg_files[@]}"
  "${sysroot_files[@]}"
  "${extra_inputs_rel_project_root[@]}"
)

# List inputs in a file to avoid exceeding shell limit.
inputs_file_list="$output".inputs
mkdir -p "$(dirname "$inputs_file_list")"
(IFS=$'\n' ; echo "${remote_inputs[*]}") > "$inputs_file_list"
cleanup_files+=("$inputs_file_list")

# Outputs include the declared output file and a depfile.
relative_outputs=( "$output" )
test -z "$depfile" || relative_outputs+=( "$depfile" )
relative_outputs+=( "${extra_outputs[@]}" )

remote_outputs=()
remote_outputs_joined=
test "${#relative_outputs[@]}" = 0 || {
  _remote_outputs_comma="$(printf "${build_subdir}/%s," "${relative_outputs[@]}")"
  remote_outputs_joined="${_remote_outputs_comma%,}"  # get rid of last trailing comma
}

dump_vars() {
  debug_var "build subdir" "$build_subdir"
  debug_var "clang dir" "${clang_dir[@]}"
  debug_var "target triple" "$target_triple"
  debug_var "clang lib triple" "$clang_lib_triple"
  debug_var "outputs" "${remote_outputs[@]}"
  debug_var "rustc binary" "$rustc_relative"
  debug_var "rustc shlibs" "${rustc_shlibs[@]}"
  debug_var "rust stdlibs" "${extra_rust_stdlibs[@]}"
  debug_var "rust lld" "${rust_lld[@]}"
  debug_var "source root" "$top_source"
  debug_var "linker" "${linker[@]}"
  debug_var "lld" "${lld[@]}"
  debug_var "libc++" "$libcxx"
  debug_var "rt libdir" "${rt_libdir[@]}"
  debug_var "link args" "${link_arg_files[@]}"
  debug_var "link sysroot" "${link_sysroot[@]}"
  debug_var "sysroot triple" "$sysroot_triple"
  debug_var "sysroot files" "${sysroot_files[@]}"
  debug_var "env var files" "${envvar_files[@]}"
  debug_var "depfile" "$depfile"
  debug_var "[$script: dep-info]" "${dep_only_command[@]}"
  debug_var "depfile inputs" "${depfile_inputs[@]}"
  debug_var "extern paths" "${extern_paths[@]}"
  debug_var "extra inputs" "${extra_inputs_rel_project_root[@]}"
  debug_var "extra outputs" "${extra_outputs[@]}"
  debug_var "rewrapper options" "${rewrapper_options[@]}"
}

dump_vars

if test "$local_only" = 1
then
  check_determinism_prefix=()
  test "$check_determinism" = 0 || {
    # When checking determinism, backup a copy of declared outputs
    # and compare.
    check_determinism_prefix=(
      "${check_determinism_command[@]}"
      --outputs "${relative_outputs[@]}"
      --
    )
  }

  test "${#local_trace_prefix[@]}" = 0 || {
    echo "Logging file access trace to $output.fsatrace."
  }

  # Don't bother mentioning the fsatrace file as an output,
  # for checking determinism, as it may be sensitive to process id,
  # and other temporary file accesses.

  # Run original command and exit (no remote execution).
  "${check_determinism_prefix[@]}" \
    "${local_trace_prefix[@]}" \
    "${rustc_command[@]}"
  determinism_status="$?"
  case "$output" in
    host_arm64*/obj/third_party/rust_crates/*libpem-*.rlib)
      # TODO(https://fxbug.dev/86896): nondeterministic
      exit 0
      ;;
  esac
  exit "$determinism_status"
fi

# Otherwise, prepare for remote execution.

remote_trace_flags=()
test "$trace" = 0 || {
  remote_trace_flags=( --fsatrace-path="$fsatrace" )
}

# Assemble the remote execution command.
# During development, if you need to test a pre-release at top-of-tree,
# symlink the bazel-built binaries into a single directory, e.g.:
#  --bindir=$HOME/re-client/install-bin
# and pass options available in the new version, e.g.:
#  --preserve_symlink
remote_rustc_command=(
  "$remote_action_wrapper"
  --exec_root="$project_root"
  "${remote_trace_flags[@]}"
  --input_list_paths="$inputs_file_list"
  --output_files="$remote_outputs_joined"
  "${rewrapper_options[@]}"
  --
  "${rustc_command[@]}"
)

if test "$dry_run" = 1
then
  echo "[$script: skipped]:" "${remote_rustc_command[@]}"
  dump_vars
  exit
fi

# Execute the rust command remotely.
debug_var "[$script: remote]" "${remote_rustc_command[@]}"
"${remote_rustc_command[@]}"
status="$?"

# Scan remote-generated depfile for absolute paths, and reject them.
abs_deps=()
if test -f "$depfile"
then
  # TEMPORARY WORKAROUND until upstream fix lands:
  #   https://github.com/pest-parser/pest/pull/522
  # Rewrite the depfile if it contains any absolute paths from the remote
  # build; paths should be relative to the root_build_dir.
  sed -i -e "s|$remote_project_root/out/[^/]*/||g" "$depfile"

  mapfile -t remote_depfile_inputs < <(depfile_inputs_by_line "$depfile")
  for f in "${remote_depfile_inputs[@]}"
  do
    case "$f" in
      # With --exec_strategy=local, it is ok to have absolute paths under
      # $project_root because the remote cache does not see this depfile.
      # Remotely generated depfiles will not match this case because they
      # operate in a different environment, so there is no need to condition
      # this case further.
      "$project_root"/*) ;;
      /*) abs_deps+=("$f") ;;
    esac
  done
  test "${#abs_deps[@]}" = 0 || status=1
  # error message below
fi

# Workaround https://fxbug.dev/89245: relative-ize absolute path of current
# working directory in linker map files.
if test -f "$map_output"
then sed -i -e "s|$remote_project_root/|$project_root_rel/|g" "$map_output"
fi

test "$status" = 0 || {
  cat <<EOF
======== Remote Rust build action FAILED ========
This could either be a failure with the original command, or something
wrong with the remote version of the command.

Try the command locally first, without RBE, and make sure that works.
Add 'disable_rbe = true' to the problematic Rust target in GN,
or 'fx set' without '--rbe' to disable globally.

Once it passes locally, re-enable RBE.

You can manually re-run the remote command outside of the build system
to reproduce the failure (with -v for debug info):

  cd $build_subdir && $script_dir/fuchsia-reproxy-wrap.sh -- $script -v -- ...

If the remote version still fails, file a bug, and CC fuchsia-build-team@,
including output from the verbose re-run.

EOF
  # Identify which target failed by its command, useful in parallel build.
  debug_var "[$script: FAIL]" "${remote_rustc_command[@]}"
  dump_vars

  # Reject absolute paths in depfiles.
  if test "${#abs_deps[@]}" -ge 1
  then
    print_var "Error: Forbidden absolute paths in remote-generated depfile" "${abs_deps[@]}"
  fi
}

# In compare mode, also build locally and compare outputs.
# Fail if any differences are found.
test "$status" -ne 0 || test "$compare" = 0 || {
  # Backup remote outputs.
  for f in "${outputs[@]}"
  do
    out_rel="${f#$build_subdir/}"
    mv "$out_rel"{,.remote}
  done

  # Run locally.
  "${local_trace_prefix[@]}" "${rustc_command[@]}" || {
    status=$?
    echo "Local command failed for comparison: ${rustc_command[@]}"
    exit "$status"
  }

  # Workaround https://fxbug.dev/89245: relative-ize absolute path of current
  # working directory in linker map files.
  if test -f "$map_output"
  then sed -i -e "s|$project_root/|$project_root_rel/|g" "$map_output"
  fi

  # TEMPORARY WORKAROUND until upstream fix lands:
  #   https://github.com/pest-parser/pest/pull/522
  # Remove redundant occurences of the current working dir absolute path.
  # Paths should be relative to the root_build_dir.
  sed -i -e "s|$PWD/||g" "$depfile"

  # Compare outputs.
  output_diffs=()
  for f in "${outputs[@]}"
  do
    out_rel="${f#$build_subdir/}"
    if cmp "$out_rel"{,.remote}
    then
      # Reclaim space when outputs match.
      rm -f "$out_rel".remote
    else
      # cmp already reports that files differ.
      output_diffs+=("$out_rel")
    fi
  done

  test "${#output_diffs[@]}" = 0 || {
    echo "*** Differences between local (-) and remote (+) build outputs found. ***"
    for f in "${output_diffs[@]}"
    do
      echo "  $build_subdir/$f vs."
      echo "    $build_subdir/$f.remote"
      echo
      "$detail_diff" "$f"{,.remote} || :
      echo
      echo "------------------------------------"
    done

    # If we 'fsatrace'd local and remote actions, compare their traces.
    test "$trace" = 0 || {
      echo "Comparing local (-) vs. remote (+) file access traces."
      # Use sed to normalize absolute paths.
      diff -u \
        <(sed -e "s|$project_root/||g" "$output.fsatrace") \
        <(sed -e "s|$remote_project_root/||g" "$output.remote-fsatrace")
    }
    status=1
  }
}

exit "$status"
