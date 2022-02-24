// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(dead_code)]

/* automatically generated by rust-bindgen */

pub const TA_KEYSAFE_CMD_GENERATE_KEY: u32 = 0;
pub const TA_KEYSAFE_CMD_GET_PUBLIC_KEY: u32 = 1;
pub const TA_KEYSAFE_CMD_ENCRYPT_DATA: u32 = 2;
pub const TA_KEYSAFE_CMD_DECRYPT_DATA: u32 = 3;
pub const TA_KEYSAFE_CMD_SIGN: u32 = 4;
pub const TA_KEYSAFE_CMD_GET_HARDWARE_DERIVED_KEY: u32 = 5;
pub const TA_KEYSAFE_CMD_PARSE_KEY: u32 = 6;
pub const TA_KEYSAFE_CMD_IMPORT_KEY: u32 = 7;
pub const TA_KEYSAFE_ALG_RSA_SSA_PSS_SHA256_2048: u32 = 1;
pub const TA_KEYSAFE_ALG_RSA_SSA_PSS_SHA256_3072: u32 = 2;
pub const TA_KEYSAFE_ALG_RSA_SSA_PSS_SHA512_4096: u32 = 3;
pub const TA_KEYSAFE_ALG_RSA_SSA_PKCS1_SHA256_2048: u32 = 4;
pub const TA_KEYSAFE_ALG_RSA_SSA_PKCS1_SHA256_3072: u32 = 5;
pub const TA_KEYSAFE_ALG_RSA_SSA_PKCS1_SHA512_4096: u32 = 6;
pub const TA_KEYSAFE_ALG_ECDSA_SHA256_P256: u32 = 7;
pub const TA_KEYSAFE_ALG_ECDSA_SHA512_P384: u32 = 8;
pub const TA_KEYSAFE_ALG_ECDSA_SHA512_P521: u32 = 9;
pub const TA_KEYSAFE_ALG_AES_GCM_256: u32 = 10;
pub const TA_KEYSAFE_WRAPPING_KEY_SIZE_BITS: u32 = 128;
pub const TA_KEYSAFE_AES_GCM_TAG_LEN_BYTES: u32 = 16;
pub const TA_KEYSAFE_AES_GCM_IV_LEN_BYTES: u32 = 12;