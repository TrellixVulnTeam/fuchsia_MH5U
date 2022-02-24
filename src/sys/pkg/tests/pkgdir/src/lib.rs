// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::create_proxy,
    fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy},
    fidl_test_fidl_pkg::{Backing, HarnessMarker},
    fuchsia_component::client::connect_to_protocol,
    std::fmt::Debug,
};

mod directory;
mod file;
mod node;

fn repeat_by_n(seed: char, n: usize) -> String {
    std::iter::repeat(seed).take(n).collect()
}

async fn dirs_to_test() -> impl Iterator<Item = PackageSource> {
    let proxy = connect_to_protocol::<HarnessMarker>().unwrap();
    let connect = |backing| {
        let proxy = Clone::clone(&proxy);
        async move {
            let (dir, server) = create_proxy::<DirectoryMarker>().unwrap();
            let () = proxy.connect_package(backing, server).await.unwrap().unwrap();
            PackageSource { dir, backing }
        }
    };
    IntoIterator::into_iter([connect(Backing::Pkgfs).await, connect(Backing::Pkgdir).await])
}

// TODO(fxbug.dev/75481): migrate all callers to use `dirs_to_test`
async fn just_pkgfs_for_now() -> impl Iterator<Item = PackageSource> {
    dirs_to_test().await.filter(|source| source.is_pkgfs())
}

struct PackageSource {
    backing: Backing,
    dir: DirectoryProxy,
}
impl PackageSource {
    #[allow(dead_code)]
    fn is_pkgfs(&self) -> bool {
        self.backing == Backing::Pkgfs
    }

    #[allow(dead_code)]
    fn is_pkgdir(&self) -> bool {
        self.backing == Backing::Pkgdir
    }
}

macro_rules! flag_list {
    [$($flag:ident),* $(,)?] => {
        [
            $((fidl_fuchsia_io::$flag, stringify!($flag))),*
        ]
    };
}

// flags in same order as they appear in fuchsia.io in an attempt to make it easier
// to keep this list up to date. Although if this list gets out of date it's
// not the end of the world, the debug printer just won't know how to decode
// them and will hex format the not-decoded flags.
const OPEN_FLAGS: &[(u32, &str)] = &flag_list![
    OPEN_RIGHT_EXECUTABLE,
    OPEN_RIGHT_READABLE,
    OPEN_RIGHT_WRITABLE,
    OPEN_FLAG_CREATE,
    OPEN_FLAG_CREATE_IF_ABSENT,
    OPEN_FLAG_TRUNCATE,
    OPEN_FLAG_DIRECTORY,
    OPEN_FLAG_APPEND,
    OPEN_FLAG_NO_REMOTE,
    OPEN_FLAG_NODE_REFERENCE,
    OPEN_FLAG_DESCRIBE,
    OPEN_FLAG_POSIX_WRITABLE,
    OPEN_FLAG_POSIX_EXECUTABLE,
    OPEN_FLAG_NOT_DIRECTORY,
    CLONE_FLAG_SAME_RIGHTS,
];

#[derive(PartialEq, Eq)]
struct OpenFlags(u32);

impl Debug for OpenFlags {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut flags = self.0;
        let flag_strings = OPEN_FLAGS.iter().filter_map(|&flag| {
            if self.0 & flag.0 == flag.0 {
                flags &= !flag.0;
                Some(flag.1)
            } else {
                None
            }
        });
        let mut first = true;
        for flag in flag_strings {
            if !first {
                write!(f, " | ")?;
            }
            first = false;
            write!(f, "{}", flag)?;
        }
        if flags != 0 {
            if !first {
                write!(f, " | ")?;
            }
            first = false;
            write!(f, "{:#x}", flags)?;
        }
        if first {
            write!(f, "0")?;
        }

        Ok(())
    }
}

// modes in same order as they appear in fuchsia.io in an attempt to make it
// easier to keep this list up to date. Although if this list gets out of date
// it's not the end of the world, the debug printer just won't know how to
// decode them and will octal format the not-decoded flags.
const MODE_TYPES: &[(u32, &str)] = &flag_list![
    MODE_TYPE_DIRECTORY,
    MODE_TYPE_BLOCK_DEVICE,
    MODE_TYPE_FILE,
    MODE_TYPE_SOCKET,
    MODE_TYPE_SERVICE,
];

#[derive(PartialEq, Eq)]
struct Mode(u32);

impl Debug for Mode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut flags = self.0;
        let flag_strings = MODE_TYPES.iter().filter_map(|&flag| {
            if self.0 & flag.0 == flag.0 {
                flags &= !flag.0;
                Some(flag.1)
            } else {
                None
            }
        });
        let mut first = true;
        for flag in flag_strings {
            if !first {
                write!(f, " | ")?;
            }
            first = false;
            write!(f, "{}", flag)?;
        }
        if flags != 0 {
            if !first {
                write!(f, " | ")?;
            }
            first = false;
            write!(f, "{:#o}", flags)?;
        }
        if first {
            write!(f, "0")?;
        }

        Ok(())
    }
}
