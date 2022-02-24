// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![cfg(test)]

use {
    crate::{
        client::{connection_quality::SignalData, types as client_types},
        config_management::{
            Credential, NetworkConfig, NetworkConfigError, NetworkIdentifier, PastConnectionData,
            PastConnectionList, SavedNetworksManagerApi, ScanResultType,
        },
    },
    async_trait::async_trait,
    fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_zircon as zx,
    futures::{channel::mpsc, lock::Mutex},
    log::info,
    rand::Rng,
    std::{collections::HashMap, sync::Arc},
    wlan_common::hasher::WlanHasher,
};

pub struct FakeSavedNetworksManager {
    saved_networks: Mutex<HashMap<NetworkIdentifier, Vec<NetworkConfig>>>,
    disconnects_recorded: Mutex<Vec<DisconnectRecord>>,
    pub fail_all_stores: bool,
    pub active_scan_result_recorded: Arc<Mutex<bool>>,
    pub passive_scan_result_recorded: Arc<Mutex<bool>>,
    pub past_connections_response: PastConnectionList,
}

#[derive(Debug, Clone, PartialEq)]
pub struct DisconnectRecord {
    pub id: NetworkIdentifier,
    pub credential: Credential,
    pub bssid: client_types::Bssid,
    pub uptime: zx::Duration,
    pub curr_time: zx::Time,
}

impl FakeSavedNetworksManager {
    pub fn new() -> Self {
        Self {
            saved_networks: Mutex::new(HashMap::new()),
            disconnects_recorded: Mutex::new(vec![]),
            fail_all_stores: false,
            active_scan_result_recorded: Arc::new(Mutex::new(false)),
            passive_scan_result_recorded: Arc::new(Mutex::new(false)),
            past_connections_response: PastConnectionList::new(),
        }
    }

    /// Create FakeSavedNetworksManager, saving network configs with the specified
    /// network identifiers and credentials at init.
    pub fn new_with_saved_networks(network_configs: Vec<(NetworkIdentifier, Credential)>) -> Self {
        let saved_networks = network_configs
            .into_iter()
            .filter_map(|(id, cred)| {
                NetworkConfig::new(id.clone(), cred, false).ok().map(|config| (id, vec![config]))
            })
            .collect::<HashMap<NetworkIdentifier, Vec<NetworkConfig>>>();

        Self {
            saved_networks: Mutex::new(saved_networks),
            disconnects_recorded: Mutex::new(vec![]),
            fail_all_stores: false,
            active_scan_result_recorded: Arc::new(Mutex::new(false)),
            passive_scan_result_recorded: Arc::new(Mutex::new(false)),
            past_connections_response: PastConnectionList::new(),
        }
    }

    /// Create FakeSavedNetworksManager, that will always respond to get_past_connections with
    /// the specified value.
    pub fn new_with_past_connections_response(response: PastConnectionList) -> Self {
        Self {
            saved_networks: Mutex::new(HashMap::new()),
            disconnects_recorded: Mutex::new(vec![]),
            fail_all_stores: false,
            active_scan_result_recorded: Arc::new(Mutex::new(false)),
            passive_scan_result_recorded: Arc::new(Mutex::new(false)),
            past_connections_response: response,
        }
    }

    pub fn drain_recorded_disconnects(&self) -> Vec<DisconnectRecord> {
        self.disconnects_recorded
            .try_lock()
            .expect("expect locking self.disconnects_recorded to succeed")
            .drain(..)
            .collect()
    }

    /// Manually change the hidden network probabiltiy of a saved network.
    pub async fn update_hidden_prob(&self, id: NetworkIdentifier, hidden_prob: f32) {
        let mut saved_networks = self.saved_networks.lock().await;
        let networks = match saved_networks.get_mut(&id) {
            Some(networks) => networks,
            None => {
                info!("Failed to find network to update");
                return;
            }
        };
        for network in networks.iter_mut() {
            network.hidden_probability = hidden_prob;
        }
    }
}

#[async_trait]
impl SavedNetworksManagerApi for FakeSavedNetworksManager {
    async fn remove(
        &self,
        network_id: NetworkIdentifier,
        credential: Credential,
    ) -> Result<bool, NetworkConfigError> {
        let mut saved_networks = self.saved_networks.lock().await;
        if let Some(network_configs) = saved_networks.get_mut(&network_id) {
            let original_len = network_configs.len();
            network_configs.retain(|cfg| cfg.credential != credential);
            if original_len != network_configs.len() {
                return Ok(true);
            }
        }
        Ok(false)
    }

    async fn known_network_count(&self) -> usize {
        unimplemented!()
    }

    async fn lookup(&self, id: NetworkIdentifier) -> Vec<NetworkConfig> {
        self.saved_networks.lock().await.entry(id).or_default().iter().map(Clone::clone).collect()
    }

    async fn lookup_compatible(
        &self,
        _ssid: &client_types::Ssid,
        _scan_security: client_types::SecurityTypeDetailed,
    ) -> Vec<NetworkConfig> {
        unimplemented!()
    }

    /// Note that the configs-per-NetworkIdentifier limit is set to 1 in
    /// this mock struct. If a NetworkIdentifier is already stored, writing
    /// a config to it will evict the previously store one.
    async fn store(
        &self,
        network_id: NetworkIdentifier,
        credential: Credential,
    ) -> Result<Option<NetworkConfig>, NetworkConfigError> {
        if self.fail_all_stores {
            return Err(NetworkConfigError::StashWriteError);
        }
        let config = NetworkConfig::new(network_id.clone(), credential, false)?;
        return Ok(self
            .saved_networks
            .lock()
            .await
            .insert(network_id, vec![config])
            .map(|mut v| v.pop())
            .flatten());
    }

    async fn record_connect_result(
        &self,
        _id: NetworkIdentifier,
        _credential: &Credential,
        _bssid: client_types::Bssid,
        _connect_result: fidl_sme::ConnectResult,
        _discovered_in_scan: Option<fidl_common::ScanType>,
    ) {
    }

    async fn record_disconnect(
        &self,
        id: &NetworkIdentifier,
        credential: &Credential,
        bssid: client_types::Bssid,
        uptime: zx::Duration,
        curr_time: zx::Time,
    ) {
        let mut disconnects_recorded = self.disconnects_recorded.lock().await;
        disconnects_recorded.push(DisconnectRecord {
            id: id.clone(),
            credential: credential.clone(),
            bssid,
            uptime,
            curr_time,
        });
    }

    async fn record_periodic_metrics(&self) {}

    async fn record_scan_result(
        &self,
        scan_type: ScanResultType,
        _results: Vec<client_types::NetworkIdentifierDetailed>,
    ) {
        match scan_type {
            ScanResultType::Undirected => {
                let mut v = self.passive_scan_result_recorded.lock().await;
                *v = true;
            }
            ScanResultType::Directed(_) => {
                let mut v = self.active_scan_result_recorded.lock().await;
                *v = true
            }
        }
    }

    async fn get_networks(&self) -> Vec<NetworkConfig> {
        self.saved_networks
            .lock()
            .await
            .values()
            .into_iter()
            .map(|cfgs| cfgs.clone())
            .flatten()
            .collect()
    }

    async fn get_past_connections(
        &self,
        _id: &NetworkIdentifier,
        _credential: &Credential,
        _bssid: &client_types::Bssid,
    ) -> PastConnectionList {
        self.past_connections_response.clone()
    }
}

pub fn create_wlan_hasher() -> WlanHasher {
    WlanHasher::new(rand::thread_rng().gen::<u64>().to_le_bytes())
}

pub fn create_inspect_persistence_channel() -> (mpsc::Sender<String>, mpsc::Receiver<String>) {
    const DEFAULT_BUFFER_SIZE: usize = 100; // arbitrary value
    mpsc::channel(DEFAULT_BUFFER_SIZE)
}

pub fn create_fake_connection_data(
    bssid: client_types::Bssid,
    disconnect_time: zx::Time,
) -> PastConnectionData {
    let mut rng = rand::thread_rng();
    PastConnectionData::new(
        bssid,
        disconnect_time - zx::Duration::from_seconds(rng.gen::<u8>().into()),
        zx::Duration::from_seconds(rng.gen_range::<i64, _>(5..10).into()),
        disconnect_time,
        client_types::DisconnectReason::NetworkUnsaved,
        SignalData::new(rng.gen_range(-90..-20), rng.gen_range(-90..-20), 10),
        rng.gen::<u8>().into(),
    )
}
