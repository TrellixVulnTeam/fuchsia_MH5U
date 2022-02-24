// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"flag"
	"os"
	"os/signal"
	"syscall"

	"github.com/google/subcommands"

	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

var (
	colors   = color.ColorAuto
	logLevel = logger.DebugLevel
)

func init() {
	flag.Var(&colors, "color", "use color in output, can be never, auto, always")
	flag.Var(&logLevel, "log-level", "output verbosity, can be fatal, error, warning, info, debug or trace")
}

func main() {
	subcommands.Register(subcommands.HelpCommand(), "")
	subcommands.Register(subcommands.CommandsCommand(), "")
	subcommands.Register(subcommands.FlagsCommand(), "")
	subcommands.Register(&SetCommand{}, "")
	subcommands.Register(&BuildCommand{}, "")

	flag.Parse()

	l := logger.NewLogger(logLevel, color.NewColor(colors), os.Stdout, os.Stderr, "fint ")
	l.SetFlags(logger.Ltime | logger.Lmicroseconds | logger.Lshortfile)
	ctx := logger.WithLogger(context.Background(), l)
	ctx, cancel := signal.NotifyContext(ctx, syscall.SIGTERM, syscall.SIGINT)
	defer cancel()
	os.Exit(int(subcommands.Execute(ctx)))
}
