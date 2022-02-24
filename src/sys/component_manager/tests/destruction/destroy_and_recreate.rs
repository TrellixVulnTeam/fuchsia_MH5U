// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::endpoints::{self, Proxy},
    fidl_fidl_test_components as ftest, fidl_fuchsia_component as fcomponent,
    fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy, MODE_TYPE_SERVICE},
    fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_zircon as zx,
    io_util::{self, OPEN_RIGHT_READABLE},
    std::path::PathBuf,
    tracing::*,
};

#[fuchsia::component]
async fn main() {
    info!("Started collection realm");
    let realm = client::connect_to_protocol::<fcomponent::RealmMarker>()
        .expect("could not connect to Realm service");

    // Create a "trigger realm" child component.
    info!("Creating child");
    {
        let mut collection_ref = fdecl::CollectionRef { name: "coll".to_string() };
        let child_decl = fdecl::Child {
            name: Some("trigger".to_string()),
            url: Some(
                "fuchsia-pkg://fuchsia.com/destruction_integration_test#meta/trigger_realm.cm"
                    .to_string(),
            ),
            startup: Some(fdecl::StartupMode::Lazy),
            environment: None,
            ..fdecl::Child::EMPTY
        };
        realm
            .create_child(&mut collection_ref, child_decl, fcomponent::CreateChildArgs::EMPTY)
            .await
            .expect(&format!("create_child failed"))
            .expect(&format!("failed to create child"));
    }

    // Bind to child, causing it to start (along with its eager children).
    info!("Binding to child");
    {
        let mut child_ref =
            fdecl::ChildRef { name: "trigger".to_string(), collection: Some("coll".to_string()) };
        let (dir, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
        realm
            .open_exposed_dir(&mut child_ref, server_end)
            .await
            .expect(&format!("open_exposed_dir failed"))
            .expect(&format!("failed to open child exposed dir"));
        let trigger = open_trigger_svc(&dir).expect("failed to open trigger service");
        trigger.run().await.expect("trigger failed");
    }

    // Destroy the child.
    info!("Destroying and recreating child");
    {
        let mut child_ref =
            fdecl::ChildRef { name: "trigger".to_string(), collection: Some("coll".to_string()) };
        realm
            .destroy_child(&mut child_ref)
            .await
            .expect("destroy_child failed")
            .expect("failed to destroy child");
    }

    // Recreate the child immediately.
    {
        let mut collection_ref = fdecl::CollectionRef { name: "coll".to_string() };
        let child_decl = fdecl::Child {
            name: Some("trigger".to_string()),
            url: Some(
                "fuchsia-pkg://fuchsia.com/destruction_integration_test#meta/trigger_realm.cm"
                    .to_string(),
            ),
            startup: Some(fdecl::StartupMode::Lazy),
            environment: None,
            ..fdecl::Child::EMPTY
        };
        realm
            .create_child(&mut collection_ref, child_decl, fcomponent::CreateChildArgs::EMPTY)
            .await
            .expect(&format!("create_child failed"))
            .expect(&format!("failed to create child"));
    }

    // Sleep so that it's more likely that, if the new component is destroyed incorrectly,
    // this test will fail.
    fasync::Timer::new(fasync::Time::after(zx::Duration::from_seconds(5))).await;

    // Restart the child.
    info!("Restarting to child");
    {
        let mut child_ref =
            fdecl::ChildRef { name: "trigger".to_string(), collection: Some("coll".to_string()) };
        let (dir, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
        realm
            .open_exposed_dir(&mut child_ref, server_end)
            .await
            .expect(&format!("open_exposed_dir failed"))
            .expect(&format!("failed to open child exposed dir"));
        let trigger = open_trigger_svc(&dir).expect("failed to open trigger service");
        trigger.run().await.expect("trigger failed");
    }
}

fn open_trigger_svc(dir: &DirectoryProxy) -> Result<ftest::TriggerProxy, Error> {
    let node_proxy = io_util::open_node(
        dir,
        &PathBuf::from("fidl.test.components.Trigger"),
        OPEN_RIGHT_READABLE,
        MODE_TYPE_SERVICE,
    )
    .context("failed to open trigger service")?;
    Ok(ftest::TriggerProxy::new(node_proxy.into_channel().unwrap()))
}
