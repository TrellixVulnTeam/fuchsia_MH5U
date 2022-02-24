// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    regex::{Captures, Regex},
    serde_json::Value,
    std::path::PathBuf,
};

pub(crate) mod cache;
pub(crate) mod config;
pub(crate) mod data;
pub(crate) mod env_var;
pub(crate) mod file_check;
pub(crate) mod filter;
pub(crate) mod flatten;
pub(crate) mod home;
pub(crate) mod identity;
pub(crate) mod runtime;

// Negative lookbehind (or lookahead for that matter) is not supported in Rust's regex.
// Instead, replace with this string - which hopefully will not be used by anyone in the
// configuration.  Insert joke here about how hope is not a strategy.
const TEMP_REPLACE: &str = "#<#ffx!!replace#>#";

pub(crate) fn preprocess(value: &Value) -> Option<String> {
    value.as_str().map(|s| s.to_string()).map(|s| s.replace("$$", TEMP_REPLACE))
}

pub(crate) fn postprocess(value: String) -> Value {
    Value::String(value.to_string().replace(TEMP_REPLACE, "$"))
}

pub(crate) fn replace_regex<T>(value: &String, regex: &Regex, replacer: T) -> String
where
    T: Fn(&str) -> Result<String>,
{
    regex
        .replace_all(value, |caps: &Captures<'_>| {
            // Skip the first one since that'll be the whole string.
            caps.iter()
                .skip(1)
                .map(|cap| cap.map(|c| replacer(c.as_str())))
                .fold(String::new(), |acc, v| if let Some(Ok(s)) = v { acc + &s } else { acc })
        })
        .into_owned()
}

pub(crate) fn replace<'a, T, P>(
    regex: &'a Regex,
    base_path: P,
    next: &'a T,
) -> Box<dyn Fn(Value) -> Option<Value> + Send + Sync + 'a>
where
    T: Fn(Value) -> Option<Value> + Sync,
    P: Fn() -> Result<PathBuf> + Sync + Send + 'a,
{
    Box::new(move |value| -> Option<Value> {
        match preprocess(&value)
            .as_ref()
            .map(|s| {
                replace_regex(s, regex, |v| {
                    match base_path() {
                        Ok(p) => Ok(p.to_str().map_or(v.to_string(), |s| s.to_string())),
                        Err(_) => Ok(v.to_string()), //just pass through
                    }
                })
            })
            .map(postprocess)
        {
            Some(v) => next(v),
            None => next(value),
        }
    })
}
