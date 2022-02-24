// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This file contains a data type for pre-authentication state, which is
//! mutable per-account data that is readable when an account is locked. It also
//! contains two implementations for persistence of the data, one Stash-based
//! and one in-memory fake store, for use with tests.

use account_common::{AccountManagerError, ResultExt};
use anyhow::format_err;
use async_trait::async_trait;
use fidl::endpoints::create_proxy;
use fidl_fuchsia_identity_account::Error as ApiError;
use fidl_fuchsia_mem::Buffer;
use fidl_fuchsia_stash::{StoreAccessorProxy, StoreMarker, StoreProxy, Value};
use fuchsia_component::client::connect_to_protocol;
use fuchsia_zircon::Vmo;
use futures::lock::Mutex;
use std::sync::Arc;

/// Identifier in stash for the authentication mechanism id field.
const AUTH_MECHANISM_ID: &str = "auth_mechanism_id";

/// Identifier in stash for the enrollment data field.
const ENROLLMENT_DATA: &str = "enrollment_data";

/// Pre-authentication state for a system account.
#[derive(Clone, Debug, PartialEq)]
pub enum State {
    /// No authentication mechanism enrollments.
    NoEnrollments,

    /// A single enrollment of an authentication mechanism,
    /// containig the ID of the authentication mechanism and
    /// the enrollment data for it.
    SingleEnrollment { auth_mechanism_id: String, data: Vec<u8> },
}

/// Manages persistent pre-auth state. Capable of reading and writing state
/// atomically.
#[async_trait]
pub trait Manager: Send + Sync {
    /// Returns the current pre-auth state.
    async fn get(&self) -> Result<Arc<State>, AccountManagerError>;

    /// Sets the pre-auth state.
    async fn put(&self, state: State) -> Result<(), AccountManagerError>;

    /// Removes the pre-auth state.
    async fn remove(&self) -> Result<(), AccountManagerError>;
}

/// Stash-backed pre-auth manager. It uses two Stash fields, for auth mechanism
/// id and enrollment data, respectively. There are two valid states: either no
/// fields are set or both are set. The stash manager keeps a cached version of
/// the latest seen pre-auth state, so that reads can be cheap.
pub struct StashManager {
    store_proxy: StoreProxy,
    cached_state: Mutex<Option<Arc<State>>>,
}

impl StashManager {
    /// Create a StashManager using a given store name.
    pub fn create(store_name: &str) -> Result<Self, AccountManagerError> {
        let store_proxy =
            connect_to_protocol::<StoreMarker>().account_manager_error(ApiError::Resource)?;
        store_proxy.identify(store_name)?;
        let cached_state = Mutex::new(None);
        Ok(Self { store_proxy, cached_state })
    }

    fn create_accessor(&self, read_only: bool) -> Result<StoreAccessorProxy, AccountManagerError> {
        let (accessor_proxy, server_end) = create_proxy()?;
        self.store_proxy.create_accessor(read_only, server_end)?;
        Ok(accessor_proxy)
    }

    // Fetches and returns the pre-auth state from the Stash service.
    async fn fetch_state(&self) -> Result<State, AccountManagerError> {
        let accessor = self.create_accessor(true)?;
        let auth_mechanism_id_val = accessor.get_value(AUTH_MECHANISM_ID).await?.map(|x| *x);
        let enrollment_data_val = accessor.get_value(ENROLLMENT_DATA).await?.map(|x| *x);
        match (auth_mechanism_id_val, enrollment_data_val) {
            (None, None) => Ok(State::NoEnrollments),
            (Some(Value::Stringval(auth_mechanism_id)), Some(Value::Bytesval(buf))) => {
                let data = read_mem_buffer(&buf)?;
                Ok(State::SingleEnrollment { auth_mechanism_id, data })
            }
            (auth_mechanism_id_opt, enrollment_data_opt) => Err(format_err!(
                "Invalid pre-auth data read: {}: {:?}, {}: {:?}",
                AUTH_MECHANISM_ID,
                auth_mechanism_id_opt,
                ENROLLMENT_DATA,
                enrollment_data_opt
            )),
        }
        .account_manager_error(ApiError::Unknown)
    }
}

#[async_trait]
impl Manager for StashManager {
    async fn get(&self) -> Result<Arc<State>, AccountManagerError> {
        let mut cached_state = self.cached_state.lock().await;
        let current_state = match &*cached_state {
            None => Arc::new(self.fetch_state().await?),
            Some(cached_state) => Arc::clone(&cached_state),
        };
        *cached_state = Some(Arc::clone(&current_state));
        Ok(current_state)
    }

    async fn put(&self, state: State) -> Result<(), AccountManagerError> {
        let accessor = self.create_accessor(false)?;
        match &state {
            &State::NoEnrollments => {
                accessor.delete_value(AUTH_MECHANISM_ID)?;
                accessor.delete_value(ENROLLMENT_DATA)?;
            }
            &State::SingleEnrollment { ref auth_mechanism_id, ref data } => {
                let mut auth_mechanism_id_val = Value::Stringval(auth_mechanism_id.clone());
                let mut data_val = Value::Bytesval(write_mem_buffer(&data)?);
                accessor.set_value(AUTH_MECHANISM_ID, &mut auth_mechanism_id_val)?;
                accessor.set_value(ENROLLMENT_DATA, &mut data_val)?;
            }
        }
        accessor.flush().await?.map_err(|err| {
            AccountManagerError::new(ApiError::Resource)
                .with_cause(format_err!("Failed committing update to stash: {:?}", err))
        })?;
        *self.cached_state.lock().await = Some(Arc::new(state));
        Ok(())
    }

    async fn remove(&self) -> Result<(), AccountManagerError> {
        self.put(State::NoEnrollments).await
    }
}

fn read_mem_buffer(buffer: &Buffer) -> Result<Vec<u8>, AccountManagerError> {
    let mut res = vec![0; buffer.size as usize];
    buffer.vmo.read(&mut res[..], 0).account_manager_error(ApiError::Resource)?;
    Ok(res)
}

fn write_mem_buffer(data: &[u8]) -> Result<Buffer, AccountManagerError> {
    let vmo = Vmo::create(data.len() as u64).account_manager_error(ApiError::Resource)?;
    vmo.write(&data, 0).account_manager_error(ApiError::Resource)?;
    Ok(Buffer { vmo, size: data.len() as u64 })
}

/// Pre-auth manager with an in-memory state.
pub struct InMemoryManager {
    state: Mutex<Arc<State>>,
}

impl InMemoryManager {
    /// Create an in-memory manager with a pre-set initial state.
    pub fn create(initial_state: State) -> Self {
        Self { state: Mutex::new(Arc::new(initial_state)) }
    }
}

#[async_trait]
impl Manager for InMemoryManager {
    async fn get(&self) -> Result<Arc<State>, AccountManagerError> {
        let state = &*self.state.lock().await;
        Ok(Arc::clone(&state))
    }

    async fn put(&self, state: State) -> Result<(), AccountManagerError> {
        let mut state_lock = self.state.lock().await;
        *state_lock = Arc::new(state);
        Ok(())
    }

    async fn remove(&self) -> Result<(), AccountManagerError> {
        self.put(State::NoEnrollments).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_async as fasync;
    use lazy_static::lazy_static;
    use rand::{
        distributions::{Alphanumeric, DistString as _},
        thread_rng,
    };

    lazy_static! {
        static ref TEST_STATE: State = State::SingleEnrollment {
            auth_mechanism_id: String::from("test_id"),
            data: vec![1, 2, 3],
        };
    }

    fn random_store_id() -> String {
        let rand_string = Alphanumeric.sample_string(&mut thread_rng(), 20);
        format!("pre_auth_test_{}", &rand_string)
    }

    #[fasync::run_until_stalled(test)]
    async fn in_memory_basic() -> Result<(), AccountManagerError> {
        let manager = InMemoryManager::create(State::NoEnrollments);
        assert_eq!(manager.get().await?.as_ref(), &State::NoEnrollments);
        manager.put(TEST_STATE.clone()).await?;
        assert_eq!(manager.get().await?.as_ref(), &*TEST_STATE);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn stash_no_enrollments() -> Result<(), AccountManagerError> {
        let manager = StashManager::create(&random_store_id())?;
        assert_eq!(manager.get().await?.as_ref(), &State::NoEnrollments);
        manager.put(State::NoEnrollments).await?;
        assert_eq!(manager.get().await?.as_ref(), &State::NoEnrollments);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn stash_single_enrollment() -> Result<(), AccountManagerError> {
        let manager = StashManager::create(&random_store_id())?;
        manager.put(TEST_STATE.clone()).await?;
        assert_eq!(manager.get().await?.as_ref(), &*TEST_STATE);
        manager.put(State::NoEnrollments).await?;
        assert_eq!(manager.get().await?.as_ref(), &State::NoEnrollments);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn stash_lifecycle() -> Result<(), AccountManagerError> {
        let store_name = random_store_id();
        {
            let manager = StashManager::create(&store_name)?;
            manager.put(TEST_STATE.clone()).await?;
        }
        let manager = StashManager::create(&store_name)?;
        assert_eq!(manager.get().await?.as_ref(), &*TEST_STATE);
        Ok(())
    }

    /// Manually construct an invalid state by directly connecting to Stash.
    #[fasync::run_singlethreaded(test)]
    async fn stash_inconsistent_state() -> Result<(), AccountManagerError> {
        let store_name = random_store_id();
        {
            let store_proxy =
                connect_to_protocol::<StoreMarker>().account_manager_error(ApiError::Resource)?;
            store_proxy.identify(&store_name)?;
            let (accessor_proxy, server_end) = create_proxy()?;
            store_proxy.create_accessor(false, server_end)?;
            accessor_proxy
                .set_value(AUTH_MECHANISM_ID, &mut Value::Stringval(String::from("ignored")))?;
            accessor_proxy.flush().await?.unwrap();
        }

        let manager = StashManager::create(&store_name)?;
        assert_eq!(manager.get().await.unwrap_err().api_error, ApiError::Unknown);

        Ok(())
    }

    /// Manually populate the Stash store with a known key but an invalid type.
    #[fasync::run_singlethreaded(test)]
    async fn stash_type_mismatch() -> Result<(), AccountManagerError> {
        let store_name = random_store_id();
        {
            let store_proxy =
                connect_to_protocol::<StoreMarker>().account_manager_error(ApiError::Resource)?;
            store_proxy.identify(&store_name)?;
            let (accessor_proxy, server_end) = create_proxy()?;
            store_proxy.create_accessor(false, server_end)?;
            accessor_proxy
                .set_value(AUTH_MECHANISM_ID, &mut Value::Stringval(String::from("ignored")))?;
            accessor_proxy
                .set_value(ENROLLMENT_DATA, &mut Value::Stringval(String::from("ignored")))?;
            accessor_proxy.flush().await?.unwrap();
        }
        let manager = StashManager::create(&store_name)?;
        assert_eq!(manager.get().await.unwrap_err().api_error, ApiError::Unknown);
        Ok(())
    }
}
