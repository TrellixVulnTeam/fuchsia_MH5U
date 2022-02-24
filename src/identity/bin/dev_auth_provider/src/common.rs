// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon::Duration;
use rand::{
    distributions::{Alphanumeric, DistString as _},
    thread_rng,
};

pub const TOKEN_LIFETIME_SECONDS: u64 = 3600; // one hour lifetime
pub const USER_PROFILE_INFO_ID_DOMAIN: &str = "@example.com";
pub const USER_PROFILE_INFO_DISPLAY_NAME: &str = "test_user_display_name";
pub const USER_PROFILE_INFO_EMAIL: &str = "example@example.com";
pub const USER_PROFILE_INFO_IMAGE_URL: &str = "http://test_user/profile/image/url";
const RANDOM_STRING_LENGTH: usize = 10;

/// Generate random alphanumeric string of fixed length RANDOM_STRING_LENGTH
/// for creating unique tokens or id.
pub fn generate_random_string() -> String {
    Alphanumeric.sample_string(&mut thread_rng(), RANDOM_STRING_LENGTH)
}

/// Calculate expiry time for a token that expires in `TOKEN_LIFETIME_SECONDS`.
pub fn get_token_expiry_time_nanos() -> i64 {
    let expiry_time =
        fuchsia_runtime::utc_time() + Duration::from_seconds(TOKEN_LIFETIME_SECONDS as i64);
    expiry_time.into_nanos()
}
