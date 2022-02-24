// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_identity_authentication::{
    AttemptedEvent, Enrollment, Error as ApiError, StorageUnlockMechanismRequest,
    StorageUnlockMechanismRequestStream,
};
use futures::prelude::*;
use lazy_static::lazy_static;

type EnrollmentData = Vec<u8>;
type PrekeyMaterial = Vec<u8>;

lazy_static! {
    /// The enrollment data always reported by this authenticator.
    static ref FIXED_ENROLLMENT_DATA: Vec<u8> = vec![0, 1, 2];

    /// The magic prekey material corresponding to a successful authentication
    /// attempt with the account system. This constant is copied to
    /// the account_handler implementation and needs to stay in sync.
    static ref MAGIC_PREKEY: Vec<u8>  = vec![77; 32];

    /// Valid prekey material but is not the magic prekey. Should be used to
    /// generate authentication failures.
    static ref NOT_MAGIC_PREKEY: Vec<u8> = vec![80; 32];
}

/// Determines the behavior of the authenticator.
#[derive(Debug, Clone, Copy)]
pub enum Mode {
    /// Enroll returns fixed enrollment data and magic prekey material.
    /// Authenticate ignores enrollment data and returns magic prekey material.
    AlwaysSucceed,

    /// Enroll returns fixed enrollment data and magic prekey material.
    /// Authenticate ignores enrollment data and returns prekey material which
    /// is valid but not equal to the magic prekey.
    AlwaysFailAuthentication,
}

/// A development-only implementation of the
/// fuchsia.identity.authentication.StorageUnlockMechanism fidl protocol
/// that responds according to its `mode`.
pub struct StorageUnlockMechanism {
    mode: Mode,
}

impl StorageUnlockMechanism {
    pub fn new(mode: Mode) -> Self {
        Self { mode }
    }
}

impl StorageUnlockMechanism {
    /// Asynchronously handle fidl requests received on the provided stream.
    pub async fn handle_requests_from_stream(
        &self,
        mut stream: StorageUnlockMechanismRequestStream,
    ) -> Result<(), fidl::Error> {
        while let Some(request) = stream.try_next().await? {
            self.handle_request(request)?;
        }
        Ok(())
    }

    /// Asynchronously handle a fidl request.
    fn handle_request(&self, request: StorageUnlockMechanismRequest) -> Result<(), fidl::Error> {
        match request {
            StorageUnlockMechanismRequest::Authenticate { enrollments, responder } => {
                responder.send(&mut self.authenticate(enrollments))
            }
            StorageUnlockMechanismRequest::Enroll { responder } => {
                responder.send(&mut self.enroll())
            }
        }
    }

    /// Implementation of `authenticate` fidl method.
    fn authenticate(&self, enrollments: Vec<Enrollment>) -> Result<AttemptedEvent, ApiError> {
        let enrollment = enrollments.into_iter().next().ok_or(ApiError::InvalidRequest)?;
        let Enrollment { id, .. } = enrollment;
        let prekey_material = match self.mode {
            Mode::AlwaysSucceed => MAGIC_PREKEY.clone(),
            Mode::AlwaysFailAuthentication => NOT_MAGIC_PREKEY.clone(),
        };
        Ok(AttemptedEvent {
            timestamp: fuchsia_runtime::utc_time().into_nanos(),
            enrollment_id: id,
            updated_enrollment_data: None,
            prekey_material,
        })
    }

    /// Implementation of `enroll` fidl method.
    fn enroll(&self) -> Result<(EnrollmentData, PrekeyMaterial), ApiError> {
        match self.mode {
            Mode::AlwaysSucceed | Mode::AlwaysFailAuthentication => {
                Ok((FIXED_ENROLLMENT_DATA.clone(), MAGIC_PREKEY.clone()))
            }
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_identity_authentication::{
        StorageUnlockMechanismMarker, StorageUnlockMechanismProxy,
    };
    use fuchsia_async as fasync;
    use futures::future::join;

    const TEST_ENROLLMENT_ID: u64 = 0x42;
    const TEST_ENROLLMENT_ID_2: u64 = 0xabba;

    async fn run_proxy_test<Fn, Fut>(mode: Mode, test_fn: Fn)
    where
        Fn: FnOnce(StorageUnlockMechanismProxy) -> Fut,
        Fut: Future<Output = Result<(), fidl::Error>>,
    {
        let (proxy, stream) = create_proxy_and_stream::<StorageUnlockMechanismMarker>().unwrap();
        let mechanism = StorageUnlockMechanism::new(mode);
        let server_fut = mechanism.handle_requests_from_stream(stream);
        let test_fut = test_fn(proxy);

        let (test_result, server_result) = join(test_fut, server_fut).await;
        assert!(test_result.is_ok());
        assert!(server_result.is_ok());
    }

    #[fasync::run_until_stalled(test)]
    async fn always_succeed_enroll_and_authenticate_produce_same_prekey() {
        run_proxy_test(Mode::AlwaysSucceed, |proxy| async move {
            let (enrollment_data, enrollment_prekey) = proxy.enroll().await?.unwrap();

            let enrollment = Enrollment { id: TEST_ENROLLMENT_ID, data: enrollment_data.clone() };

            let AttemptedEvent { enrollment_id, updated_enrollment_data, prekey_material, .. } =
                proxy.authenticate(&mut vec![enrollment].iter_mut()).await?.unwrap();

            assert_eq!(enrollment_id, TEST_ENROLLMENT_ID);
            assert!(updated_enrollment_data.is_none());
            assert_eq!(prekey_material, enrollment_prekey);
            Ok(())
        })
        .await
    }

    #[fasync::run_until_stalled(test)]
    async fn always_succeed_authenticate_multiple_enrollments() {
        run_proxy_test(Mode::AlwaysSucceed, |proxy| async move {
            let enrollment = Enrollment { id: TEST_ENROLLMENT_ID, data: vec![3] };
            let enrollment_2 = Enrollment { id: TEST_ENROLLMENT_ID_2, data: vec![12] };

            let AttemptedEvent { enrollment_id, updated_enrollment_data, prekey_material, .. } =
                proxy.authenticate(&mut vec![enrollment, enrollment_2].iter_mut()).await?.unwrap();

            assert_eq!(enrollment_id, TEST_ENROLLMENT_ID);
            assert!(updated_enrollment_data.is_none());
            assert_eq!(prekey_material, MAGIC_PREKEY.clone());
            Ok(())
        })
        .await
    }

    #[fasync::run_until_stalled(test)]
    async fn always_fail_authentication_enroll_and_authenticate() {
        run_proxy_test(Mode::AlwaysFailAuthentication, |proxy| async move {
            let (enrollment_data, enrollment_prekey) = proxy.enroll().await?.unwrap();

            let enrollment = Enrollment { id: TEST_ENROLLMENT_ID, data: enrollment_data.clone() };
            assert_eq!(enrollment_prekey, MAGIC_PREKEY.clone());

            let AttemptedEvent { enrollment_id, updated_enrollment_data, prekey_material, .. } =
                proxy.authenticate(&mut vec![enrollment].iter_mut()).await?.unwrap();

            assert_ne!(prekey_material, MAGIC_PREKEY.clone());
            assert_eq!(enrollment_id, TEST_ENROLLMENT_ID);
            assert!(updated_enrollment_data.is_none());
            Ok(())
        })
        .await
    }
}
