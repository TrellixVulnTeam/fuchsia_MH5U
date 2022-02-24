// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOTE: The comments that have [START/END ...] in them are used to identify code snippets that
// appear in the documentation.  Please be aware that changes in these blocks will affect the
// documentation on fuchsia.dev.
// You can find the usages in the documenation by running:
//  find docs -name "*.md" | xargs \
//  grep "\{% includecode .*gerrit_path=\"examples/hello_world/rust/src/main.rs\""

// [START main]
fn main() {
    println!("{}, world!", greeting());
    eprintln!("{}, world!", greeting());
}

fn greeting() -> String {
    return String::from("Hello");
}
// [END main]

// [START test_mod]
#[cfg(test)]
mod tests {
    #[fuchsia::test]
    fn it_works() {
        assert_eq!(true, true);
    }
}
// [END test_mod]

#[cfg(test)]
mod hello_tests {
    use crate::greeting;

    // [START async_test]
    #[fuchsia::test]
    async fn my_test() {
        let some_future = async { 4 };
        assert_eq!(some_future.await, 4);
    }
    // [END async_test]

    #[fuchsia::test]
    fn greeting_test() {
        let expected = String::from("Hello");
        assert_eq!(greeting(), expected)
    }
}
