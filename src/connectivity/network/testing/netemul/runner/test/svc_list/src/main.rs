// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Context as _, Error};

use std::path::Path;

use fidl;
use fidl::endpoints::ProtocolMarker;
use fidl_fuchsia_netemul_environment::{
    EnvironmentOptions, LaunchService, LoggerOptions, ManagedEnvironmentMarker, VirtualDevice,
};
use fidl_fuchsia_netemul_network::{
    DeviceProxy_Marker, EndpointBacking, EndpointConfig, EndpointManagerMarker,
    NetworkContextMarker,
};
use fidl_fuchsia_netemul_sync::SyncManagerMarker;
use fidl_fuchsia_netstack::NetstackMarker;
use fidl_fuchsia_sys::{
    ComponentControllerEvent, ComponentControllerEventStream, ComponentControllerMarker,
    LaunchInfo, LauncherMarker, TerminationReason,
};

use fuchsia_async as fasync;
use fuchsia_component::client;
use futures::TryStreamExt;
use structopt::StructOpt;

const EP_NAME: &str = "ep0";
const EP_MOUNT: &str = "class/ethernet/ep0";
const MY_PACKAGE: &str = "fuchsia-pkg://fuchsia.com/netemul-sandbox-test#meta/svc-list.cmx";
const NETSTACK_URL: &str = "fuchsia-pkg://fuchsia.com/netemul-sandbox-test#meta/netstack.cmx";
const FAKE_SVC_NAME: &str = "fuchsia.some.fake.Service";
const FAKE_SVC_URL: &str = "fuchsia-pkg://fuchsia.com/fake#meta/fake.cmx";

async fn wait_for_component(
    component_events: &mut ComponentControllerEventStream,
) -> Result<(), Error> {
    // wait for child to exit and mimic the result code
    let result = loop {
        let event = component_events
            .try_next()
            .await
            .context("wait for child component to exit")?
            .ok_or_else(|| format_err!("Child didn't exit cleanly"))?;

        match event {
            ComponentControllerEvent::OnTerminated {
                return_code: code,
                termination_reason: reason,
            } => {
                println!("Child exited with code {}, reason {}", code, reason as u32);
                if code != 0 || reason != TerminationReason::Exited {
                    break Err(format_err!(
                        "Child exited with code {}, reason {}",
                        code,
                        reason as u32
                    ));
                } else {
                    break Ok(());
                }
            }
            _ => {
                continue;
            }
        }
    };
    result
}

// this is the main body of our test, which
// runs in an executor
async fn run_test() -> Result<(), Error> {
    // connect to NetworkContext and ManagedEnvironment services
    let netctx = client::connect_to_protocol::<NetworkContextMarker>()?;
    let env = client::connect_to_protocol::<ManagedEnvironmentMarker>()?;

    // get the endpoint manager
    let (epm, epmch) = fidl::endpoints::create_proxy::<EndpointManagerMarker>()?;
    netctx.get_endpoint_manager(epmch)?;

    let mut cfg = EndpointConfig { backing: EndpointBacking::Ethertap, mac: None, mtu: 1500 };

    // create a network endpoint
    let (_, ep) = epm.create_endpoint(EP_NAME, &mut cfg).await?;
    let ep = ep.unwrap().into_proxy()?;

    // get the endpoint proxy to pass to child environment
    let (ep_proxy_client, ep_proxy_server) =
        fidl::endpoints::create_endpoints::<DeviceProxy_Marker>()?;
    ep.get_proxy_(ep_proxy_server)?;

    // prepare a child managed environment
    let (child_env, child_env_server) =
        fidl::endpoints::create_proxy::<ManagedEnvironmentMarker>()?;

    let env_options = EnvironmentOptions {
        name: Some(String::from("child_env")),
        services: Some(vec![LaunchService {
            name: String::from(NetstackMarker::NAME),
            url: String::from(NETSTACK_URL),
            arguments: Vec::new(),
        }]),
        // pass the endpoint's proxy to create a virtual device
        devices: Some(vec![VirtualDevice {
            path: String::from(EP_MOUNT),
            device: ep_proxy_client,
        }]),
        inherit_parent_launch_services: Some(false),
        logger_options: Some(LoggerOptions {
            enabled: Some(true),
            klogs_enabled: Some(false),
            filter_options: None,
            syslog_output: None,
            ..LoggerOptions::EMPTY
        }),
        ..EnvironmentOptions::EMPTY
    };
    // launch the child env
    env.create_child_environment(child_env_server, env_options)?;

    // launch as a process in the created environment.
    let (launcher, launcher_req) = fidl::endpoints::create_proxy::<LauncherMarker>()?;
    child_env.get_launcher(launcher_req)?;

    // launch info is our own package
    // plus the command line argument to run the child proc
    let mut linfo = LaunchInfo {
        url: String::from(MY_PACKAGE),
        arguments: Some(vec![String::from("-c")]),
        additional_services: None,
        directory_request: None,
        err: None,
        out: None,
        flat_namespace: None,
    };

    let (comp_controller, comp_controller_req) =
        fidl::endpoints::create_proxy::<ComponentControllerMarker>()?;
    let mut component_events = comp_controller.take_event_stream();
    launcher.create_component(&mut linfo, Some(comp_controller_req))?;

    wait_for_component(&mut component_events).await
}

fn check_service(service: &str) -> Result<(), Error> {
    let fs_path = format!("/svc/{}", service);
    let p = Path::new(&fs_path);
    if p.exists() {
        Ok(())
    } else {
        Err(format_err!("Service {} does not exist", service))
    }
}

fn check_virtual_device(vdev: &str) -> Result<(), Error> {
    let fs_path = &format!("/vdev/{}", vdev);
    let p = Path::new(fs_path);
    if p.exists() {
        Ok(())
    } else {
        Err(format_err!("Virtual device {} does not exist", vdev))
    }
}

async fn launch_grandchild() -> Result<(), Error> {
    let env = client::connect_to_protocol::<ManagedEnvironmentMarker>()?;

    let env_options = EnvironmentOptions {
        name: Some(String::from("grandchild_env")),
        // add some arbitrary service to the grandchild environment
        services: Some(vec![LaunchService {
            name: String::from(FAKE_SVC_NAME),
            url: String::from(FAKE_SVC_URL),
            arguments: Vec::new(),
        }]),
        devices: None,
        // inherit parent configuration to check if netstack flows through
        // this won't be the same netstack *instance*, though. But it should be
        // launched with the same url as the "child" environment
        inherit_parent_launch_services: Some(true),
        logger_options: Some(LoggerOptions {
            enabled: Some(true),
            klogs_enabled: Some(false),
            filter_options: None,
            syslog_output: None,
            ..LoggerOptions::EMPTY
        }),
        ..EnvironmentOptions::EMPTY
    };

    let (child_env, child_env_server) =
        fidl::endpoints::create_proxy::<ManagedEnvironmentMarker>()?;

    // launch the grandchild env
    env.create_child_environment(child_env_server, env_options)?;

    // launch info is our own package
    // plus the command line argument to run the grandchild proc
    let mut linfo = LaunchInfo {
        url: String::from(MY_PACKAGE),
        arguments: Some(vec![String::from("-g")]),
        additional_services: None,
        directory_request: None,
        err: None,
        out: None,
        flat_namespace: None,
    };

    // launch myself as a process in the created environment.
    let (launcher, launcher_req) = fidl::endpoints::create_proxy::<LauncherMarker>()?;
    child_env.get_launcher(launcher_req)?;

    let (comp_controller, comp_controller_req) =
        fidl::endpoints::create_proxy::<ComponentControllerMarker>()?;
    let mut component_events = comp_controller.take_event_stream();
    launcher.create_component(&mut linfo, Some(comp_controller_req))?;

    wait_for_component(&mut component_events).await
}

fn main() -> Result<(), Error> {
    // Make sure all services exist.
    let () = check_service(NetworkContextMarker::NAME)?;
    let () = check_service(ManagedEnvironmentMarker::NAME)?;
    let () = check_service(SyncManagerMarker::NAME)?;
    let () = check_service(fidl_fuchsia_net_tun::ControlMarker::NAME)?;

    #[derive(StructOpt, Debug)]
    struct Opt {
        #[structopt(short = "c")]
        is_child: bool,
        #[structopt(short = "g")]
        is_grandchild: bool,
    }

    let opt = Opt::from_args();
    // the same binary is used for the root test
    // and the test in child envs
    // a flag is passed on the command line to change
    // the code path
    if opt.is_child {
        let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;
        println!("Running as child");
        // check that the virtual ethernet device is there
        check_virtual_device(EP_MOUNT)?;
        // check that netstack was served
        check_service(NetstackMarker::NAME)?;
        // launch grandchild service to test environment inheritance
        executor.run_singlethreaded(launch_grandchild())
    } else if opt.is_grandchild {
        println!("Running as grandchild");
        // assert that netstack was served (should be present due to inheritance)
        check_service(NetstackMarker::NAME)?;
        // and the fake service:
        check_service(FAKE_SVC_NAME)?;
        Ok(())
    } else {
        let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;
        executor.run_singlethreaded(run_test())
    }
}
