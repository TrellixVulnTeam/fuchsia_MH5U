// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    app_set::{EagerPackage, FuchsiaAppSet},
    channel::{ChannelConfig, ChannelConfigs},
    eager_package_config::EagerPackageConfigs,
};
use anyhow::{anyhow, Error};
use fidl_fuchsia_boot::{ArgumentsMarker, ArgumentsProxy};
use log::{error, warn};
use omaha_client::{
    common::App,
    configuration::{Config, Updater},
    protocol::{request::OS, Cohort},
};
use std::fs;
use std::io;
use version::Version;

/// This struct is the overall "configuration" of the omaha client.  Minus the PolicyConfig.  That
/// should probably be included in here as well, eventually.
pub struct ClientConfiguration {
    pub platform_config: omaha_client::configuration::Config,
    pub app_set: FuchsiaAppSet,
    pub channel_data: ChannelData,
}

/// This wraps up all the channel-related configuration (name, appid, config, and any other params
/// that they might have).  Mostly as a grouping for returning from fns in a cleaner way than a
/// tuple provides.
pub struct ChannelData {
    pub source: ChannelSource,
    pub name: Option<String>,
    pub config: Option<ChannelConfig>,
    pub appid: String,
}

/// The source of the channel configuration.
#[derive(Debug, Eq, PartialEq)]
pub enum ChannelSource {
    MinFS,
    Default,
    VbMeta,
}

impl ClientConfiguration {
    /// Given an (optional) set of ChannelConfigs, load all the other configs that are needed, and
    /// construct an overall configuration struct that can then be used.
    /// TODO: Move the reading of channel_configs.json into this.
    pub async fn initialize(channel_configs: Option<&ChannelConfigs>) -> Result<Self, io::Error> {
        let version = get_version()?;
        let (vbmeta_appid, vbmeta_channel) =
            get_appid_and_channel_from_vbmeta().await.unwrap_or_else(|e| {
                warn!("Failed to get app id and channel from vbmeta {:?}", e);
                (None, None)
            });
        Ok(Self::initialize_from(&version, channel_configs, vbmeta_appid, vbmeta_channel).await)
    }

    async fn initialize_from(
        version: &str,
        channel_configs: Option<&ChannelConfigs>,
        appid: Option<String>,
        channel_name: Option<String>,
    ) -> Self {
        let (channel_name, channel_source) = if channel_name.is_some() {
            (channel_name, ChannelSource::VbMeta)
        } else {
            // The channel wasn't found in VBMeta, so instead look for a default channel in the
            // channel configuration.
            if let Some(ChannelConfigs { default_channel: Some(name), .. }) = channel_configs {
                (Some(name.clone()), ChannelSource::Default)
            } else {
                // Channel will be loaded from `Storage` by state machine.
                (None, ChannelSource::MinFS)
            }
        };

        // Locate the channel config for the channel, if it exists.
        let channel_config = if let (Some(name), Some(configs)) = (&channel_name, channel_configs) {
            configs.get_channel(name)
        } else {
            None
        };

        // If no appid in vbmeta, look up the appid of the channel from the channel config.
        let appid = if let (None, Some(config)) = (&appid, &channel_config) {
            config.appid.clone()
        } else {
            appid
        };

        // If no appid in the channel configs, then attempt to read from config data.
        let appid =
            appid.unwrap_or_else(|| match fs::read_to_string("/config/data/omaha_app_id") {
                Ok(id) => id,
                Err(e) => {
                    error!("Unable to read omaha app id from config/data: {:?}", e);
                    String::new()
                }
            });

        let product_id = get_productid_from_vbmeta().await.unwrap_or_else(|e| {
            warn!("Failed to get single product id from vbmeta {:?}", e);
            None
        });

        // Construct the only app that Fuchsia has
        let app = App::builder(appid.clone(), Self::parse_version(version))
            .with_cohort(Cohort {
                hint: channel_name.clone(),
                name: channel_name.clone(),
                ..Cohort::default()
            })
            .with_extra("channel", channel_name.clone().unwrap_or_default())
            .with_extra("product_id", product_id.unwrap_or_default())
            .build();
        let mut app_set = FuchsiaAppSet::new(app);

        match EagerPackageConfigs::from_namespace() {
            Ok(eager_package_configs) => {
                Self::add_eager_packages(&mut app_set, eager_package_configs)
            }
            Err(e) => {
                match e.downcast_ref::<std::io::Error>() {
                    Some(io_err) if io_err.kind() == std::io::ErrorKind::NotFound => {
                        warn!("eager package config not found: {:#}", anyhow!(e))
                    }
                    _ => error!(
                        "Failed to load eager package config from namespace: {:#}",
                        anyhow!(e)
                    ),
                };
            }
        }

        ClientConfiguration {
            platform_config: get_config(&version).await,
            app_set,
            channel_data: ChannelData {
                source: channel_source,
                name: channel_name,
                config: channel_config,
                appid,
            },
        }
    }

    /// Add all eager packages in eager package config to app set.
    fn add_eager_packages(app_set: &mut FuchsiaAppSet, eager_package_configs: EagerPackageConfigs) {
        for package in eager_package_configs.packages {
            // TODO: ask pkg-resolver for current channel and version first.
            let version = [0];
            let channel_config = package.channel_config.get_default_channel();

            let appid = match channel_config.as_ref().and_then(|c| c.appid.as_ref()) {
                Some(appid) => &appid,
                None => {
                    error!("no appid for package '{}'", package.url);
                    ""
                }
            };

            let mut app_builder = App::builder(appid, version);
            if let Some(channel_config) = channel_config {
                app_builder = app_builder
                    .with_cohort(Cohort {
                        hint: Some(channel_config.name.clone()),
                        name: Some(channel_config.name.clone()),
                        ..Cohort::default()
                    })
                    .with_extra("channel", channel_config.name);
            }
            let app = app_builder.build();

            app_set.add_eager_package(EagerPackage::new(app, Some(package.channel_config)));
        }
    }

    /// Helper to wrap the parsing of a version string, logging any parse errors and making sure
    /// that there's still some valid value as a result.
    fn parse_version(version: &str) -> Version {
        match version.parse::<Version>() {
            Ok(parsed_version) => parsed_version,
            Err(e) => {
                error!("Unable to parse '{}' as Omaha version format: {:?}", version, e);
                Version::from([0])
            }
        }
    }
}

pub async fn get_config(version: &str) -> Config {
    // This file does not exist in production, it is only used in integration/e2e testing.
    let service_url = match get_service_url_from_vbmeta().await {
        Ok(Some(url)) => url,
        _ => fs::read_to_string("/config/data/omaha_url").unwrap_or_else(|_| {
            "https://clients2.google.com/service/update2/fuchsia/json".to_string()
        }),
    };
    Config {
        updater: Updater { name: "Fuchsia".to_string(), version: Version::from([0, 0, 1, 0]) },

        os: OS {
            platform: "Fuchsia".to_string(),
            version: version.to_string(),
            service_pack: "".to_string(),
            arch: std::env::consts::ARCH.to_string(),
        },

        service_url,
    }
}

pub fn get_version() -> Result<String, io::Error> {
    fs::read_to_string("/config/build-info/version").map(|s| s.trim_end().to_string())
}

async fn get_productid_from_vbmeta() -> Result<Option<String>, Error> {
    let proxy = fuchsia_component::client::connect_to_protocol::<ArgumentsMarker>()?;
    get_productid_from_vbmeta_impl(proxy).await
}

async fn get_productid_from_vbmeta_impl(proxy: ArgumentsProxy) -> Result<Option<String>, Error> {
    let res = proxy.get_string("product_id").await;
    match res {
        Ok(id) => Ok(id),
        Err(e) => Err(anyhow!("error getting product id : {:?}", e)),
    }
}

async fn get_appid_and_channel_from_vbmeta() -> Result<(Option<String>, Option<String>), Error> {
    let proxy = fuchsia_component::client::connect_to_protocol::<ArgumentsMarker>()?;
    get_appid_and_channel_from_vbmeta_impl(proxy).await
}

async fn get_appid_and_channel_from_vbmeta_impl(
    proxy: ArgumentsProxy,
) -> Result<(Option<String>, Option<String>), Error> {
    let vec = vec!["omaha_app_id", "ota_channel"];
    let res = proxy.get_strings(&mut vec.into_iter()).await?;
    if res.len() != 2 {
        Err(anyhow!("Remote endpoint returned {} values, expected 2", res.len()))
    } else {
        Ok((res[0].clone(), res[1].clone()))
    }
}

async fn get_service_url_from_vbmeta() -> Result<Option<String>, Error> {
    let proxy = fuchsia_component::client::connect_to_protocol::<ArgumentsMarker>()?;
    get_service_url_from_vbmeta_impl(proxy).await
}

async fn get_service_url_from_vbmeta_impl(proxy: ArgumentsProxy) -> Result<Option<String>, Error> {
    Ok(proxy.get_string("omaha_url").await?)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::eager_package_config::EagerPackageConfig;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_boot::ArgumentsRequest;
    use fuchsia_async as fasync;
    use fuchsia_url::pkg_url::PkgUrl;
    use futures::prelude::*;
    use omaha_client::app_set::AppSet;

    #[fasync::run_singlethreaded(test)]
    async fn test_get_config() {
        let client_config = ClientConfiguration::initialize_from("1.2.3.4", None, None, None).await;
        let config = client_config.platform_config;
        assert_eq!(config.updater.name, "Fuchsia");
        let os = config.os;
        assert_eq!(os.platform, "Fuchsia");
        assert_eq!(os.version, "1.2.3.4");
        assert_eq!(os.arch, std::env::consts::ARCH);
        assert_eq!(config.service_url, "https://clients2.google.com/service/update2/fuchsia/json");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set_config_read_init() {
        let config = ClientConfiguration::initialize_from("1.2.3.4", None, None, None).await;
        assert_eq!(config.channel_data.source, ChannelSource::MinFS);
        let apps = config.app_set.get_apps();
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "fuchsia:test-app-id");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, None);
        assert_eq!(apps[0].cohort.hint, None);
        assert_eq!(apps[0].extra_fields["product_id"], "");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set_default_channel() {
        let config = ClientConfiguration::initialize_from(
            "1.2.3.4",
            Some(&ChannelConfigs {
                default_channel: Some("default-channel".to_string()),
                known_channels: vec![],
            }),
            None,
            None,
        )
        .await;
        assert_eq!(config.channel_data.source, ChannelSource::Default);
        let apps = config.app_set.get_apps();
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "fuchsia:test-app-id");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, Some("default-channel".to_string()));
        assert_eq!(apps[0].cohort.hint, Some("default-channel".to_string()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_channel_data_configured() {
        let channel_config = ChannelConfig::with_appid("some-channel", "some-appid");
        let channel_configs = ChannelConfigs {
            default_channel: Some(channel_config.name.clone()),
            known_channels: vec![channel_config.clone()],
        };
        let config =
            ClientConfiguration::initialize_from("1.2.3.4", Some(&channel_configs), None, None)
                .await;
        let channel_data = config.channel_data;

        assert_eq!(channel_data.source, ChannelSource::Default);
        assert_eq!(channel_data.name, Some("some-channel".to_string()));
        assert_eq!(channel_data.config, Some(channel_config));
        assert_eq!(channel_data.appid, "some-appid");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set_appid_from_channel_configs() {
        let config = ClientConfiguration::initialize_from(
            "1.2.3.4",
            Some(&ChannelConfigs {
                default_channel: Some("some-channel".to_string()),
                known_channels: vec![
                    ChannelConfig::new("no-appid-channel"),
                    ChannelConfig::with_appid("wrong-channel", "wrong-appid"),
                    ChannelConfig::with_appid("some-channel", "some-appid"),
                    ChannelConfig::with_appid("some-other-channel", "some-other-appid"),
                ],
            }),
            None,
            None,
        )
        .await;
        assert_eq!(config.channel_data.source, ChannelSource::Default);
        let apps = config.app_set.get_apps();
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "some-appid");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, Some("some-channel".to_string()));
        assert_eq!(apps[0].cohort.hint, Some("some-channel".to_string()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set_invalid_version() {
        let config =
            ClientConfiguration::initialize_from("invalid version", None, None, None).await;
        let apps = config.app_set.get_apps();
        assert_eq!(apps[0].version, Version::from([0]));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_appid_and_channel_from_vbmeta() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let (appid, channel) = get_appid_and_channel_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(appid, Some("test-appid".to_string()));
            assert_eq!(channel, Some("test-channel".to_string()));
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetStrings { keys, responder }) => {
                    assert_eq!(keys, vec!["omaha_app_id", "ota_channel"]);
                    let vec: Vec<Option<&str>> = vec![Some("test-appid"), Some("test-channel")];
                    responder.send(&mut vec.into_iter()).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_productid_from_vbmeta() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let product_id = get_productid_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(product_id, Some("test-productid".to_string()));
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetString { key, responder }) => {
                    assert_eq!(key, "product_id".to_string());
                    let ret = Some("test-productid");
                    responder.send(ret).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_productid_from_vbmeta_missing() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let product_id = get_productid_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(product_id, None);
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetString { key, responder }) => {
                    assert_eq!(key, "product_id".to_string());
                    let ret = None;
                    responder.send(ret).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_init_from_vbmeta() {
        let config = ClientConfiguration::initialize_from(
            "1.2.3.4",
            Some(&ChannelConfigs {
                default_channel: Some("wrong-channel".to_string()),
                known_channels: vec![ChannelConfig::with_appid("wrong-channel", "wrong-appid")],
            }),
            Some("vbmeta-appid".to_string()),
            Some("vbmeta-channel".to_string()),
        )
        .await;
        assert_eq!(config.channel_data.source, ChannelSource::VbMeta);
        assert_eq!(config.channel_data.name, Some("vbmeta-channel".to_string()));
        assert_eq!(config.channel_data.config, None);
        assert_eq!(config.channel_data.appid, "vbmeta-appid");
        let apps = config.app_set.get_apps();
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "vbmeta-appid");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, Some("vbmeta-channel".to_string()));
        assert_eq!(apps[0].cohort.hint, Some("vbmeta-channel".to_string()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_appid_and_channel_from_vbmeta_missing() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let (appid, channel) = get_appid_and_channel_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(appid, None);
            assert_eq!(channel, None);
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetStrings { keys, responder }) => {
                    assert_eq!(keys.len(), 2);
                    let ret: Vec<Option<&str>> = vec![None, None];
                    responder.send(&mut ret.into_iter()).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_appid_and_channel_from_vbmeta_error() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            assert!(get_appid_and_channel_from_vbmeta_impl(proxy).await.is_err());
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetStrings { .. }) => {
                    // Don't respond.
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_service_url_from_vbmeta() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let url = get_service_url_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(url, Some("test-url".to_string()));
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetString { key, responder }) => {
                    assert_eq!(key, "omaha_url");
                    responder.send(Some("test-url")).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_service_url_from_vbmeta_missing() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            let url = get_service_url_from_vbmeta_impl(proxy).await.unwrap();
            assert_eq!(url, None);
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetString { key, responder }) => {
                    assert_eq!(key, "omaha_url");
                    responder.send(None).expect("send failed");
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_service_url_from_vbmeta_error() {
        let (proxy, mut stream) = create_proxy_and_stream::<ArgumentsMarker>().unwrap();
        let fut = async move {
            assert!(get_service_url_from_vbmeta_impl(proxy).await.is_err());
        };
        let stream_fut = async move {
            match stream.next().await.unwrap() {
                Ok(ArgumentsRequest::GetString { .. }) => {
                    // Don't respond.
                }
                request => panic!("Unexpected request: {:?}", request),
            }
        };
        future::join(fut, stream_fut).await;
    }

    #[test]
    fn test_add_eager_packages() {
        let system_app = App::builder("system_app_id", [1]).build();
        let mut app_set = FuchsiaAppSet::new(system_app.clone());
        let config = EagerPackageConfigs {
            packages: vec![
                EagerPackageConfig {
                    url: PkgUrl::parse("fuchsia-pkg://example.com/package").unwrap(),
                    flavor: Some("debug".into()),
                    channel_config: ChannelConfigs {
                        default_channel: Some("stable".into()),
                        known_channels: vec![
                            ChannelConfig {
                                name: "stable".into(),
                                repo: "stable".into(),
                                appid: Some("1a2b3c4d".into()),
                                check_interval_secs: None,
                            },
                            ChannelConfig {
                                name: "beta".into(),
                                repo: "beta".into(),
                                appid: Some("1a2b3c4d".into()),
                                check_interval_secs: None,
                            },
                            ChannelConfig {
                                name: "alpha".into(),
                                repo: "alpha".into(),
                                appid: Some("1a2b3c4d".into()),
                                check_interval_secs: None,
                            },
                            ChannelConfig {
                                name: "test".into(),
                                repo: "test".into(),
                                appid: Some("2b3c4d5e".into()),
                                check_interval_secs: None,
                            },
                        ],
                    },
                },
                EagerPackageConfig {
                    url: PkgUrl::parse("fuchsia-pkg://example.com/package2").unwrap(),
                    flavor: None,
                    channel_config: ChannelConfigs {
                        default_channel: None,
                        known_channels: vec![ChannelConfig {
                            name: "stable".into(),
                            repo: "stable".into(),
                            appid: Some("3c4d5e6f".into()),
                            check_interval_secs: None,
                        }],
                    },
                },
            ],
        };
        ClientConfiguration::add_eager_packages(&mut app_set, config);
        let package_app = App::builder("1a2b3c4d", [0])
            .with_cohort(Cohort {
                hint: Some("stable".into()),
                name: Some("stable".into()),
                ..Cohort::default()
            })
            .with_extra("channel", "stable")
            .build();
        let package2_app = App::builder("", [0]).build();
        assert_eq!(app_set.get_apps(), vec![system_app, package_app, package2_app]);
    }
}
