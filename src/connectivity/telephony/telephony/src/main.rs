// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! System service for managing cellular modems

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_telephony_manager::{ManagerRequest, ManagerRequestStream},
    fidl_fuchsia_telephony_ril::{
        RadioInterfaceLayerMarker, RadioInterfaceLayerProxy, SetupMarker,
    },
    fuchsia_async as fasync,
    fuchsia_component::{
        client::{launch, launcher, App},
        fuchsia_single_component_package_url,
        server::ServiceFs,
    },
    fuchsia_inspect as inspect,
    fuchsia_syslog::{self as syslog, macros::*},
    fuchsia_vfs_watcher::{WatchEvent, Watcher},
    fuchsia_zircon as zx,
    futures::{
        future::{self, join},
        Future, StreamExt, TryFutureExt, TryStreamExt,
    },
    io_util::{open_directory_in_namespace, OPEN_RIGHT_READABLE},
    parking_lot::RwLock,
    qmi::connect_transport_device,
    std::fs::File,
    std::path::{Path, PathBuf},
    std::sync::Arc,
    thiserror::Error,
};

const QMI_TRANSPORT: &str = "/dev/class/qmi-transport";
const RIL_URI: &str = fuchsia_single_component_package_url!("ril-qmi");

#[derive(Debug, PartialEq)]
pub enum ModemType {
    Qmi,
    //TODO(bwb): Other types of modem interfaces
    At, // Ex: Mbim, AT, etc...
}

#[derive(Error, Debug)]
pub enum TelError {
    #[error("telephony svc does not know how to work with {:?} yet", 0)]
    UnknownTransport(ModemType),
    #[error("Connection to a radio has failed")]
    FailedConnection(),
    #[error("The Radio Interface Layer has returned a error: {:?}", 0)]
    RilError(fidl_fuchsia_telephony_ril::RilError),
}

pub async fn connect_qmi_transport(path: PathBuf) -> Result<fasync::Channel, Error> {
    let file = File::open(&path)?;
    let chan = connect_transport_device(&file).await?;
    Ok(fasync::Channel::from_channel(chan)?)
}

pub async fn start_modem(
    ty: ModemType,
    chan: zx::Channel,
    node: inspect::Node,
    path: PathBuf,
) -> Result<Radio, Error> {
    let launcher = launcher().context("Failed to open launcher service")?;
    let app = launch(&launcher, RIL_URI.to_string(), None)
        .context("Failed to launch qmi-modem service")?;
    let setup_ril = app.connect_to_protocol::<SetupMarker>()?;
    let ril = app.connect_to_protocol::<RadioInterfaceLayerMarker>()?;
    match ty {
        ModemType::Qmi => match setup_ril.connect_transport(chan.into()).await? {
            Ok(_) => Ok(Radio::new(app, ril, node, path)),
            Err(e) => Err(TelError::RilError(e).into()),
        },
        t => return Err(TelError::UnknownTransport(t).into()),
    }
}

pub fn start_service(
    mgr: Arc<Manager>,
    stream: ManagerRequestStream,
) -> impl Future<Output = Result<(), Error>> {
    stream
        .try_for_each(move |evt| {
            let _ = match evt {
                ManagerRequest::IsAvailable { responder } => {
                    responder.send(!mgr.radios.read().is_empty())
                }
                // TODO(bwb): Get based on iface id, not just first one
                ManagerRequest::GetRilHandle { ril_iface, responder } => {
                    fx_log_info!("Vending a RIL handle to another process");
                    let radios = mgr.radios.read();
                    match radios.first() {
                        Some(radio) => {
                            let resp = radio.app.pass_to_protocol::<RadioInterfaceLayerMarker>(
                                ril_iface.into_channel(),
                            );
                            responder.send(resp.is_ok())
                        }
                        None => responder.send(false),
                    }
                }
            };
            future::ready(Ok(()))
        })
        .map_err(|e| e.into())
}

pub struct Radio {
    pub app: App,
    // TODO(bwb) Deref into Ril proxy?
    #[allow(dead_code)]
    // TODO(bwb) remove dead_code, needed to retain ownership for now.
    ril: RadioInterfaceLayerProxy,
    path: PathBuf,

    _node: inspect::Node,
    _uri_str_property: inspect::StringProperty,
}

impl Radio {
    pub fn new(
        app: App,
        ril: RadioInterfaceLayerProxy,
        node: inspect::Node,
        path: PathBuf,
    ) -> Self {
        let uri_str_property = node.create_string("URI", path.to_string_lossy().as_ref());
        Radio { app, ril, path, _node: node, _uri_str_property: uri_str_property }
    }
}

pub struct Manager {
    radios: RwLock<Vec<Radio>>,

    _node: inspect::Node,
    radios_node: inspect::Node,
}

impl Manager {
    pub fn new(node: inspect::Node) -> Self {
        let radios_node = node.create_child("radios");
        Manager { radios: RwLock::new(vec![]), _node: node, radios_node }
    }

    async fn add_new_devices(
        &self,
        file_name: PathBuf,
        path: PathBuf,
        fn_open_file: fn(&PathBuf) -> File,
    ) -> Result<(), Error> {
        let qmi_path = path.join(file_name.clone());
        fx_log_info!("Connecting to {}", qmi_path.display());
        let file = fn_open_file(&qmi_path);
        let channel = qmi::connect_transport_device(&file).await?;
        let svc = start_modem(
            ModemType::Qmi,
            channel,
            self.radios_node.create_child(format!("radio-qmi-{}", file_name.display())),
            qmi_path,
        )
        .await?;
        self.radios.write().push(svc);
        Ok(())
    }

    fn remove_devices(&self, file_name: PathBuf, path: PathBuf) {
        let qmi_path = path.join(file_name);
        fx_log_info!("Disconnected from {}", qmi_path.display());

        let mut radios = self.radios.write();
        radios.retain(|radio| radio.path != qmi_path);
    }

    async fn watch_new_devices(
        &self,
        dir_path: &str,
        fn_open_dir: fn(&PathBuf) -> fidl_fuchsia_io::DirectoryProxy,
        fn_open_file: fn(&PathBuf) -> File,
    ) -> Result<(), Error> {
        // TODO(bwb): make more generic to support non-qmi devices
        let path: &Path = Path::new(dir_path);
        let dir = fn_open_dir(&path.to_path_buf());
        let mut watcher = Watcher::new(dir).await.unwrap();
        while let Some(msg) = watcher.try_next().await? {
            match msg.event {
                WatchEvent::EXISTING | WatchEvent::ADD_FILE => {
                    self.add_new_devices(msg.filename, path.to_path_buf(), fn_open_file).await?;
                }
                WatchEvent::REMOVE_FILE => self.remove_devices(msg.filename, path.to_path_buf()),
                _ => (),
            }
        }
        Ok(())
    }

    fn open_dir(path: &PathBuf) -> fidl_fuchsia_io::DirectoryProxy {
        open_directory_in_namespace(path.to_str().unwrap(), OPEN_RIGHT_READABLE).unwrap()
    }
    fn open_file(path: &PathBuf) -> File {
        File::open(path).unwrap()
    }
}

fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["telephony"]).expect("Can't init logger");
    fx_log_info!("Starting telephony management service...");
    let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;

    // Creates a new inspector object. This will create the "root" node in the
    // inspect tree to which further children objects can be added.
    let inspector = inspect::Inspector::new();

    let manager = Arc::new(Manager::new(inspector.root().create_child("manager")));
    let mgr = manager.clone();
    let device_watcher = manager
        .watch_new_devices(QMI_TRANSPORT, Manager::open_dir, Manager::open_file)
        .unwrap_or_else(|e| fx_log_err!("Failed to watch new devices: {:?}", e));

    let mut fs = ServiceFs::new();

    fs.dir("svc").add_fidl_service(move |stream| {
        fx_log_info!("Spawning Management Interface");
        fasync::Task::spawn(
            start_service(mgr.clone(), stream)
                .unwrap_or_else(|e| fx_log_err!("Failed to spawn {:?}", e)),
        )
        .detach()
    });

    // Serves the Inspect Tree at the standard location "/diagnostics/fuchsia.inspect.Tree"
    inspect_runtime::serve(&inspector, &mut fs)?;

    fs.take_and_serve_directory_handle()?;

    let ((), ()) = executor.run_singlethreaded(join(device_watcher, fs.collect::<()>()));
    Ok(())
}

#[cfg(test)]
mod test {

    use {super::*, fuchsia_inspect::assert_data_tree, tel_dev::isolated_devmgr};

    impl Manager {
        fn open_isolated_devmgr_file(path: &PathBuf) -> File {
            isolated_devmgr::open_file_in_isolated_devmgr(path)
                .expect("Opening file in IsolatedDevmgr failed")
        }
    }

    #[test]
    fn pass() -> () {
        ();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_manager_inspect_tree() -> Result<(), Error> {
        let inspector = inspect::Inspector::new();
        let manager = Arc::new(Manager::new(inspector.root().create_child("manager")));
        let launcher = launcher().context("Failed to open launcher service")?;

        let create_app_and_ril_for_test = || -> Result<(App, RadioInterfaceLayerProxy), Error> {
            let app = launch(&launcher, RIL_URI.to_string(), None)
                .context("Failed to launch ril-qmi service")?;
            let ril = app.connect_to_protocol::<RadioInterfaceLayerMarker>()?;
            Ok((app, ril))
        };

        assert_data_tree!(inspector, root: {
            manager: {
                radios: {}
            }
        });

        {
            let (app, ril) = create_app_and_ril_for_test().unwrap();
            let path = PathBuf::from(r"class/qmi-transport/000");
            let _radio1 = Radio::new(app, ril, manager.radios_node.create_child("radio-1"), path);
            assert_data_tree!(inspector, root: {
               manager: {
                   radios: {
                       "radio-1": {
                           URI: "class/qmi-transport/000"
                       }
                   }
               }
            });

            // Radio will be removed from inspect tree as it is out of scope/dropped
        }

        assert_data_tree!(inspector, root: {
            manager: {
                radios: {}
            }
        });

        let (app, ril) = create_app_and_ril_for_test().unwrap();
        let path = PathBuf::from(r"class/qmi-transport/001");
        let node = manager.radios_node.create_child("radio-1");
        let _radio1 = Radio::new(app, ril, node, path);
        assert_data_tree!(inspector, root: {
            manager: {
                radios: {
                    "radio-1": {
                        URI: "class/qmi-transport/001"
                    }
                }
            }
        });

        let (app2, ril2) = create_app_and_ril_for_test().unwrap();
        let path2 = PathBuf::from(r"class/qmi-transport/002");
        let node2 = manager.radios_node.create_child("radio-2");
        let _radio2 = Radio::new(app2, ril2, node2, path2);

        assert_data_tree!(inspector, root: {
            manager: {
                radios: {
                    "radio-1": {
                        URI: "class/qmi-transport/001"
                    },
                    "radio-2": {
                        URI: "class/qmi-transport/002"
                    }
                }
            }
        });

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_tel_mgr_add_remove_device_inspect_tree() -> Result<(), Error> {
        syslog::init_with_tags(&["test_tel_mgr"]).expect("Can't init logger");

        let inspector = inspect::Inspector::new();
        let manager = Arc::new(Manager::new(inspector.root().create_child("manager")));

        manager
            .add_new_devices(
                PathBuf::from("000"),
                PathBuf::from(r"class/qmi-transport"),
                Manager::open_isolated_devmgr_file,
            )
            .await?;

        assert_data_tree!(inspector, root: {
           manager: {
               radios: {
                   "radio-qmi-000": {
                       URI: "class/qmi-transport/000"
                   }
               }
           }
        });

        manager.remove_devices(PathBuf::from("000"), PathBuf::from(r"class/qmi-transport"));

        assert_data_tree!(inspector, root: {
           manager: {
               radios: {
               }
           }
        });

        Ok(())
    }
}
