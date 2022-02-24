// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    errors::{ffx_bail, ffx_error},
    ffx_component::connect_to_lifecycle_controller,
    ffx_component_start_args::ComponentStartCommand,
    ffx_core::ffx_plugin,
    fidl_fuchsia_developer_remotecontrol as rc, fidl_fuchsia_sys2 as fsys,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
};

#[ffx_plugin()]
pub async fn start(rcs_proxy: rc::RemoteControlProxy, cmd: ComponentStartCommand) -> Result<()> {
    let lifecycle_controller = connect_to_lifecycle_controller(&rcs_proxy).await?;
    start_impl(lifecycle_controller, cmd.moniker, &mut std::io::stdout()).await?;
    Ok(())
}

async fn start_impl<W: std::io::Write>(
    lifecycle_controller: fsys::LifecycleControllerProxy,
    moniker: String,
    writer: &mut W,
) -> Result<fsys::StartResult> {
    let moniker = AbsoluteMoniker::parse_str(&moniker)
        .map_err(|e| ffx_error!("Moniker could not be parsed: {}", e))?;
    writeln!(writer, "Moniker: {}", moniker)?;

    // LifecycleController accepts RelativeMonikers only
    let moniker = format!(".{}", moniker.to_string());
    let res = lifecycle_controller.start(&moniker).await;
    match res {
        Ok(sr) => match sr {
            Ok(fsys::StartResult::Started) => {
                writeln!(writer, "Component started.")?;
                Ok(fsys::StartResult::Started)
            }
            Ok(fsys::StartResult::AlreadyStarted) => {
                writeln!(writer, "Component is already running.")?;
                Ok(fsys::StartResult::AlreadyStarted)
            }
            Err(e) => {
                ffx_bail!("Lifecycle protocol could not start the component instance: {:?}", e)
            }
        },
        Err(e) => {
            ffx_bail!("FIDL error: {:?}", e)
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        super::*, fidl::endpoints::create_proxy_and_stream, futures::TryStreamExt,
        std::io::BufWriter,
    };

    fn setup_fake_lifecycle_controller(
        expected_moniker: &'static str,
        is_running: bool,
    ) -> fsys::LifecycleControllerProxy {
        let (lifecycle_controller, mut stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::Start { moniker, responder, .. } => {
                    assert_eq!(expected_moniker, moniker);
                    let sr = if is_running {
                        fsys::StartResult::AlreadyStarted
                    } else {
                        fsys::StartResult::Started
                    };
                    responder.send(&mut Ok(sr)).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }
        })
        .detach();
        lifecycle_controller
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_success() -> Result<()> {
        let mut output = String::new();
        let mut writer = unsafe { BufWriter::new(output.as_mut_vec()) };
        let lifecycle_controller =
            setup_fake_lifecycle_controller("./core/ffx-laboratory:test", false);
        let response =
            start_impl(lifecycle_controller, "/core/ffx-laboratory:test".to_string(), &mut writer)
                .await;
        assert_eq!(response.unwrap(), fsys::StartResult::Started);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_already_started() -> Result<()> {
        let mut output = String::new();
        let mut writer = unsafe { BufWriter::new(output.as_mut_vec()) };
        let lifecycle_controller =
            setup_fake_lifecycle_controller("./core/ffx-laboratory:test", true);
        let response =
            start_impl(lifecycle_controller, "/core/ffx-laboratory:test".to_string(), &mut writer)
                .await;
        assert_eq!(response.unwrap(), fsys::StartResult::AlreadyStarted);
        Ok(())
    }
}
