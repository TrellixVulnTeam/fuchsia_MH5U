// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, format_err, Error};
use fidl::endpoints::create_endpoints;
use fidl_fuchsia_identity_account::{
    AccountManagerMarker, AccountManagerProxy, AccountProxy, Error as ApiError, Lifetime, Scenario,
    ThreatScenario,
};
use fidl_fuchsia_stash::StoreMarker;
use fuchsia_async::{self as fasync, DurationExt, TimeoutExt};
use fuchsia_component::client::{launch, App};
use fuchsia_component::fuchsia_single_component_package_url;
use fuchsia_component::server::{NestedEnvironment, ServiceFs};
use fuchsia_zircon as zx;
use futures::prelude::*;
use lazy_static::lazy_static;
use std::ops::Deref;

/// Type alias for the LocalAccountId FIDL type
type LocalAccountId = u64;

const ALWAYS_SUCCEED_AUTH_MECHANISM_ID: &str =
    "fuchsia-pkg://fuchsia.com/dev_authenticator#meta/dev_authenticator_always_succeed.cmx";

const ALWAYS_FAIL_AUTHENTICATION_AUTH_MECHANISM_ID: &str = concat!(
    "fuchsia-pkg://fuchsia.com/dev_authenticator",
    "#meta/dev_authenticator_always_fail_authentication.cmx"
);

lazy_static! {
    /// URL for account manager.
    static ref ACCOUNT_MANAGER_URL: String =
        String::from(fuchsia_single_component_package_url!("account_manager"));

    /// Arguments passed to account manager started in test environment.
    static ref ACCOUNT_MANAGER_ARGS: Vec<String> = vec![
        "--dev-auth-providers".to_string(),
        "--dev-auth-mechanisms".to_string(),
    ];

    static ref TEST_SCENARIO: Scenario = Scenario {
        include_test: false,
        threat_scenario: ThreatScenario::BasicAttacker,
    };

    /// Maximum time between a lock request and when the account is locked
    static ref LOCK_REQUEST_DURATION: zx::Duration = zx::Duration::from_seconds(5);
}

/// Calls provision_new_account on the supplied account_manager, returning an error on any
/// non-OK responses, or the account ID on success.
async fn provision_new_account(
    account_manager: &AccountManagerProxy,
    lifetime: Lifetime,
    auth_mechanism_id: Option<&str>,
) -> Result<LocalAccountId, Error> {
    account_manager
        .provision_new_account(lifetime, auth_mechanism_id)
        .await?
        .map_err(|error| format_err!("ProvisionNewAccount returned error: {:?}", error))
}

/// A proxy to an account manager running in a nested environment.
struct NestedAccountManagerProxy {
    /// Proxy to account manager.
    account_manager_proxy: AccountManagerProxy,

    /// Application object for account manager.  Needs to be kept in scope to
    /// keep the nested environment alive.
    _app: App,

    /// The nested environment account manager is running in.  Needs to be kept
    /// in scope to keep the nested environment alive.
    _nested_envronment: NestedEnvironment,
}

impl Deref for NestedAccountManagerProxy {
    type Target = AccountManagerProxy;

    fn deref(&self) -> &AccountManagerProxy {
        &self.account_manager_proxy
    }
}

/// Start account manager in an isolated environment and return a proxy to it. An optional
/// environment label can be provided in order to test state preservation across component
/// termination and restart. If env is None a randomized environment label will be picked
/// to provide isolation across tests.
/// NOTE: Do not reuse environment labels across tests. A NestedAccountManagerProxy should be
/// destroyed before a new one referencing the same enviroment is created.
fn create_account_manager(env: Option<String>) -> Result<NestedAccountManagerProxy, Error> {
    let mut service_fs = ServiceFs::new();
    service_fs.add_proxy_service::<StoreMarker, _>();

    let nested_environment = match env {
        None => service_fs.create_salted_nested_environment("account_test_env")?,
        Some(label) => service_fs.create_nested_environment(&label)?,
    };

    let app = launch(
        nested_environment.launcher(),
        ACCOUNT_MANAGER_URL.clone(),
        Some(ACCOUNT_MANAGER_ARGS.clone()),
    )?;
    fasync::Task::spawn(service_fs.collect()).detach();
    let account_manager_proxy = app.connect_to_protocol::<AccountManagerMarker>()?;

    Ok(NestedAccountManagerProxy {
        account_manager_proxy,
        _app: app,
        _nested_envronment: nested_environment,
    })
}

/// Locks an account and waits for the channel to close.
async fn lock_and_check(account: &AccountProxy) -> Result<(), Error> {
    account.lock().await?.map_err(|err| anyhow!("Lock failed: {:?}", err))?;
    account
        .take_event_stream()
        .for_each(|_| async move {}) // Drain
        .map(|_| Ok(())) // Completed drain results in ok
        .on_timeout(LOCK_REQUEST_DURATION.after_now(), || Err(anyhow!("Locking timeout exceeded")))
        .await
}

// TODO(jsankey): Work with ComponentFramework and cramertj@ to develop a nice Rust equivalent of
// the C++ TestWithEnvironment fixture to provide isolated environments for each test case.  We
// are currently creating a new environment for account manager to run in, but the tests
// themselves run in a single environment.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_provision_new_account() -> Result<(), Error> {
    let account_manager = create_account_manager(None)?;

    // Verify we initially have no accounts.
    assert_eq!(account_manager.get_account_ids().await?, vec![]);

    // Provision a new account.
    let account_1 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    assert_eq!(account_manager.get_account_ids().await?, vec![account_1]);

    // Provision a second new account and verify it has a different ID.
    let account_2 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    assert_ne!(account_1, account_2);

    // Provision account with an auth mechanism
    let account_3 = account_manager
        .provision_new_account(Lifetime::Persistent, Some(ALWAYS_SUCCEED_AUTH_MECHANISM_ID))
        .await?
        .unwrap();

    let account_ids = account_manager.get_account_ids().await?;
    assert_eq!(account_ids.len(), 3);
    assert!(account_ids.contains(&account_1));
    assert!(account_ids.contains(&account_2));
    assert!(account_ids.contains(&account_3));

    // Auth state is not yet supported
    assert_eq!(
        account_manager.get_account_auth_states(&mut TEST_SCENARIO.clone()).await?,
        Err(ApiError::UnsupportedOperation)
    );

    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_provision_then_lock_then_unlock_account() -> Result<(), Error> {
    let account_manager = create_account_manager(None)?;

    // Provision account with an auth mechanism that passes authentication
    let account_id = provision_new_account(
        &account_manager,
        Lifetime::Persistent,
        Some(ALWAYS_SUCCEED_AUTH_MECHANISM_ID),
    )
    .await?;
    let (acp_client_end, _) = create_endpoints()?;
    let (account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Ok(())
    );
    let account_proxy = account_client_end.into_proxy()?;

    // Lock the account and ensure that it's locked
    lock_and_check(&account_proxy).await?;

    // Unlock the account and re-acquire a channel
    let (acp_client_end, _) = create_endpoints()?;
    let (account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Ok(())
    );
    let account_proxy = account_client_end.into_proxy()?;
    assert_eq!(account_proxy.get_lifetime().await.unwrap(), Lifetime::Persistent);
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_unlock_account() -> Result<(), Error> {
    let account_manager =
        create_account_manager(Some("test_successful_authentication".to_string()))?;

    // Provision account with an auth mechanism that passes authentication
    let account_id = provision_new_account(
        &account_manager,
        Lifetime::Persistent,
        Some(ALWAYS_SUCCEED_AUTH_MECHANISM_ID),
    )
    .await?;

    // Restart the account manager, now the account should be locked
    std::mem::drop(account_manager);
    let account_manager =
        create_account_manager(Some("test_successful_authentication".to_string()))?;

    // Unlock the account and acquire a channel to it
    let (acp_client_end, _) = create_endpoints()?;
    let (account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Ok(())
    );
    let account_proxy = account_client_end.into_proxy()?;
    assert_eq!(account_proxy.get_lifetime().await.unwrap(), Lifetime::Persistent);
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_provision_then_lock_then_unlock_fail_authentication() -> Result<(), Error> {
    let account_manager = create_account_manager(None)?;

    // Provision account with an auth mechanism that fails authentication
    let account_id = provision_new_account(
        &account_manager,
        Lifetime::Persistent,
        Some(ALWAYS_FAIL_AUTHENTICATION_AUTH_MECHANISM_ID),
    )
    .await?;
    let (acp_client_end, _) = create_endpoints()?;
    let (account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Ok(())
    );
    let account_proxy = account_client_end.into_proxy()?;

    // Lock the account and ensure that it's locked
    lock_and_check(&account_proxy).await?;

    // Attempting to unlock the account fails with an authentication error
    let (acp_client_end, _) = create_endpoints()?;
    let (_, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Err(ApiError::FailedAuthentication)
    );
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_unlock_account_fail_authentication() -> Result<(), Error> {
    let account_manager = create_account_manager(Some("test_fail_authentication".to_string()))?;

    // Provision account with an auth mechanism that fails authentication
    let account_id = provision_new_account(
        &account_manager,
        Lifetime::Persistent,
        Some(ALWAYS_FAIL_AUTHENTICATION_AUTH_MECHANISM_ID),
    )
    .await?;

    // Restart the account manager, now the account should be locked
    std::mem::drop(account_manager);
    let account_manager = create_account_manager(Some("test_fail_authentication".to_string()))?;

    // Attempting to unlock the account fails with an authentication error
    let (acp_client_end, _) = create_endpoints()?;
    let (_, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_id, acp_client_end, account_server_end).await?,
        Err(ApiError::FailedAuthentication)
    );
    Ok(())
}

// This represents two nearly identical tests, one with ephemeral and one with persistent accounts
async fn get_account_and_persona_helper(lifetime: Lifetime) -> Result<(), Error> {
    let account_manager = create_account_manager(None)?;

    assert_eq!(account_manager.get_account_ids().await?, vec![]);

    let account = provision_new_account(&account_manager, lifetime, None).await?;
    // Connect a channel to the newly created account and verify it's usable.
    let (acp_client_end, _) = create_endpoints()?;
    let (account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account, acp_client_end, account_server_end).await?,
        Ok(())
    );
    let account_proxy = account_client_end.into_proxy()?;
    let account_auth_state = account_proxy.get_auth_state(&mut TEST_SCENARIO.clone()).await?;
    assert_eq!(account_auth_state, Err(ApiError::UnsupportedOperation));

    // Cannot lock account not protected by an auth mechanism
    assert_eq!(account_proxy.lock().await?, Err(ApiError::FailedPrecondition));
    assert_eq!(account_proxy.get_lifetime().await?, lifetime);

    // Connect a channel to the account's default persona and verify it's usable.
    let (persona_client_end, persona_server_end) = create_endpoints()?;
    assert!(account_proxy.get_default_persona(persona_server_end).await?.is_ok());
    let persona_proxy = persona_client_end.into_proxy()?;
    let persona_auth_state = persona_proxy.get_auth_state(&mut TEST_SCENARIO.clone()).await?;
    assert_eq!(persona_auth_state, Err(ApiError::UnsupportedOperation));
    assert_eq!(persona_proxy.get_lifetime().await?, lifetime);

    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_get_persistent_account_and_persona() -> Result<(), Error> {
    get_account_and_persona_helper(Lifetime::Persistent).await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_get_ephemeral_account_and_persona() -> Result<(), Error> {
    get_account_and_persona_helper(Lifetime::Ephemeral).await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_account_deletion() -> Result<(), Error> {
    let account_manager = create_account_manager(None)?;

    assert_eq!(account_manager.get_account_ids().await?, vec![]);

    let account_1 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    let account_2 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    let existing_accounts = account_manager.get_account_ids().await?;
    assert!(existing_accounts.contains(&account_1));
    assert!(existing_accounts.contains(&account_2));
    assert_eq!(existing_accounts.len(), 2);

    // Delete an account and verify it is removed.
    assert_eq!(account_manager.remove_account(account_1, true).await?, Ok(()));
    assert_eq!(account_manager.get_account_ids().await?, vec![account_2]);
    // Connecting to the deleted account should fail.
    let (acp_client_end, _acp_server_end) = create_endpoints()?;
    let (_account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_1, acp_client_end, account_server_end).await?,
        Err(ApiError::NotFound)
    );

    Ok(())
}

/// Ensure that an account manager created in a specific environment picks up the state of
/// previous instances that ran in that environment. Also check that some basic operations work on
/// accounts created in that previous lifetime.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_lifecycle() -> Result<(), Error> {
    let account_manager = create_account_manager(Some("test_account_deletion".to_string()))?;

    assert_eq!(account_manager.get_account_ids().await?, vec![]);

    let account_1 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    let account_2 = provision_new_account(&account_manager, Lifetime::Persistent, None).await?;
    let account_3 = provision_new_account(&account_manager, Lifetime::Ephemeral, None).await?;

    let existing_accounts = account_manager.get_account_ids().await?;
    assert_eq!(existing_accounts.len(), 3);

    // Kill and restart account manager in the same environment
    std::mem::drop(account_manager);
    let account_manager = create_account_manager(Some("test_account_deletion".to_string()))?;

    let existing_accounts = account_manager.get_account_ids().await?;
    assert_eq!(existing_accounts.len(), 2); // The ephemeral account was dropped

    // Make sure we can't retrieve the ephemeral account
    let (acp_client_end, _) = create_endpoints()?;
    let (_account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_3, acp_client_end, account_server_end).await?,
        Err(ApiError::NotFound)
    );
    // Retrieve a persistent account that was created in the earlier lifetime
    let (acp_client_end, _) = create_endpoints()?;
    let (_account_client_end, account_server_end) = create_endpoints()?;
    assert_eq!(
        account_manager.get_account(account_1, acp_client_end, account_server_end).await?,
        Ok(())
    );

    // Delete an account and verify it is removed.
    assert_eq!(account_manager.remove_account(account_2, true).await?, Ok(()));
    assert_eq!(account_manager.get_account_ids().await?, vec![account_1]);

    Ok(())
}
