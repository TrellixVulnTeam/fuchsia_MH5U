// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_developer_bridge::VersionInfo;
use std::ffi::CString;

const fn default_version_value() -> [u8; 64] {
    // Placeholder value to be replaced post-link: "V3RS1ONS" * 8
    [
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
        'V' as u8, '3' as u8, 'R' as u8, 'S' as u8, '1' as u8, 'O' as u8, 'N' as u8, 'S' as u8,
    ]
}

const fn default_build_value() -> [u8; 64] {
    // Placeholder value to be replaced post-link: "BU1LDV3R" * 8
    [
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
        'B' as u8, 'U' as u8, '1' as u8, 'L' as u8, 'D' as u8, 'V' as u8, '3' as u8, 'R' as u8,
    ]
}

#[cfg(target_os = "macos")]
#[used]
#[no_mangle]
// mach-o section specifiers require a segment and section separated by a comma.
#[link_section = ".FFX_VERSION,.ffx_version"]
static VERSION_INFO: [u8; 64] = default_version_value();

#[cfg(not(target_os = "macos"))]
#[used]
#[no_mangle]
#[link_section = ".ffx_version"]
static VERSION_INFO: [u8; 64] = default_version_value();

#[cfg(target_os = "macos")]
#[used]
#[no_mangle]
// mach-o section specifiers require a segment and section separated by a comma.
#[link_section = ".FFX_BUILD,.ffx_build"]
static BUILD_VERSION: [u8; 64] = default_build_value();

#[cfg(not(target_os = "macos"))]
#[used]
#[no_mangle]
#[link_section = ".ffx_build"]
static BUILD_VERSION: [u8; 64] = default_build_value();

pub fn build_info() -> VersionInfo {
    let null_char = |b: &u8| *b == 0;
    let version_info =
        &VERSION_INFO[..VERSION_INFO.iter().position(null_char).unwrap_or(VERSION_INFO.len())];
    let build_version =
        &BUILD_VERSION[..BUILD_VERSION.iter().position(null_char).unwrap_or(BUILD_VERSION.len())];
    build_info_impl(
        CString::new(version_info)
            .expect("ffx build error: invalid version string format embedded")
            .to_string_lossy()
            .trim()
            .to_string(),
        CString::new(build_version)
            .expect("ffx build error: invalid version string format embedded")
            .to_string_lossy()
            .trim()
            .to_string(),
    )
}

fn build_info_impl(raw_version_info: String, raw_build_version: String) -> VersionInfo {
    let split: Vec<&str> = raw_version_info.split("-").collect();
    if split.len() != 2 {
        return VersionInfo { build_version: Some(raw_build_version), ..VersionInfo::EMPTY };
    }

    let raw_hash = split.get(0).unwrap().to_string();
    let hash_opt = if raw_hash.is_empty() { None } else { Some(raw_hash) };
    let timestamp_str = split.get(1).unwrap();
    let timestamp = timestamp_str.parse::<u64>().ok();

    return VersionInfo {
        commit_hash: hash_opt,
        commit_timestamp: timestamp,
        build_version: Some(raw_build_version.trim().to_string()),
        ..VersionInfo::EMPTY
    };
}

#[cfg(test)]
mod test {
    use super::*;

    const HASH: &str = "hashyhashhash";
    const TIMESTAMP: u64 = 12345689;
    const FAKE_BUILD_VERSION: &str = "20201118";

    #[test]
    fn test_valid_string_dirty() {
        let s = format!("{}-{}", HASH, TIMESTAMP);
        let result = build_info_impl(s, FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: Some(HASH.to_string()),
                commit_timestamp: Some(TIMESTAMP),
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }

    #[test]
    fn test_valid_string_clean() {
        let s = format!("{}-{}", HASH, TIMESTAMP);
        let result = build_info_impl(s, FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: Some(HASH.to_string()),
                commit_timestamp: Some(TIMESTAMP),
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }

    #[test]
    fn test_invalid_string_empty() {
        let result = build_info_impl(String::default(), FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: None,
                commit_timestamp: None,
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }

    #[test]
    fn test_invalid_string_empty_with_hyphens() {
        let result = build_info_impl("--".to_string(), FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: None,
                commit_timestamp: None,
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }

    #[test]
    fn test_invalid_string_clean_missing_hash() {
        let result = build_info_impl(format!("-{}", TIMESTAMP), FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: None,
                commit_timestamp: Some(TIMESTAMP),
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }

    #[test]
    fn test_invalid_string_clean_missing_hash_and_timestamp() {
        let result = build_info_impl("--".to_string(), FAKE_BUILD_VERSION.to_string());

        assert_eq!(
            result,
            VersionInfo {
                commit_hash: None,
                commit_timestamp: None,
                build_version: Some(FAKE_BUILD_VERSION.to_string()),
                ..VersionInfo::EMPTY
            }
        );
    }
}
