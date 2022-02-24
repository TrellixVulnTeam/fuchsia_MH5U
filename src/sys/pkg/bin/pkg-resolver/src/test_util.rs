// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    serde::Serialize,
    serde_json,
    std::{
        fs::File,
        io::{self, Write as _},
        str,
    },
    tempfile::{self, TempDir},
};

pub(crate) fn create_dir<'a, T, S>(iter: T) -> TempDir
where
    T: IntoIterator<Item = (&'a str, S)>,
    S: Serialize,
{
    let dir = tempfile::tempdir().unwrap();

    for (name, config) in iter {
        let path = dir.path().join(name);
        let mut f = io::BufWriter::new(File::create(path).unwrap());
        serde_json::to_writer(&mut f, &config).unwrap();
        f.flush().unwrap();
    }

    dir
}
