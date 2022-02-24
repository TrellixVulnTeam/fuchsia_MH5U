// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::scrutiny::Scrutiny,
    anyhow::Result,
    scrutiny_config::Config,
    scrutiny_plugins::{
        core::CorePlugin, devmgr_config::DevmgrConfigPlugin, engine::EnginePlugin,
        search::SearchPlugin, static_pkgs::StaticPkgsPlugin, sys::SysRealmPlugin,
        toolkit::ToolkitPlugin, verify::VerifyPlugin,
    },
};

/// Launches scrutiny from a configuration file. This is intended to be used by binaries that
/// want to launch custom configurations of the Scrutiny framework with select features enabled.
pub fn launch_from_config(config: Config) -> Result<String> {
    let mut scrutiny = Scrutiny::new(config)?;
    scrutiny.plugin(DevmgrConfigPlugin::new())?;
    scrutiny.plugin(StaticPkgsPlugin::new())?;
    scrutiny.plugin(CorePlugin::new())?;
    scrutiny.plugin(SearchPlugin::new())?;
    scrutiny.plugin(EnginePlugin::new(
        scrutiny.scheduler(),
        scrutiny.dispatcher(),
        scrutiny.plugin_manager(),
    ))?;
    scrutiny.plugin(ToolkitPlugin::new())?;
    scrutiny.plugin(VerifyPlugin::new())?;
    scrutiny.plugin(SysRealmPlugin::new())?;
    scrutiny.run()
}

/// Provides a default launcher for the Scrutiny frontend. This is intended to
/// be used by binaries that wish to launch a full copy of the Scrutiny
/// framework with default settings.
pub fn launch() -> Result<String> {
    launch_from_config(Scrutiny::args_from_env()?)
}

/// Provides a utility launcher for the Scruity frontend. This is intended to
/// be used by consumer libraries that simply want to launch the framework to
/// run a single command.
pub fn run_command(command: String) -> Result<String> {
    launch_from_config(Config::run_command(command))
}

/// Provides a utility launcher for the Scrutiny frontend. This is inteded to
/// be used by consumer libraries that simply want to launch the framework to
/// run a Scrutiny script.
pub fn run_script(script_path: String) -> Result<String> {
    launch_from_config(Config::run_script(script_path))
}
