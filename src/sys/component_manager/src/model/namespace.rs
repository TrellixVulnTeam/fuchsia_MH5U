// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        constants::PKG_PATH,
        model::{
            component::{ComponentInstance, Package, Runtime, StartReason, WeakComponentInstance},
            error::ModelError,
            routing::{
                self, route_and_open_capability, OpenDirectoryOptions, OpenOptions,
                OpenProtocolOptions, OpenServiceOptions, OpenStorageOptions,
            },
        },
    },
    ::routing::{
        capability_source::ComponentCapability, component_instance::ComponentInstanceInterface,
        rights::Rights, route_to_storage_decl, verify_instance_in_component_id_index, RouteRequest,
    },
    cm_logger::{
        fmt::{FmtArgsLogger, LOGGER as MODEL_LOGGER},
        scoped::ScopedLogger,
    },
    cm_rust::{self, CapabilityPath, ComponentDecl, UseDecl, UseProtocolDecl},
    fidl::endpoints::{create_endpoints, ClientEnd, ProtocolMarker, Proxy, ServerEnd},
    fidl_fuchsia_component_runner as fcrunner,
    fidl_fuchsia_io::{self as fio, DirectoryMarker, DirectoryProxy, NodeMarker},
    fidl_fuchsia_logger::LogSinkMarker,
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::future::{AbortHandle, Abortable, BoxFuture},
    log::*,
    std::{collections::HashMap, sync::Arc},
    vfs::{
        directory::entry::DirectoryEntry, directory::helper::DirectlyMutable,
        directory::immutable::simple as pfs, execution_scope::ExecutionScope, path::Path,
        remote::remote,
    },
};

type Directory = Arc<pfs::Simple>;

pub struct IncomingNamespace {
    pub package_dir: Option<DirectoryProxy>,
    dir_abort_handles: Vec<AbortHandle>,
    logger: Option<ScopedLogger>,
}

impl Drop for IncomingNamespace {
    fn drop(&mut self) {
        for abort_handle in &self.dir_abort_handles {
            abort_handle.abort();
        }
    }
}

impl IncomingNamespace {
    pub fn new(package: Option<Package>) -> Result<Self, ModelError> {
        let package_dir = package.map(|p| p.package_dir);
        Ok(Self { package_dir, dir_abort_handles: vec![], logger: None })
    }

    /// Returns a Logger whose output is attributed to this component's
    /// namespace.
    pub fn get_attributed_logger(&self) -> Option<&ScopedLogger> {
        self.logger.as_ref()
    }

    /// Get a logger, an attributed logger is returned if available, otherwise
    /// a default logger whose output belongs to component manager is returned.
    pub fn get_logger(&self) -> &dyn FmtArgsLogger {
        if let Some(attr_logger) = self.logger.as_ref() {
            attr_logger
        } else {
            // MODEL_LOGGER is a lazy_static, which obscures the type, Deref to
            // see through to the type, a reference to which implements
            // FmtArgsLogger
            &*MODEL_LOGGER
        }
    }

    /// In addition to populating a Vec<fcrunner::ComponentNamespaceEntry>, `populate` will start
    /// serving and install handles to pseudo directories.
    pub async fn populate<'a>(
        &'a mut self,
        component: WeakComponentInstance,
        decl: &'a ComponentDecl,
    ) -> Result<Vec<fcrunner::ComponentNamespaceEntry>, ModelError> {
        let mut ns: Vec<fcrunner::ComponentNamespaceEntry> = vec![];

        // Populate the /pkg namespace.
        if let Some(package_dir) = &self.package_dir {
            Self::add_pkg_directory(&mut ns, package_dir)?;
        }

        // Populate the namespace from uses, using the component manager's namespace.
        // svc_dirs will hold (path,directory) pairs. Each pair holds a path in the
        // component's namespace and a directory that ComponentMgr will host for the component.
        let mut svc_dirs = HashMap::new();

        // directory_waiters will hold Future<Output=()> objects that will wait for activity on
        // a channel and then route the channel to the appropriate component's out directory.
        let mut directory_waiters = Vec::new();

        let mut log_sink_decl: Option<UseProtocolDecl> = None;
        for use_ in &decl.uses {
            match use_ {
                cm_rust::UseDecl::Directory(_) => {
                    Self::add_directory_use(
                        &mut ns,
                        &mut directory_waiters,
                        use_,
                        component.clone(),
                    )?;
                }
                cm_rust::UseDecl::Protocol(s) => {
                    Self::add_service_or_protocol_use(
                        &mut svc_dirs,
                        UseDecl::Protocol(s.clone()),
                        &s.target_path,
                        component.clone(),
                    )?;
                    if s.source_name.0 == LogSinkMarker::NAME {
                        log_sink_decl = Some(s.clone());
                    }
                }
                cm_rust::UseDecl::Service(s) => {
                    Self::add_service_or_protocol_use(
                        &mut svc_dirs,
                        UseDecl::Service(s.clone()),
                        &s.target_path,
                        component.clone(),
                    )?;
                }
                cm_rust::UseDecl::Storage(_) => {
                    Self::add_storage_use(&mut ns, &mut directory_waiters, use_, component.clone())
                        .await?;
                }
                cm_rust::UseDecl::Event(_) | cm_rust::UseDecl::EventStreamDeprecated(_) => {
                    // Event capabilities are handled in model::model,
                    // as these are capabilities used by the framework itself
                    // and not given to components directly.
                }
                cm_rust::UseDecl::EventStream(_) => {
                    // TODO(fxbug.dev/81980): install EventStream at the requested path.
                }
            }
        }

        // Start hosting the services directories and add them to the namespace
        self.serve_and_install_svc_dirs(&mut ns, svc_dirs)?;
        self.start_directory_waiters(directory_waiters)?;

        if let Some(log_decl) = &log_sink_decl {
            let (ns_, logger) = self.get_logger_from_ns(ns, log_decl).await;
            ns = ns_;
            self.logger = logger;
        }

        Ok(ns)
    }

    /// Given the set of namespace entries and a LogSink protocol's
    /// `UseProtocolDecl`, look through the namespace for where to connect
    /// to the LogSink protocol. The log connection, if any, is stored in the
    /// IncomingNamespace.
    async fn get_logger_from_ns(
        &self,
        ns: Vec<fcrunner::ComponentNamespaceEntry>,
        log_sink_decl: &UseProtocolDecl,
    ) -> (Vec<fcrunner::ComponentNamespaceEntry>, Option<ScopedLogger>) {
        // A new set of namespace entries is returned because when the entry
        // used to connect to LogSink is found, that entry is consumed. A
        // matching entry is created and placed in the set of entries returned
        // by this function. `self` is taken as mutable so the
        // logger connection can be stored when found.
        let mut new_ns = vec![];
        let mut log_ns_dir: Option<(fcrunner::ComponentNamespaceEntry, String)> = None;
        let mut logger = Option::<ScopedLogger>::None;
        // Find namespace directory specified in the log_sink_decl
        for ns_dir in ns {
            if let Some(path) = &ns_dir.path.clone() {
                // Check if this namespace path is a stem of the decl's path
                if log_ns_dir.is_none() {
                    if let Ok(path_remainder) =
                        Self::is_subpath_of(log_sink_decl.target_path.to_string(), path.to_string())
                    {
                        log_ns_dir = Some((ns_dir, path_remainder));
                        continue;
                    }
                }
            }
            new_ns.push(ns_dir);
        }

        // If we found a matching namespace entry, try to open the log proxy
        if let Some((mut entry, remaining_path)) = log_ns_dir {
            if let Some(dir) = entry.directory {
                let _str = log_sink_decl.target_path.to_string();
                let (restored_dir, logger_) = get_logger_from_dir(dir, remaining_path).await;
                entry.directory = restored_dir;
                logger = logger_;
            }
            new_ns.push(entry);
        }
        (new_ns, logger)
    }

    /// add_pkg_directory will add a handle to the component's package under /pkg in the namespace.
    fn add_pkg_directory(
        ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
        package_dir: &DirectoryProxy,
    ) -> Result<(), ModelError> {
        let clone_dir_proxy = io_util::clone_directory(package_dir, fio::CLONE_FLAG_SAME_RIGHTS)
            .map_err(|e| ModelError::namespace_creation_failed(e))?;
        let cloned_dir = ClientEnd::new(
            clone_dir_proxy
                .into_channel()
                .expect("could not convert directory to channel")
                .into_zx_channel(),
        );
        ns.push(fcrunner::ComponentNamespaceEntry {
            path: Some(PKG_PATH.to_str().unwrap().to_string()),
            directory: Some(cloned_dir),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        });
        Ok(())
    }

    /// Adds a directory waiter to `waiters` and updates `ns` to contain a handle for the
    /// directory described by `use_`. Once the channel is readable, the future calls
    /// `route_directory` to forward the channel to the source component's outgoing directory and
    /// terminates.
    fn add_directory_use(
        ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
        waiters: &mut Vec<BoxFuture<()>>,
        use_: &UseDecl,
        component: WeakComponentInstance,
    ) -> Result<(), ModelError> {
        Self::add_directory_helper(ns, waiters, use_, component)
    }

    /// Adds a directory waiter to `waiters` and updates `ns` to contain a handle for the
    /// storage described by `use_`. Once the channel is readable, the future calls
    /// `route_storage` to forward the channel to the source component's outgoing directory and
    /// terminates.
    async fn add_storage_use(
        ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
        waiters: &mut Vec<BoxFuture<'_, ()>>,
        use_: &UseDecl,
        component: WeakComponentInstance,
    ) -> Result<(), ModelError> {
        // Prevent component from using storage capability if it is restricted to the component ID
        // index, and the component isn't in the index.
        match use_ {
            UseDecl::Storage(use_storage_decl) => {
                // To check that the storage capability is restricted to the storage decl, we have
                // to resolve the storage source capability. Because storage capabilites are only
                // ever `offer`d down the component tree, and we always resolve parents before
                // children, this resolution will walk the cache-happy path.
                // TODO(dgonyeo): Eventually combine this logic with the general-purpose startup
                // capability check.
                let instance = component.upgrade()?;
                let mut noop_mapper = ComponentInstance::new_route_mapper();
                if let Ok(source) =
                    route_to_storage_decl(use_storage_decl.clone(), &instance, &mut noop_mapper)
                        .await
                {
                    verify_instance_in_component_id_index(&source, &instance)?;
                }
            }
            _ => unreachable!("unexpected storage decl"),
        }

        Self::add_directory_helper(ns, waiters, use_, component)
    }

    fn add_directory_helper(
        ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
        waiters: &mut Vec<BoxFuture<()>>,
        use_: &UseDecl,
        component: WeakComponentInstance,
    ) -> Result<(), ModelError> {
        let target_path =
            use_.path().expect("use decl without path used in add_directory_helper").to_string();
        let flags = match use_ {
            UseDecl::Directory(dir) => Rights::from(dir.rights).into_legacy(),
            UseDecl::Storage(_) => fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE,
            _ => panic!("not a directory or storage capability"),
        };
        let use_ = use_.clone();
        let (client_end, server_end) =
            create_endpoints().expect("could not create storage proxy endpoints");
        let route_on_usage = async move {
            // Wait for the channel to become readable.
            let server_end = fasync::Channel::from_channel(server_end.into_channel())
                .expect("failed to convert server_end into async channel");
            let on_signal_fut = fasync::OnSignals::new(&server_end, zx::Signals::CHANNEL_READABLE);
            on_signal_fut.await.unwrap();
            let target = match component.upgrade() {
                Ok(component) => component,
                Err(e) => {
                    error!(
                        "failed to upgrade WeakComponentInstance routing use \
                        decl `{:?}`: {:?}",
                        &use_, e
                    );
                    return;
                }
            };
            let mut server_end = server_end.into_zx_channel();
            let (route_request, open_options) = match &use_ {
                UseDecl::Directory(use_dir_decl) => (
                    RouteRequest::UseDirectory(use_dir_decl.clone()),
                    OpenOptions::Directory(OpenDirectoryOptions {
                        flags,
                        open_mode: fio::MODE_TYPE_DIRECTORY,
                        relative_path: String::new(),
                        server_chan: &mut server_end,
                    }),
                ),
                UseDecl::Storage(use_storage_decl) => (
                    RouteRequest::UseStorage(use_storage_decl.clone()),
                    // TODO(fxbug.dev/50716): This StartReason is wrong. We need to refactor the Storage
                    // capability to plumb through the correct StartReason.
                    OpenOptions::Storage(OpenStorageOptions {
                        open_mode: fio::MODE_TYPE_DIRECTORY,
                        server_chan: &mut server_end,
                        start_reason: StartReason::Eager,
                    }),
                ),
                _ => panic!("not a directory or storage capability"),
            };
            if let Err(e) = route_and_open_capability(route_request, &target, open_options).await {
                routing::report_routing_failure(
                    &target,
                    &ComponentCapability::Use(use_),
                    &e,
                    server_end,
                )
                .await;
            }
        };

        waiters.push(Box::pin(route_on_usage));
        ns.push(fcrunner::ComponentNamespaceEntry {
            path: Some(target_path.clone()),
            directory: Some(client_end),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        });
        Ok(())
    }

    /// start_directory_waiters will spawn the futures in directory_waiters as abortables, and adds
    /// the abort handles to the IncomingNamespace.
    fn start_directory_waiters(
        &mut self,
        directory_waiters: Vec<BoxFuture<'static, ()>>,
    ) -> Result<(), ModelError> {
        for waiter in directory_waiters {
            let (abort_handle, abort_registration) = AbortHandle::new_pair();
            self.dir_abort_handles.push(abort_handle);
            let future = Abortable::new(waiter, abort_registration);
            // The future for a directory waiter will only terminate once the directory channel is
            // first used, so we must start up a new task here to run the future instead of calling
            // await on it directly. This is wrapped in an async move {.await;}` block to drop
            // the unused return value.
            fasync::Task::spawn(async move {
                future.await.unwrap_or_else(|e| warn!("directory waiter aborted: {}", e));
            })
            .detach();
        }
        Ok(())
    }

    /// Adds a service broker in `svc_dirs` for service described by `use_`. The service will be
    /// proxied to the outgoing directory of the source component.
    fn add_service_or_protocol_use(
        svc_dirs: &mut HashMap<String, Directory>,
        use_: UseDecl,
        capability_path: &CapabilityPath,
        component: WeakComponentInstance,
    ) -> Result<(), ModelError> {
        let not_found_component_copy = component.clone();
        let route_open_fn = move |scope: ExecutionScope,
                                  flags: u32,
                                  mode: u32,
                                  relative_path: Path,
                                  server_end: ServerEnd<NodeMarker>| {
            let use_ = use_.clone();
            let component = component.clone();
            scope.spawn(async move {
                    let target = match component.upgrade() {
                        Ok(component) => component,
                        Err(e) => {
                            error!(
                                "failed to upgrade WeakComponentInstance routing use \
                                decl `{:?}`: {:?}",
                                &use_, e
                            );
                            return;
                        }
                    };
                    let mut server_end = server_end.into_channel();
                    let (route_request, open_options) = {
                        match &use_ {
                            UseDecl::Service(use_service_decl) => {
                                (RouteRequest::UseService(use_service_decl.clone()),
                                 OpenOptions::Service(
                                     OpenServiceOptions{
                                         flags,
                                         open_mode: mode,
                                         relative_path: relative_path.into_string(),
                                         server_chan: &mut server_end
                                     }
                                 ))
                            },
                            UseDecl::Protocol(use_protocol_decl) => {
                                (RouteRequest::UseProtocol(use_protocol_decl.clone()),
                                 OpenOptions::Protocol(
                                     OpenProtocolOptions{
                                         flags,
                                         open_mode: mode,
                                         relative_path: relative_path.into_string(),
                                         server_chan: &mut server_end
                                     }
                                 ))
                            },
                            _ => panic!("add_service_or_protocol_use called with non-service or protocol capability"),
                        }
                    };

                    let res = routing::route_and_open_capability(route_request, &target, open_options).await;
                    if let Err(e) = res {
                        routing::report_routing_failure(
                            &target,
                            &ComponentCapability::Use(use_),
                            &e,
                            server_end,
                        )
                        .await;
                    }
                });
        };

        let service_dir = svc_dirs.entry(capability_path.dirname.clone()).or_insert_with(|| {
            make_dir_with_not_found_logging(
                capability_path.dirname.clone(),
                not_found_component_copy,
            )
        });
        service_dir
            .clone()
            .add_entry(&capability_path.basename, remote(route_open_fn))
            .expect("could not add service to directory");
        Ok(())
    }

    /// serve_and_install_svc_dirs will take all of the pseudo directories collected in
    /// svc_dirs (as populated by add_service_use calls), start them and install them in the
    /// namespace. The abortable handles are saved in the IncomingNamespace, to
    /// be called when the IncomingNamespace is dropped.
    fn serve_and_install_svc_dirs(
        &mut self,
        ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
        svc_dirs: HashMap<String, Directory>,
    ) -> Result<(), ModelError> {
        for (target_dir_path, pseudo_dir) in svc_dirs {
            let (client_end, server_end) =
                create_endpoints::<NodeMarker>().expect("could not create node proxy endpoints");
            pseudo_dir.clone().open(
                ExecutionScope::new(),
                fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE,
                fio::MODE_TYPE_DIRECTORY,
                Path::dot(),
                server_end.into_channel().into(),
            );

            ns.push(fcrunner::ComponentNamespaceEntry {
                path: Some(target_dir_path.as_str().to_string()),
                directory: Some(ClientEnd::new(client_end.into_channel())), // coerce to ClientEnd<Dir>
                ..fcrunner::ComponentNamespaceEntry::EMPTY
            });
        }
        Ok(())
    }

    /// Determines if the `full` is a subpath of the `stem`. Returns the
    /// remaining portion of the path if `full` us a subpath. Returns Error if
    /// `stem` and `full` are the same.
    fn is_subpath_of(full: String, stem: String) -> Result<String, ()> {
        let stem_path = std::path::Path::new(&stem);
        let full_path = std::path::Path::new(&full);

        let remainder = full_path
            .strip_prefix(stem_path)
            // Unwrapping the `to_str` conversion should be safe here since we
            // started with a Unicode value, put it into a path and now are
            // extracting a portion of that value.
            .map(|path| path.to_str().unwrap().to_string())
            .map_err(|_| ())?;

        if remainder.is_empty() {
            Err(())
        } else {
            Ok(remainder)
        }
    }
}

/// Given a Directory, connect to the LogSink protocol at the default
/// location.
pub async fn get_logger_from_dir(
    dir: ClientEnd<DirectoryMarker>,
    at_path: String,
) -> (Option<ClientEnd<DirectoryMarker>>, Option<ScopedLogger>) {
    let mut logger = Option::<ScopedLogger>::None;
    match dir.into_proxy() {
        Ok(dir_proxy) => {
            match ScopedLogger::from_directory(&dir_proxy, at_path).await {
                Ok(ns_logger) => {
                    logger = Some(ns_logger);
                }
                Err(err) => {
                    log::info!("LogSink.Connect() failed, logs will be attributed to component manager: {}", err);
                }
            }

            // Now that we have the LogSink and socket, put the LogSink
            // protocol directory back where we found it.
            (
                dir_proxy.into_channel().map_or_else(
                    |e| {
                        log::error!(
                            "LogSink proxy could not be converted back to channel: {:?}",
                            e
                        );
                        None
                    },
                    |chan| Some(ClientEnd::<fidl_fuchsia_io::DirectoryMarker>::new(chan.into())),
                ),
                logger,
            )
        }
        Err(e) => {
            log::info!("Directory client channel could not be turned into proxy: {}", e);
            (None, logger)
        }
    }
}

fn make_dir_with_not_found_logging(
    root_path: String,
    component_for_logger: WeakComponentInstance,
) -> Arc<pfs::Simple> {
    let new_dir = pfs::simple();
    // Grab a copy of the directory path, it will be needed if we log a
    // failed open request.
    new_dir.clone().set_not_found_handler(Box::new(move |path| {
        // Clone the component pointer and pass the copy into the logger.
        let component_for_logger = component_for_logger.clone();
        let requested_path = format!("{}/{}", &root_path, path);

        // Spawn a task which logs the error. It would be nicer to not
        // spawn a task, but locking the component is async and this
        // closure is not.
        fasync::Task::spawn(async move {
            match component_for_logger.upgrade() {
                Ok(target) => {
                    let execution = target.lock_execution().await;
                    let logger = match &execution.runtime {
                        Some(Runtime { namespace: Some(ns), .. }) => ns.get_logger(),
                        _ => &*MODEL_LOGGER,
                    };
                    logger.log(
                        Level::Warn,
                        format_args!(
                            "No capability available at path {} for component {}, \
                                verify the component has the proper `use` declaration.",
                            requested_path, target.abs_moniker
                        ),
                    );
                }
                Err(_) => {}
            }
        })
        .detach();
    }));
    new_dir
}

#[cfg(test)]
pub mod test {

    use {
        super::*,
        crate::model::testing::test_helpers::MockServiceRequest,
        cm_rust::{CapabilityPath, DependencyType, UseProtocolDecl, UseSource},
        fidl::endpoints::{self, ProtocolMarker},
        fidl_fuchsia_component_runner as fcrunner,
        fidl_fuchsia_logger::{LogSinkMarker, LogSinkRequest},
        fuchsia_async,
        fuchsia_component::server::ServiceFs,
        futures::StreamExt,
        std::{
            convert::TryFrom,
            sync::{Arc, Mutex},
        },
    };

    #[test]
    fn test_subpath_handling() {
        let mut stem = "/".to_string();
        let mut full = "/".to_string();
        assert_eq!(IncomingNamespace::is_subpath_of(full, stem), Err(()));

        stem = "/".to_string();
        full = "/subdir".to_string();
        assert_eq!(IncomingNamespace::is_subpath_of(full, stem), Ok("subdir".to_string()));

        stem = "/subdir1/subdir2".to_string();
        full = "/subdir1/file.txt".to_string();
        assert_eq!(IncomingNamespace::is_subpath_of(full, stem), Err(()));

        stem = "/this/path/has/a/typ0".to_string();
        full = "/this/path/has/a/typo/not/exclamation/point".to_string();
        assert_eq!(IncomingNamespace::is_subpath_of(full, stem), Err(()));

        stem = "/subdir1".to_string();
        full = "/subdir1/subdir2/subdir3/file.txt".to_string();
        assert_eq!(
            IncomingNamespace::is_subpath_of(full, stem),
            Ok("subdir2/subdir3/file.txt".to_string())
        );
    }

    #[fuchsia::test]
    /// Tests that the logger is connected to when it is in a subdirectory of a
    /// namespace entry.
    async fn test_logger_at_root_of_entry() {
        let incoming_ns = IncomingNamespace::new(None).expect("namespace failed to create");
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
        };

        let (dir_client, dir_server) =
            endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>()
                .expect("failed to create VFS endpoints");
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir
            .serve_connection(dir_server.into_channel())
            .expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(
            Some(&mut root_dir),
            incoming_ns,
            ns_entries,
            log_decl,
            true,
        )
        .await;
    }

    #[fuchsia::test]
    /// Tests that the logger is connected to when it is in a subdirectory of a
    /// namespace entry.
    async fn test_logger_at_subdir_of_entry() {
        let incoming_ns = IncomingNamespace::new(None).expect("namespace failed to create");
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/arbitrary-dir/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
        };

        let (dir_client, dir_server) =
            endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>()
                .expect("failed to create VFS endpoints");
        let mut root_dir = ServiceFs::new_local();
        let mut svc_dir = root_dir.dir("arbitrary-dir");
        svc_dir.add_fidl_service_at(LogSinkMarker::NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir
            .serve_connection(dir_server.into_channel())
            .expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(
            Some(&mut root_dir),
            incoming_ns,
            ns_entries,
            log_decl,
            true,
        )
        .await;
    }

    #[fuchsia::test]
    async fn test_multiple_namespace_entries() {
        let incoming_ns = IncomingNamespace::new(None).expect("namespace failed to create");
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
        };

        let (dir_client, dir_server) =
            endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>()
                .expect("failed to create VFS endpoints");
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir
            .serve_connection(dir_server.into_channel())
            .expect("failed to add serving channel");

        // Create a directory for another namespace entry which we don't
        // actually expect to be accessed.
        let (extra_dir_client, extra_dir_server) =
            endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>()
                .expect("Failed creating directory endpoints");
        let mut extra_dir = ServiceFs::new_local();
        extra_dir.add_fidl_service(MockServiceRequest::LogSink);
        extra_dir
            .serve_connection(extra_dir_server.into_channel())
            .expect("serving channel failed");

        let ns_entries = vec![
            fcrunner::ComponentNamespaceEntry {
                path: Some("/svc".to_string()),
                directory: Some(dir_client),
                ..fcrunner::ComponentNamespaceEntry::EMPTY
            },
            fcrunner::ComponentNamespaceEntry {
                path: Some("/sv".to_string()),
                directory: Some(extra_dir_client),
                ..fcrunner::ComponentNamespaceEntry::EMPTY
            },
        ];

        verify_logger_connects_in_namespace(
            Some(&mut root_dir),
            incoming_ns,
            ns_entries,
            log_decl,
            true,
        )
        .await;
    }

    #[fuchsia::test]
    async fn test_no_connect_on_empty_namespace() {
        let incoming_ns = IncomingNamespace::new(None).expect("namespace failed to create");
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
        };

        let ns_entries = vec![];

        verify_logger_connects_in_namespace(
            Option::<&mut ServiceFs<fuchsia_component::server::ServiceObjLocal<MockServiceRequest>>>::None,
            incoming_ns,
            ns_entries,
            log_decl,
            false,
        )
        .await;
    }

    #[fuchsia::test]
    async fn test_logsink_dir_not_in_namespace() {
        let incoming_ns = IncomingNamespace::new(None).expect("namespace failed to create");
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
        };

        let (dir_client, dir_server) =
            endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>()
                .expect("failed to create VFS endpoints");
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::NAME, MockServiceRequest::LogSink);
        root_dir
            .serve_connection(dir_server.into_channel())
            .expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/not-the-svc-dir".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(
            Some(&mut root_dir),
            incoming_ns,
            ns_entries,
            log_decl,
            false,
        )
        .await;
    }

    /// Verify the expected logger connection behavior and that the logger is
    /// set or not in the namespace.
    async fn verify_logger_connects_in_namespace<
        T: fuchsia_component::server::ServiceObjTrait<Output = MockServiceRequest>,
    >(
        root_dir: Option<&mut ServiceFs<T>>,
        incoming_ns: IncomingNamespace,
        ns_entries: Vec<fcrunner::ComponentNamespaceEntry>,
        proto_decl: UseProtocolDecl,
        connects: bool,
    ) {
        let connection_count = if connects { 1u8 } else { 0u8 };
        // Create a task that will access the namespace by calling
        // `get_logger_from_ns`. This task won't complete until the VFS backing
        // the namespace starts responding to requests. This VFS is served by
        // code in the next stanza.
        fuchsia_async::Task::spawn(async move {
            let ns_size = ns_entries.len();
            let (procesed_ns, logger) =
                incoming_ns.get_logger_from_ns(ns_entries, &proto_decl).await;
            assert_eq!(logger.is_some(), connects);
            assert_eq!(ns_size, procesed_ns.len())
        })
        .detach();

        if let Some(dir) = root_dir {
            // Serve the directory and when the LogSink service is requested
            // provide a closure that counts number of calls to the Connect
            // method. Serving stops when the spawned task drops the
            // IncomingNamespace, which holds the other side of the VFS
            // directory handle.
            let request_count = Arc::new(Mutex::new(0u8));
            let request_count_copy = request_count.clone();
            dir.for_each_concurrent(10usize, move |request: MockServiceRequest| match request {
                MockServiceRequest::LogSink(mut r) => {
                    let req_count = request_count_copy.clone();
                    async move {
                        match r.next().await.expect("stream error").expect("fidl error") {
                            LogSinkRequest::Connect { .. } => {
                                let mut count = req_count.lock().expect("locking failed");
                                *count += 1;
                            }
                            LogSinkRequest::ConnectStructured { .. } => {
                                panic!("Unexpected call to `ConnectStructured`");
                            }
                            LogSinkRequest::WaitForInterestChange { responder: _ } => {
                                panic!("Unexpected call to `WaitForInterestChange`")
                            }
                        }
                    }
                }
            })
            .await;
            assert_eq!(*request_count.lock().expect("lock failed"), connection_count);
        }
    }
}
