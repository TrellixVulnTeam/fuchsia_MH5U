// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::framework::RealmCapabilityHost,
    crate::model::{
        component::{ComponentInstance, StartReason, WeakComponentInstance},
        error::ModelError,
        model::Model,
    },
    ::routing::error::ComponentInstanceError,
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_sys2 as fsys,
    futures::prelude::*,
    log::*,
    moniker::{
        AbsoluteMoniker, AbsoluteMonikerBase, MonikerError, RelativeMoniker, RelativeMonikerBase,
    },
    std::convert::TryFrom,
    std::sync::{Arc, Weak},
};

#[derive(Clone)]
pub struct LifecycleController {
    model: Weak<Model>,
    prefix: AbsoluteMoniker,
}

impl LifecycleController {
    pub fn new(model: Weak<Model>, prefix: AbsoluteMoniker) -> Self {
        Self { model, prefix }
    }

    fn construct_moniker(&self, input: &str) -> Result<AbsoluteMoniker, fcomponent::Error> {
        let relative_moniker = RelativeMoniker::try_from(input).map_err(|e: MonikerError| {
            debug!("lifecycle controller received invalid component moniker: {}", e);
            fcomponent::Error::InvalidArguments
        })?;
        if !relative_moniker.up_path().is_empty() {
            debug!(
                "lifecycle controller received moniker that attempted to reach outside its scope"
            );
            return Err(fcomponent::Error::InvalidArguments);
        }
        let abs_moniker = AbsoluteMoniker::from_relative(&self.prefix, &relative_moniker).map_err(
            |e: MonikerError| {
                debug!("lifecycle controller received invalid component moniker: {}", e);
                fcomponent::Error::InvalidArguments
            },
        )?;

        Ok(abs_moniker)
    }

    async fn resolve_component(
        &self,
        moniker: &str,
    ) -> Result<Arc<ComponentInstance>, fcomponent::Error> {
        let abs_moniker = self.construct_moniker(moniker)?;
        let model = self.model.upgrade().ok_or(fcomponent::Error::Internal)?;

        model.look_up(&abs_moniker).await.map_err(|e| match e {
            e @ ModelError::ResolverError { .. } | e @ ModelError::ComponentInstanceError {
                err: ComponentInstanceError::ResolveFailed { .. }
            } => {
                debug!(
                    "lifecycle controller failed to resolve component instance {}: {:?}",
                    abs_moniker,
                    e
                );
                fcomponent::Error::InstanceCannotResolve
            }
            e @ ModelError::ComponentInstanceError {
                err: ComponentInstanceError::InstanceNotFound { .. },
            } => {
                debug!(
                    "lifecycle controller was asked to perform an operation on a component instance that doesn't exist {}: {:?}",
                    abs_moniker,
                    e,
                );
                fcomponent::Error::InstanceNotFound
            }
            e => {
                error!(
                    "unexpected error encountered by lifecycle controller while looking up component {}: {:?}",
                    abs_moniker,
                    e,
                );
                fcomponent::Error::Internal
            }
        })
    }

    async fn resolve(&self, moniker: String) -> Result<(), fcomponent::Error> {
        self.resolve_component(&moniker).await?;
        Ok(())
    }

    async fn start(&self, moniker: String) -> Result<fsys::StartResult, fcomponent::Error> {
        let component = self.resolve_component(&moniker).await?;
        let res = component.start(&StartReason::Debug).await.map_err(|e: ModelError| {
            debug!(
                "lifecycle controller failed to start the component instance {}: {:?}",
                moniker, e
            );
            fcomponent::Error::InstanceCannotStart
        })?;
        Ok(res)
    }

    async fn stop(&self, moniker: String, is_recursive: bool) -> Result<(), fcomponent::Error> {
        log::info!("Stopping {}", moniker);
        let component = self.resolve_component(&moniker).await?;
        log::info!("{} Resolved!", moniker);
        component.stop_instance(false, is_recursive).await.map_err(|e: ModelError| {
            debug!(
                "lifecycle controller failed to stop component instance {} (is_recursive={}): {:?}",
                moniker, is_recursive, e
            );
            fcomponent::Error::Internal
        })
    }

    async fn create_child(
        &self,
        parent_moniker: String,
        collection: fdecl::CollectionRef,
        child_decl: fdecl::Child,
        child_args: fcomponent::CreateChildArgs,
    ) -> Result<(), fcomponent::Error> {
        let parent_component = self.resolve_component(&parent_moniker).await?;
        let parent_component = WeakComponentInstance::new(&parent_component);
        RealmCapabilityHost::create_child(&parent_component, collection, child_decl, child_args)
            .await
    }

    async fn destroy_child(
        &self,
        parent_moniker: String,
        child: fdecl::ChildRef,
    ) -> Result<(), fcomponent::Error> {
        let parent_component = self.resolve_component(&parent_moniker).await?;
        let parent_component = WeakComponentInstance::new(&parent_component);
        RealmCapabilityHost::destroy_child(&parent_component, child).await
    }

    pub async fn serve(&self, mut stream: fsys::LifecycleControllerRequestStream) {
        while let Ok(Some(operation)) = stream.try_next().await {
            match operation {
                fsys::LifecycleControllerRequest::Resolve { moniker, responder } => {
                    let mut res = self.resolve(moniker).await;
                    responder
                        .send(&mut res)
                        .unwrap_or_else(|e| warn!("response send failed: {}", e));
                }
                fsys::LifecycleControllerRequest::Start { moniker, responder } => {
                    let mut res = self.start(moniker).await;
                    responder
                        .send(&mut res)
                        .unwrap_or_else(|e| warn!("response send failed: {}", e));
                }
                fsys::LifecycleControllerRequest::Stop { moniker, responder, is_recursive } => {
                    let mut res = self.stop(moniker, is_recursive).await;
                    responder
                        .send(&mut res)
                        .unwrap_or_else(|e| warn!("response send failed: {}", e));
                }
                fsys::LifecycleControllerRequest::CreateChild {
                    parent_moniker,
                    collection,
                    decl,
                    args,
                    responder,
                } => {
                    let mut res = self.create_child(parent_moniker, collection, decl, args).await;
                    responder
                        .send(&mut res)
                        .unwrap_or_else(|e| warn!("response send failed: {}", e));
                }
                fsys::LifecycleControllerRequest::DestroyChild {
                    parent_moniker,
                    child,
                    responder,
                } => {
                    let mut res = self.destroy_child(parent_moniker, child).await;
                    responder
                        .send(&mut res)
                        .unwrap_or_else(|e| warn!("response send failed: {}", e));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::testing::test_helpers::{TestEnvironmentBuilder, TestModelResult},
        cm_rust_testing::ComponentDeclBuilder,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_component_decl as fdecl, fuchsia_async as fasync,
        std::sync::Arc,
    };

    #[fuchsia::test]
    async fn lifecycle_controller_test() {
        let components = vec![
            (
                "root",
                ComponentDeclBuilder::new()
                    .add_child(cm_rust::ChildDecl {
                        name: "a".to_string(),
                        url: "test:///a".to_string(),
                        startup: fdecl::StartupMode::Eager,
                        environment: None,
                        on_terminate: None,
                    })
                    .add_child(cm_rust::ChildDecl {
                        name: "cant-resolve".to_string(),
                        url: "cant-resolve://cant-resolve".to_string(),
                        startup: fdecl::StartupMode::Eager,
                        environment: None,
                        on_terminate: None,
                    })
                    .build(),
            ),
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_child(cm_rust::ChildDecl {
                        name: "b".to_string(),
                        url: "test:///b".to_string(),
                        startup: fdecl::StartupMode::Eager,
                        environment: None,
                        on_terminate: None,
                    })
                    .build(),
            ),
            ("b", ComponentDeclBuilder::new().build()),
        ];

        let TestModelResult { model, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let lifecycle_controller = LifecycleController::new(Arc::downgrade(&model), vec![].into());

        let (lifecycle_proxy, lifecycle_request_stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();

        // async move {} is used here because we want this to own the lifecycle_controller
        let _lifecycle_server_task = fasync::Task::local(async move {
            lifecycle_controller.serve(lifecycle_request_stream).await
        });

        assert_eq!(lifecycle_proxy.resolve(".").await.unwrap(), Ok(()));

        assert_eq!(lifecycle_proxy.resolve("./a").await.unwrap(), Ok(()));

        assert_eq!(
            lifecycle_proxy.resolve(".\\scope-escape-attempt").await.unwrap(),
            Err(fcomponent::Error::InvalidArguments)
        );

        assert_eq!(
            lifecycle_proxy.resolve("./doesnt-exist").await.unwrap(),
            Err(fcomponent::Error::InstanceNotFound)
        );

        assert_eq!(
            lifecycle_proxy.resolve("./cant-resolve").await.unwrap(),
            Err(fcomponent::Error::InstanceCannotResolve)
        );
    }

    #[fuchsia::test]
    async fn lifecycle_already_started_test() {
        let components = vec![("root", ComponentDeclBuilder::new().build())];

        let test_model_result =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let lifecycle_controller =
            LifecycleController::new(Arc::downgrade(&test_model_result.model), vec![].into());

        let (lifecycle_proxy, lifecycle_request_stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();

        // async move {} is used here because we want this to own the lifecycle_controller
        let _lifecycle_server_task = fasync::Task::local(async move {
            lifecycle_controller.serve(lifecycle_request_stream).await
        });

        assert_eq!(lifecycle_proxy.start(".").await.unwrap(), Ok(fsys::StartResult::Started));

        assert_eq!(
            lifecycle_proxy.start(".").await.unwrap(),
            Ok(fsys::StartResult::AlreadyStarted)
        );
    }
}
