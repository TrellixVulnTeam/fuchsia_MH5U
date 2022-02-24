// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::common::{
    generate_random_string, get_token_expiry_time_nanos, USER_PROFILE_INFO_DISPLAY_NAME,
    USER_PROFILE_INFO_EMAIL, USER_PROFILE_INFO_IMAGE_URL,
};
use crate::oauth::AccessTokenContent;
use fidl_fuchsia_identity_external::{
    Error as ApiError, OauthOpenIdConnectRequest, OauthOpenIdConnectRequestStream,
    OpenIdTokenFromOauthRefreshTokenRequest, OpenIdUserInfoFromOauthAccessTokenRequest,
};
use fidl_fuchsia_identity_tokens::{OpenIdToken, OpenIdUserInfo};
use futures::future;
use futures::prelude::*;
use log::warn;

/// An implementation of the `OauthOpenIdConnect` protocol for testing.
pub struct OauthOpenIdConnect {}

impl OauthOpenIdConnect {
    /// Handles requests received on the provided `request_stream`.
    pub async fn handle_requests_for_stream(request_stream: OauthOpenIdConnectRequestStream) {
        request_stream
            .try_for_each(|r| future::ready(Self::handle_request(r)))
            .unwrap_or_else(|e| warn!("Error running AuthProvider{:?}", e))
            .await;
    }

    /// Handles a single `OauthOpenIdConnectRequest`.
    fn handle_request(request: OauthOpenIdConnectRequest) -> Result<(), fidl::Error> {
        match request {
            OauthOpenIdConnectRequest::GetIdTokenFromRefreshToken { request, responder } => {
                responder.send(&mut Self::get_id_token_from_refresh_token(request))
            }
            OauthOpenIdConnectRequest::GetUserInfoFromAccessToken { request, responder } => {
                responder.send(&mut Self::get_user_info_from_access_token(request))
            }
        }
    }

    fn get_id_token_from_refresh_token(
        request: OpenIdTokenFromOauthRefreshTokenRequest,
    ) -> Result<OpenIdToken, ApiError> {
        let OpenIdTokenFromOauthRefreshTokenRequest { refresh_token, audiences: _, .. } = request;
        let refresh_token = refresh_token.ok_or(ApiError::InvalidRequest)?;
        let refresh_token_content = refresh_token.content.ok_or(ApiError::InvalidRequest)?;

        Ok(OpenIdToken {
            content: Some(format!("{}:idt_{}", refresh_token_content, generate_random_string())),
            expiry_time: Some(get_token_expiry_time_nanos()),
            ..OpenIdToken::EMPTY
        })
    }

    fn get_user_info_from_access_token(
        request: OpenIdUserInfoFromOauthAccessTokenRequest,
    ) -> Result<OpenIdUserInfo, ApiError> {
        let access_token_content = request
            .access_token
            .ok_or(ApiError::InvalidRequest)?
            .content
            .ok_or(ApiError::InvalidRequest)?;

        let subject = serde_json::from_str::<AccessTokenContent>(&access_token_content)
            .map_err(|_| ApiError::InvalidRequest)?
            .account_id;

        Ok(OpenIdUserInfo {
            subject: Some(subject),
            name: Some(USER_PROFILE_INFO_DISPLAY_NAME.to_string()),
            email: Some(USER_PROFILE_INFO_EMAIL.to_string()),
            picture: Some(USER_PROFILE_INFO_IMAGE_URL.to_string()),
            ..OpenIdUserInfo::EMPTY
        })
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_identity_external::{OauthOpenIdConnectMarker, OauthOpenIdConnectProxy};
    use fidl_fuchsia_identity_tokens::{OauthAccessToken, OauthRefreshToken};
    use fuchsia_async as fasync;
    use futures::future::join;

    async fn run_proxy_test<F, Fut>(test_fn: F)
    where
        F: FnOnce(OauthOpenIdConnectProxy) -> Fut,
        Fut: Future<Output = Result<(), anyhow::Error>>,
    {
        let (proxy, stream) = create_proxy_and_stream::<OauthOpenIdConnectMarker>().unwrap();
        let server_fut = OauthOpenIdConnect::handle_requests_for_stream(stream);
        let (test_result, _) = join(test_fn(proxy), server_fut).await;
        assert!(test_result.is_ok());
    }

    #[fasync::run_until_stalled(test)]
    async fn get_id_token_from_refresh_token_test() {
        run_proxy_test(|proxy| async move {
            let refresh_token_content = format!("rt_{}", generate_random_string());
            let refresh_token = OauthRefreshToken {
                content: Some(refresh_token_content.clone()),
                account_id: Some("account_id".to_string()),
                ..OauthRefreshToken::EMPTY
            };
            let request = OpenIdTokenFromOauthRefreshTokenRequest {
                refresh_token: Some(refresh_token),
                audiences: None,
                ..OpenIdTokenFromOauthRefreshTokenRequest::EMPTY
            };
            let id_token = proxy.get_id_token_from_refresh_token(request).await?.unwrap();
            assert!(id_token.content.unwrap().contains(&refresh_token_content));
            assert!(id_token.expiry_time.is_some());
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn get_user_info_from_access_token_test() {
        run_proxy_test(|proxy| async move {
            let access_token_content = serde_json::to_string(&AccessTokenContent::new(
                "rt_token".to_string(),
                "test-account@example.com".to_string(),
                None,
            ))
            .unwrap();

            let user_info = proxy
                .get_user_info_from_access_token(OpenIdUserInfoFromOauthAccessTokenRequest {
                    access_token: Some(OauthAccessToken {
                        content: Some(access_token_content),
                        expiry_time: None,
                        ..OauthAccessToken::EMPTY
                    }),
                    ..OpenIdUserInfoFromOauthAccessTokenRequest::EMPTY
                })
                .await?
                .unwrap();
            assert_eq!(user_info.subject.unwrap(), "test-account@example.com".to_string());
            assert!(user_info.name.is_some());
            assert!(user_info.email.is_some());
            assert!(user_info.picture.is_some());
            Ok(())
        })
        .await;
    }
}
