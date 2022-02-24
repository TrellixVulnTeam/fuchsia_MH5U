// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::auth_provider_supplier::AuthProviderSupplier;
use crate::common::AccountLifetime;
use crate::inspect;
use crate::lock_request;
use crate::persona::{Persona, PersonaContext};
use crate::stored_account::StoredAccount;
use crate::TokenManager;
use account_common::{AccountManagerError, FidlPersonaId, PersonaId, ResultExt};
use anyhow::Error;
use fidl::endpoints::{ClientEnd, ServerEnd};
use fidl_fuchsia_auth::AuthenticationContextProviderProxy;
use fidl_fuchsia_identity_account::{
    AccountRequest, AccountRequestStream, AuthChangeGranularity, AuthListenerMarker, AuthState,
    Error as ApiError, Lifetime, PersonaMarker, Scenario,
};
use fidl_fuchsia_identity_internal::AccountHandlerContextProxy;
use fuchsia_inspect::{Node, NumericProperty};
use futures::prelude::*;
use identity_common::{cancel_or, TaskGroup, TaskGroupCancel};
use log::{error, info, warn};
use scopeguard;
use std::fs;
use std::sync::Arc;

/// The file name to use for a token manager database. The location is supplied
/// by `AccountHandlerContext.GetAccountPath()`
const TOKEN_DB: &str = "tokens.json";

/// The context that a particular request to an Account should be executed in, capturing
/// information that was supplied upon creation of the channel.
pub struct AccountContext {
    /// An `AuthenticationContextProviderProxy` capable of generating new `AuthenticationUiContext`
    /// channels.
    pub auth_ui_context_provider: AuthenticationContextProviderProxy,
}

/// Information about the Account that this AccountHandler instance is responsible for.
///
/// This state is only available once the Handler has been initialized to a particular account via
/// the AccountHandlerControl channel.
pub struct Account {
    /// Lifetime for this account.
    lifetime: Arc<AccountLifetime>,

    /// The default persona for this account.
    default_persona: Arc<Persona>,

    /// Collection of tasks that are using this instance.
    task_group: TaskGroup,

    /// A Sender of a lock request.
    lock_request_sender: lock_request::Sender,

    /// Helper for outputting account information via fuchsia_inspect.
    inspect: inspect::Account,
    // TODO(jsankey): Once the system and API surface can support more than a single persona, add
    // additional state here to store these personae. This will most likely be a hashmap from
    // PersonaId to Persona struct, and changing default_persona from a struct to an ID. We
    // will also need to store Arc<TokenManager> at the account level.
}

impl Account {
    /// Manually construct an account object, shouldn't normally be called directly.
    async fn new(
        persona_id: PersonaId,
        lifetime: AccountLifetime,
        context_proxy: AccountHandlerContextProxy,
        lock_request_sender: lock_request::Sender,
        inspect_parent: &Node,
    ) -> Result<Account, AccountManagerError> {
        let task_group = TaskGroup::new();
        let token_manager_task_group = task_group
            .create_child()
            .await
            .map_err(|_| AccountManagerError::new(ApiError::RemovalInProgress))?;
        let default_persona_task_group = task_group
            .create_child()
            .await
            .map_err(|_| AccountManagerError::new(ApiError::RemovalInProgress))?;
        let auth_provider_supplier = AuthProviderSupplier::new(context_proxy);
        let token_manager = Arc::new(match &lifetime {
            AccountLifetime::Ephemeral => {
                TokenManager::new_in_memory(auth_provider_supplier, token_manager_task_group)
            }
            AccountLifetime::Persistent { account_dir } => {
                let token_db_path = account_dir.join(TOKEN_DB);
                TokenManager::new(&token_db_path, auth_provider_supplier, token_manager_task_group)
                    .account_manager_error(ApiError::Unknown)?
            }
        });
        let lifetime = Arc::new(lifetime);
        let account_inspect = inspect::Account::new(inspect_parent);
        Ok(Self {
            lifetime: Arc::clone(&lifetime),
            default_persona: Arc::new(Persona::new(
                persona_id,
                lifetime,
                token_manager,
                default_persona_task_group,
                inspect_parent,
            )),
            task_group,
            lock_request_sender,
            inspect: account_inspect,
        })
    }

    /// Creates a new system account and, if it is persistent, stores it on disk.
    pub async fn create(
        lifetime: AccountLifetime,
        context_proxy: AccountHandlerContextProxy,
        lock_request_sender: lock_request::Sender,
        inspect_parent: &Node,
    ) -> Result<Account, AccountManagerError> {
        let persona_id = PersonaId::new(rand::random::<u64>());
        if let AccountLifetime::Persistent { ref account_dir } = lifetime {
            if StoredAccount::path(account_dir).exists() {
                info!("Attempting to create account twice");
                return Err(AccountManagerError::new(ApiError::Internal));
            }
            let stored_account = StoredAccount::new(persona_id.clone());
            stored_account.save(account_dir)?;
        }
        Self::new(persona_id, lifetime, context_proxy, lock_request_sender, inspect_parent).await
    }

    /// Loads an existing system account from disk.
    pub async fn load(
        lifetime: AccountLifetime,
        context_proxy: AccountHandlerContextProxy,
        lock_request_sender: lock_request::Sender,
        inspect_parent: &Node,
    ) -> Result<Account, AccountManagerError> {
        let account_dir = match lifetime {
            AccountLifetime::Persistent { ref account_dir } => account_dir,
            AccountLifetime::Ephemeral => {
                warn!(concat!(
                    "Attempting to load an ephemeral account from disk. This is not a ",
                    "supported operation."
                ));
                return Err(AccountManagerError::new(ApiError::Internal));
            }
        };
        let stored_account = StoredAccount::load(account_dir)?;
        let persona_id = stored_account.get_default_persona_id().clone();
        Self::new(persona_id, lifetime, context_proxy, lock_request_sender, inspect_parent).await
    }

    /// Removes the account from disk or returns the account and the error.
    pub fn remove(self) -> Result<(), (Self, AccountManagerError)> {
        self.remove_inner().map_err(|err| (self, err))
    }

    /// Removes the account from disk.
    fn remove_inner(&self) -> Result<(), AccountManagerError> {
        match self.lifetime.as_ref() {
            AccountLifetime::Ephemeral => Ok(()),
            AccountLifetime::Persistent { account_dir } => {
                let token_db_path = &account_dir.join(TOKEN_DB);
                if token_db_path.exists() {
                    fs::remove_file(token_db_path).map_err(|err| {
                        warn!("Failed to delete token db: {:?}", err);
                        AccountManagerError::new(ApiError::Resource).with_cause(err)
                    })?;
                }
                let to_remove = StoredAccount::path(&account_dir.clone());
                fs::remove_file(to_remove).map_err(|err| {
                    warn!("Failed to delete account doc: {:?}", err);
                    AccountManagerError::new(ApiError::Resource).with_cause(err)
                })
            }
        }
    }

    /// Returns a task group which can be used to spawn and cancel tasks that use this instance.
    pub fn task_group(&self) -> &TaskGroup {
        &self.task_group
    }

    /// Asynchronously handles the supplied stream of `AccountRequest` messages.
    pub async fn handle_requests_from_stream<'a>(
        &'a self,
        context: &'a AccountContext,
        mut stream: AccountRequestStream,
        cancel: TaskGroupCancel,
    ) -> Result<(), Error> {
        self.inspect.open_client_channels.add(1);
        scopeguard::defer!(self.inspect.open_client_channels.subtract(1));
        while let Some(result) = cancel_or(&cancel, stream.try_next()).await {
            if let Some(request) = result? {
                self.handle_request(context, request).await?;
            } else {
                break;
            }
        }
        Ok(())
    }

    /// Dispatches an `AccountRequest` message to the appropriate handler method
    /// based on its type.
    pub async fn handle_request<'a>(
        &'a self,
        context: &'a AccountContext,
        req: AccountRequest,
    ) -> Result<(), fidl::Error> {
        match req {
            AccountRequest::GetLifetime { responder } => {
                let response = self.get_lifetime();
                responder.send(response)?;
            }
            AccountRequest::GetAuthState { scenario, responder } => {
                let mut response = self.get_auth_state(scenario);
                responder.send(&mut response)?;
            }
            AccountRequest::RegisterAuthListener {
                scenario,
                listener,
                initial_state,
                granularity,
                responder,
            } => {
                let mut response =
                    self.register_auth_listener(scenario, listener, initial_state, granularity);
                responder.send(&mut response)?;
            }
            AccountRequest::GetPersonaIds { responder } => {
                let response = self.get_persona_ids();
                responder.send(&response)?;
            }
            AccountRequest::GetDefaultPersona { persona, responder } => {
                let mut response = self.get_default_persona(context, persona).await;
                responder.send(&mut response)?;
            }
            AccountRequest::GetPersona { id, persona, responder } => {
                let mut response = self.get_persona(context, id.into(), persona).await;
                responder.send(&mut response)?;
            }
            AccountRequest::GetAuthMechanismEnrollments { responder, .. } => {
                responder.send(&mut Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::CreateAuthMechanismEnrollment { responder, .. } => {
                responder.send(&mut Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::RemoveAuthMechanismEnrollment { responder, .. } => {
                responder.send(&mut Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::Lock { responder } => {
                let mut response = self.lock().await;
                responder.send(&mut response)?;
            }
            AccountRequest::GetDataDirectory { responder, .. } => {
                responder.send(&mut Err(ApiError::UnsupportedOperation))?;
            }
        }
        Ok(())
    }

    fn get_lifetime(&self) -> Lifetime {
        Lifetime::from(self.lifetime.as_ref())
    }

    fn get_auth_state(&self, _scenario: Scenario) -> Result<AuthState, ApiError> {
        // TODO(jsankey): Return real authentication state once authenticators exist to create it.
        Err(ApiError::UnsupportedOperation)
    }

    fn register_auth_listener(
        &self,
        _scenario: Scenario,
        _listener: ClientEnd<AuthListenerMarker>,
        _initial_state: bool,
        _granularity: AuthChangeGranularity,
    ) -> Result<(), ApiError> {
        // TODO(jsankey): Implement this method.
        warn!("RegisterAuthListener not yet implemented");
        Err(ApiError::UnsupportedOperation)
    }

    fn get_persona_ids(&self) -> Vec<FidlPersonaId> {
        vec![self.default_persona.id().clone().into()]
    }

    async fn get_default_persona<'a>(
        &'a self,
        context: &'a AccountContext,
        persona_server_end: ServerEnd<PersonaMarker>,
    ) -> Result<FidlPersonaId, ApiError> {
        let persona_clone = Arc::clone(&self.default_persona);
        let persona_context =
            PersonaContext { auth_ui_context_provider: context.auth_ui_context_provider.clone() };
        let stream = persona_server_end.into_stream().map_err(|err| {
            error!("Error opening Persona channel: {:?}", err);
            ApiError::Resource
        })?;
        self.default_persona
            .task_group()
            .spawn(|cancel| async move {
                persona_clone
                    .handle_requests_from_stream(&persona_context, stream, cancel)
                    .await
                    .unwrap_or_else(|e| error!("Error handling Persona channel: {:?}", e))
            })
            .await
            .map_err(|_| ApiError::RemovalInProgress)?;
        Ok(self.default_persona.id().clone().into())
    }

    async fn get_persona<'a>(
        &'a self,
        context: &'a AccountContext,
        id: PersonaId,
        persona_server_end: ServerEnd<PersonaMarker>,
    ) -> Result<(), ApiError> {
        if &id == self.default_persona.id() {
            self.get_default_persona(context, persona_server_end).await.map(|_| ())
        } else {
            warn!("Requested persona does not exist {:?}", id);
            Err(ApiError::NotFound)
        }
    }

    async fn lock(&self) -> Result<(), ApiError> {
        match self.lock_request_sender.send().await {
            Err(lock_request::SendError::NotSupported) => {
                info!("Account lock failure: unsupported account type");
                Err(ApiError::FailedPrecondition)
            }
            Err(lock_request::SendError::UnattendedReceiver) => {
                warn!("Account lock failure: unattended listener");
                Err(ApiError::Internal)
            }
            Err(lock_request::SendError::AlreadySent) => {
                info!("Received account lock request while existing request in progress");
                Ok(())
            }
            Ok(()) => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_util::*;
    use fidl::endpoints::create_endpoints;
    use fidl_fuchsia_auth::AuthenticationContextProviderMarker;
    use fidl_fuchsia_identity_account::{AccountMarker, AccountProxy, Scenario, ThreatScenario};
    use fidl_fuchsia_identity_internal::AccountHandlerContextMarker;
    use fuchsia_async as fasync;
    use fuchsia_inspect::Inspector;
    use futures::channel::oneshot;

    const TEST_SCENARIO: Scenario =
        Scenario { include_test: false, threat_scenario: ThreatScenario::BasicAttacker };

    const TEST_AUTH_MECHANISM_ID: &str = "<AUTH MECHANISM ID>";

    const TEST_ENROLLMENT_ID: u64 = 1337;

    /// Type to hold the common state require during construction of test objects and execution
    /// of a test, including an async executor and a temporary location in the filesystem.
    struct Test {
        location: TempLocation,
    }

    impl Test {
        fn new() -> Test {
            Test { location: TempLocation::new() }
        }

        async fn create_persistent_account(&self) -> Result<Account, AccountManagerError> {
            let inspector = Inspector::new();
            let (account_handler_context_client_end, _) =
                create_endpoints::<AccountHandlerContextMarker>().unwrap();
            let account_dir = self.location.path.clone();
            Account::create(
                AccountLifetime::Persistent { account_dir },
                account_handler_context_client_end.into_proxy().unwrap(),
                lock_request::Sender::NotSupported,
                &inspector.root(),
            )
            .await
        }

        async fn create_ephemeral_account(&self) -> Result<Account, AccountManagerError> {
            let inspector = Inspector::new();
            let (account_handler_context_client_end, _) =
                create_endpoints::<AccountHandlerContextMarker>().unwrap();
            Account::create(
                AccountLifetime::Ephemeral,
                account_handler_context_client_end.into_proxy().unwrap(),
                lock_request::Sender::NotSupported,
                &inspector.root(),
            )
            .await
        }

        async fn load_account(&self) -> Result<Account, AccountManagerError> {
            let inspector = Inspector::new();
            let (account_handler_context_client_end, _) =
                create_endpoints::<AccountHandlerContextMarker>().unwrap();
            Account::load(
                AccountLifetime::Persistent { account_dir: self.location.path.clone() },
                account_handler_context_client_end.into_proxy().unwrap(),
                lock_request::Sender::NotSupported,
                &inspector.root(),
            )
            .await
        }

        async fn create_persistent_account_with_lock_request(
            &self,
        ) -> Result<(Account, oneshot::Receiver<()>), AccountManagerError> {
            let inspector = Inspector::new();
            let (account_handler_context_client_end, _) =
                create_endpoints::<AccountHandlerContextMarker>().unwrap();
            let account_dir = self.location.path.clone();
            let (sender, receiver) = lock_request::channel();
            let account = Account::create(
                AccountLifetime::Persistent { account_dir },
                account_handler_context_client_end.into_proxy().unwrap(),
                sender,
                &inspector.root(),
            )
            .await?;
            Ok((account, receiver))
        }

        async fn run<TestFn, Fut>(&mut self, test_object: Account, test_fn: TestFn)
        where
            TestFn: FnOnce(AccountProxy) -> Fut,
            Fut: Future<Output = Result<(), Error>>,
        {
            let (account_client_end, account_server_end) =
                create_endpoints::<AccountMarker>().unwrap();
            let account_proxy = account_client_end.into_proxy().unwrap();
            let request_stream = account_server_end.into_stream().unwrap();

            let (ui_context_provider_client_end, _) =
                create_endpoints::<AuthenticationContextProviderMarker>().unwrap();
            let context = AccountContext {
                auth_ui_context_provider: ui_context_provider_client_end.into_proxy().unwrap(),
            };

            let task_group = TaskGroup::new();

            task_group
                .spawn(|cancel| async move {
                    test_object
                        .handle_requests_from_stream(&context, request_stream, cancel)
                        .await
                        .unwrap_or_else(|err| {
                            panic!("Fatal error handling test request: {:?}", err)
                        })
                })
                .await
                .expect("Unable to spawn task");
            test_fn(account_proxy).await.expect("Test function failed.")
        }
    }

    #[fasync::run_until_stalled(test)]
    async fn test_random_identifiers() {
        let mut test = Test::new();
        // Generating two accounts with the same accountID should lead to two different persona IDs
        let account_1 = test.create_persistent_account().await.unwrap();
        test.location = TempLocation::new();
        let account_2 = test.create_persistent_account().await.unwrap();
        assert_ne!(account_1.default_persona.id(), account_2.default_persona.id());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_ephemeral() {
        let mut test = Test::new();
        test.run(test.create_ephemeral_account().await.unwrap(), |proxy| async move {
            assert_eq!(proxy.get_lifetime().await?, Lifetime::Ephemeral);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_persistent() {
        let mut test = Test::new();
        test.run(test.create_persistent_account().await.unwrap(), |proxy| async move {
            assert_eq!(proxy.get_lifetime().await?, Lifetime::Persistent);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_create_and_load() {
        let test = Test::new();
        // Persists the account on disk
        let account_1 = test.create_persistent_account().await.unwrap();
        // Reads from same location
        let account_2 = test.load_account().await.unwrap();

        // Since persona ids are random, we can check that loading worked successfully here
        assert_eq!(account_1.default_persona.id(), account_2.default_persona.id());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_load_non_existing() {
        let test = Test::new();
        assert!(test.load_account().await.is_err()); // Reads from uninitialized location
    }

    /// Attempting to load an ephemeral account fails.
    #[fasync::run_until_stalled(test)]
    async fn test_load_ephemeral() {
        let inspector = Inspector::new();
        let (account_handler_context_client_end, _) =
            create_endpoints::<AccountHandlerContextMarker>().unwrap();
        assert!(Account::load(
            AccountLifetime::Ephemeral,
            account_handler_context_client_end.into_proxy().unwrap(),
            lock_request::Sender::NotSupported,
            &inspector.root(),
        )
        .await
        .is_err());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_create_twice() {
        let test = Test::new();
        assert!(test.create_persistent_account().await.is_ok());
        assert!(test.create_persistent_account().await.is_err()); // Tries to write to same dir
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_auth_state() {
        let mut test = Test::new();
        test.run(test.create_persistent_account().await.unwrap(), |proxy| async move {
            assert_eq!(
                proxy.get_auth_state(&mut TEST_SCENARIO.clone()).await?,
                Err(ApiError::UnsupportedOperation)
            );
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_register_auth_listener() {
        let mut test = Test::new();
        test.run(test.create_persistent_account().await.unwrap(), |proxy| {
            async move {
                let (auth_listener_client_end, _) = create_endpoints().unwrap();
                assert_eq!(
                    proxy
                        .register_auth_listener(
                            &mut TEST_SCENARIO.clone(),
                            auth_listener_client_end,
                            true, /* include initial state */
                            &mut AuthChangeGranularity {
                                presence_changes: false,
                                engagement_changes: false,
                                summary_changes: true,
                            }
                        )
                        .await?,
                    Err(ApiError::UnsupportedOperation)
                );
                Ok(())
            }
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_ids() {
        let mut test = Test::new();
        // Note: Persona ID is random. Record the persona_id before starting the test.
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = &account.default_persona.id().clone();

        test.run(account, |proxy| async move {
            let response = proxy.get_persona_ids().await?;
            assert_eq!(response.len(), 1);
            assert_eq!(&PersonaId::new(response[0]), persona_id);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_default_persona() {
        let mut test = Test::new();
        // Note: Persona ID is random. Record the persona_id before starting the test.
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = &account.default_persona.id().clone();

        test.run(account, |account_proxy| {
            async move {
                let (persona_client_end, persona_server_end) = create_endpoints().unwrap();
                let response = account_proxy.get_default_persona(persona_server_end).await?;
                assert_eq!(&PersonaId::from(response.unwrap()), persona_id);

                // The persona channel should now be usable.
                let persona_proxy = persona_client_end.into_proxy().unwrap();
                assert_eq!(
                    persona_proxy.get_auth_state(&mut TEST_SCENARIO.clone()).await?,
                    Err(ApiError::UnsupportedOperation)
                );
                assert_eq!(persona_proxy.get_lifetime().await?, Lifetime::Persistent);

                Ok(())
            }
        })
        .await;
    }

    /// When an ephemeral account is created, its default persona is also ephemeral.
    #[fasync::run_until_stalled(test)]
    async fn test_ephemeral_account_has_ephemeral_persona() {
        let mut test = Test::new();
        let account = test.create_ephemeral_account().await.unwrap();
        test.run(account, |account_proxy| async move {
            let (persona_client_end, persona_server_end) = create_endpoints().unwrap();
            assert!(account_proxy.get_default_persona(persona_server_end).await?.is_ok());
            let persona_proxy = persona_client_end.into_proxy().unwrap();

            assert_eq!(persona_proxy.get_lifetime().await?, Lifetime::Ephemeral);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_by_correct_id() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = account.default_persona.id().clone();

        test.run(account, |account_proxy| {
            async move {
                let (persona_client_end, persona_server_end) = create_endpoints().unwrap();
                assert!(account_proxy
                    .get_persona(FidlPersonaId::from(persona_id), persona_server_end)
                    .await?
                    .is_ok());

                // The persona channel should now be usable.
                let persona_proxy = persona_client_end.into_proxy().unwrap();
                assert_eq!(
                    persona_proxy.get_auth_state(&mut TEST_SCENARIO.clone()).await?,
                    Err(ApiError::UnsupportedOperation)
                );

                Ok(())
            }
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_by_incorrect_id() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        // Note: This fixed value has a 1 - 2^64 probability of not matching the randomly chosen
        // one.
        let wrong_id = PersonaId::new(13);

        test.run(account, |proxy| async move {
            let (_, persona_server_end) = create_endpoints().unwrap();
            assert_eq!(
                proxy.get_persona(wrong_id.into(), persona_server_end).await?,
                Err(ApiError::NotFound)
            );

            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_auth_mechanisms() {
        let mut test = Test::new();
        test.run(test.create_persistent_account().await.unwrap(), |proxy| async move {
            assert_eq!(
                proxy.get_auth_mechanism_enrollments().await?,
                Err(ApiError::UnsupportedOperation)
            );
            assert_eq!(
                proxy.create_auth_mechanism_enrollment(TEST_AUTH_MECHANISM_ID).await?,
                Err(ApiError::UnsupportedOperation)
            );
            assert_eq!(
                proxy.remove_auth_mechanism_enrollment(TEST_ENROLLMENT_ID).await?,
                Err(ApiError::UnsupportedOperation)
            );
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_lock() {
        let mut test = Test::new();
        let (account, mut receiver) =
            test.create_persistent_account_with_lock_request().await.unwrap();
        test.run(account, |proxy| async move {
            assert_eq!(receiver.try_recv(), Ok(None));
            assert_eq!(proxy.lock().await?, Ok(()));
            assert_eq!(receiver.await, Ok(()));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_lock_not_supported() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        test.run(account, |proxy| async move {
            assert_eq!(proxy.lock().await?, Err(ApiError::FailedPrecondition));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_lock_unattended_receiver() {
        let mut test = Test::new();
        let (account, receiver) = test.create_persistent_account_with_lock_request().await.unwrap();
        std::mem::drop(receiver);
        test.run(account, |proxy| async move {
            assert_eq!(proxy.lock().await?, Err(ApiError::Internal));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_lock_twice() {
        let mut test = Test::new();
        let (account, _receiver) =
            test.create_persistent_account_with_lock_request().await.unwrap();
        test.run(account, |proxy| async move {
            assert_eq!(proxy.lock().await?, Ok(()));
            assert_eq!(proxy.lock().await?, Ok(()));
            Ok(())
        })
        .await;
    }
}
