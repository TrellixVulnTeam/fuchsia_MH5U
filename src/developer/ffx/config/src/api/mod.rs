// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::api::{query::ConfigQuery, value::ConfigValue},
    crate::cache::load_config,
    anyhow::Result,
    serde_json::Value,
    std::convert::{From, TryFrom, TryInto},
    thiserror::Error,
};

pub mod query;
pub mod value;

pub type ConfigResult = Result<ConfigValue>;

#[derive(Debug, Error)]
#[error("Configuraton error")]
pub struct ConfigError(#[from] anyhow::Error);

impl ConfigError {
    pub fn new(e: anyhow::Error) -> Self {
        Self(e)
    }
}

pub(crate) async fn get_config<'a, T: Fn(Value) -> Option<Value>>(
    query: ConfigQuery<'a>,
    mapper: &T,
) -> ConfigResult {
    let config = load_config(&query.build_dir.map(String::from)).await?;
    let read_guard = config.read().await;
    Ok((*read_guard).get(&query, mapper).into())
}

pub(crate) fn validate_type<T>(value: Value) -> Option<Value>
where
    T: TryFrom<ConfigValue>,
    <T as std::convert::TryFrom<ConfigValue>>::Error: std::convert::From<ConfigError>,
{
    let result: std::result::Result<T, T::Error> = ConfigValue(Some(value.clone())).try_into();
    match result {
        Ok(_) => Some(value),
        Err(_) => None,
    }
}

impl From<ConfigError> for std::convert::Infallible {
    fn from(_value: ConfigError) -> Self {
        panic!("never going to happen")
    }
}
