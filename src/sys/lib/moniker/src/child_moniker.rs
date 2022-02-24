// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::error::MonikerError,
    cm_types::Name,
    core::cmp::{Ord, Ordering},
    std::{fmt, str::FromStr},
};

#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

pub trait ChildMonikerBase: Eq + PartialOrd + Clone + Default + fmt::Display {
    fn parse<T: AsRef<str>>(rep: T) -> Result<Self, MonikerError>
    where
        Self: Sized;

    fn name(&self) -> &str;

    fn collection(&self) -> Option<&str>;

    fn as_str(&self) -> &str;
}

/// Validates that the given string is valid as the instance or collection name in a moniker.
// TODO(fxbug.dev/77563): The moniker types should be updated to use Name directly instead of String
// so that it is clear what is validated and what isn't.
pub fn validate_moniker_part(name: Option<&str>) -> Result<(), MonikerError> {
    // Reuse the validation in cm_types::Name for consistency.
    name.map(|n| Name::from_str(n).map_err(|_| MonikerError::invalid_moniker_part(n)))
        .transpose()?;
    Ok(())
}

/// An child moniker locally identifies a child component instance using the name assigned by
/// its parent and its collection (if present). It is a building block for more complex monikers.
///
/// The child moniker does not distinguish between instances.
///
/// Display notation: "name[:collection]".
#[cfg_attr(feature = "serde", derive(Deserialize, Serialize))]
#[derive(Eq, PartialEq, Debug, Clone, Hash, Default)]
pub struct ChildMoniker {
    pub name: String,
    pub collection: Option<String>,
    rep: String,
}

impl ChildMonikerBase for ChildMoniker {
    /// Parses a `ChildMoniker` from a string.
    ///
    /// Input strings should be of the format `<name>(:<collection>)?`, e.g. `foo` or `biz:foo`.
    fn parse<T: AsRef<str>>(rep: T) -> Result<Self, MonikerError> {
        let rep = rep.as_ref();
        let mut parts = rep.split(":").fuse();
        let invalid = || MonikerError::invalid_moniker(rep);
        let first = parts.next().ok_or_else(invalid)?;
        let second = parts.next();
        if parts.next().is_some() || first.len() == 0 || second.map_or(false, |s| s.len() == 0) {
            return Err(invalid());
        }
        let (name, coll) = match second {
            Some(s) => (s, Some(first.to_string())),
            None => (first, None),
        };

        validate_moniker_part(Some(&name))?;
        validate_moniker_part(coll.as_deref())?;
        Ok(ChildMoniker::new(name.to_string(), coll))
    }

    fn name(&self) -> &str {
        &self.name
    }

    fn collection(&self) -> Option<&str> {
        self.collection.as_ref().map(|s| &**s)
    }

    fn as_str(&self) -> &str {
        &self.rep
    }
}

impl ChildMoniker {
    // TODO(fxbug.dev/77563): This does not currently validate the String inputs.
    pub fn new(name: String, collection: Option<String>) -> Self {
        assert!(!name.is_empty());
        let rep = if let Some(c) = collection.as_ref() {
            assert!(!c.is_empty());
            format!("{}:{}", c, name)
        } else {
            name.clone()
        };
        ChildMoniker { name, collection, rep }
    }
}

impl From<&str> for ChildMoniker {
    fn from(rep: &str) -> Self {
        ChildMoniker::parse(rep).expect(&format!("child moniker failed to parse: {}", rep))
    }
}

impl Ord for ChildMoniker {
    fn cmp(&self, other: &Self) -> Ordering {
        (&self.collection, &self.name).cmp(&(&other.collection, &other.name))
    }
}

impl PartialOrd for ChildMoniker {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl fmt::Display for ChildMoniker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn child_monikers() {
        let m = ChildMoniker::new("test".to_string(), None);
        assert_eq!("test", m.name());
        assert_eq!(None, m.collection());
        assert_eq!("test", m.as_str());
        assert_eq!("test", format!("{}", m));
        assert_eq!(m, ChildMoniker::from("test"));

        let m = ChildMoniker::new("test".to_string(), Some("coll".to_string()));
        assert_eq!("test", m.name());
        assert_eq!(Some("coll"), m.collection());
        assert_eq!("coll:test", m.as_str());
        assert_eq!("coll:test", format!("{}", m));
        assert_eq!(m, ChildMoniker::from("coll:test"));

        let max_length_part = "f".repeat(100);
        let m = ChildMoniker::parse(format!("{0}:{0}", max_length_part)).expect("valid moniker");
        assert_eq!(&max_length_part, m.name());
        assert_eq!(Some(max_length_part.as_str()), m.collection());

        assert!(ChildMoniker::parse("").is_err(), "cannot be empty");
        assert!(ChildMoniker::parse(":").is_err(), "cannot be empty with colon");
        assert!(ChildMoniker::parse("f:").is_err(), "second part cannot be empty with colon");
        assert!(ChildMoniker::parse(":f").is_err(), "first part cannot be empty with colon");
        assert!(ChildMoniker::parse("f:f:f").is_err(), "multiple colons not allowed");
        assert!(ChildMoniker::parse("@").is_err(), "invalid character in name");
        assert!(ChildMoniker::parse("@:f").is_err(), "invalid character in collection");
        assert!(ChildMoniker::parse("f:@").is_err(), "invalid character in name with collection");
        assert!(ChildMoniker::parse("f".repeat(101)).is_err(), "name too long");
        assert!(ChildMoniker::parse("f".repeat(101) + ":f").is_err(), "collection too long");
    }

    #[test]
    fn child_moniker_compare() {
        let a = ChildMoniker::new("a".to_string(), None);
        let aa = ChildMoniker::new("a".to_string(), Some("a".to_string()));
        let ab = ChildMoniker::new("a".to_string(), Some("b".to_string()));
        let ba = ChildMoniker::new("b".to_string(), Some("a".to_string()));
        let bb = ChildMoniker::new("b".to_string(), Some("b".to_string()));
        let aa_same = ChildMoniker::new("a".to_string(), Some("a".to_string()));

        assert_eq!(Ordering::Less, a.cmp(&aa));
        assert_eq!(Ordering::Greater, aa.cmp(&a));
        assert_eq!(Ordering::Less, a.cmp(&ab));
        assert_eq!(Ordering::Greater, ab.cmp(&a));
        assert_eq!(Ordering::Less, a.cmp(&ba));
        assert_eq!(Ordering::Greater, ba.cmp(&a));
        assert_eq!(Ordering::Less, a.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&a));

        assert_eq!(Ordering::Less, aa.cmp(&ab));
        assert_eq!(Ordering::Greater, ab.cmp(&aa));
        assert_eq!(Ordering::Less, aa.cmp(&ba));
        assert_eq!(Ordering::Greater, ba.cmp(&aa));
        assert_eq!(Ordering::Less, aa.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&aa));
        assert_eq!(Ordering::Equal, aa.cmp(&aa_same));
        assert_eq!(Ordering::Equal, aa_same.cmp(&aa));

        assert_eq!(Ordering::Greater, ab.cmp(&ba));
        assert_eq!(Ordering::Less, ba.cmp(&ab));
        assert_eq!(Ordering::Less, ab.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&ab));

        assert_eq!(Ordering::Less, ba.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&ba));
    }
}
