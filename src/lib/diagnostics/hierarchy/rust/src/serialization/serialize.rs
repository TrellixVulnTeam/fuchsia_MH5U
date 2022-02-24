// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{ArrayContent, Bucket, DiagnosticsHierarchy, Property},
    base64,
    serde::ser::{Serialize, SerializeMap, SerializeSeq, Serializer},
};

impl<Key> Serialize for DiagnosticsHierarchy<Key>
where
    Key: AsRef<str>,
{
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut s = serializer.serialize_map(Some(1))?;
        let name = self.name.clone();
        s.serialize_entry(&name, &SerializableHierarchyFields { hierarchy: &self })?;
        s.end()
    }
}

pub struct SerializableHierarchyFields<'a, Key> {
    pub(in crate) hierarchy: &'a DiagnosticsHierarchy<Key>,
}

impl<'a, Key> Serialize for SerializableHierarchyFields<'a, Key>
where
    Key: AsRef<str>,
{
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let items = self.hierarchy.properties.len() + self.hierarchy.children.len();
        let mut s = serializer.serialize_map(Some(items))?;
        for property in self.hierarchy.properties.iter() {
            let name = property.name();
            let _ = match property {
                Property::String(_, value) => s.serialize_entry(name, &value)?,
                Property::Int(_, value) => s.serialize_entry(name, &value)?,
                Property::Uint(_, value) => s.serialize_entry(name, &value)?,
                Property::Double(_, value) => {
                    let value =
                        if value.is_nan() || (value.is_infinite() && value.is_sign_positive()) {
                            f64::MAX
                        } else if value.is_infinite() && value.is_sign_negative() {
                            f64::MIN
                        } else {
                            *value
                        };
                    s.serialize_entry(name, &value)?;
                }
                Property::Bool(_, value) => s.serialize_entry(name, &value)?,
                Property::Bytes(_, array) => {
                    s.serialize_entry(name, &format!("b64:{}", base64::encode(&array)))?
                }
                Property::DoubleArray(_, array) => {
                    s.serialize_entry(name, &array)?;
                }
                Property::IntArray(_, array) => {
                    s.serialize_entry(name, &array)?;
                }
                Property::UintArray(_, array) => {
                    s.serialize_entry(name, &array)?;
                }
                Property::StringList(_, list) => {
                    s.serialize_entry(name, &list)?;
                }
            };
        }
        for child in self.hierarchy.children.iter() {
            s.serialize_entry(&child.name, &SerializableHierarchyFields { hierarchy: child })?;
        }
        s.end()
    }
}

macro_rules! impl_serialize_for_array_value {
    ($($type:ty,)*) => {
        $(
            impl Serialize for ArrayContent<$type> {
                fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                    match self {
                        ArrayContent::Buckets(buckets) => {
                            let mut s = serializer.serialize_map(Some(1))?;
                            s.serialize_entry("buckets", &buckets)?;
                            s.end()
                        }
                        ArrayContent::Values(values) => {
                            let mut s = serializer.serialize_seq(Some(values.len()))?;
                            for value in values {
                                s.serialize_element(&value)?;
                            }
                            s.end()
                        }
                    }
                }
            }
        )*
    }
}

macro_rules! impl_serialize_for_array_bucket {
    ($($type:ty,)*) => {
        $(
            impl Serialize for Bucket<$type> {
                fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                    let mut s = serializer.serialize_map(Some(3))?;
                    s.serialize_entry("count", &self.count)?;
                    s.serialize_entry("floor", &self.floor)?;
                    s.serialize_entry("ceiling", &self.ceiling)?;
                    s.end()
                }
            }
        )*
    }
}

impl<'a> Serialize for Bucket<f64> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut s = serializer.serialize_map(Some(3))?;
        let parts = [("count", self.count), ("floor", self.floor), ("ceiling", self.ceiling)];
        for (entry_key, value) in parts.iter() {
            if *value == std::f64::MAX || *value == std::f64::INFINITY || *value == std::f64::MAX {
                s.serialize_entry(entry_key, &std::f64::MAX)?;
            } else if *value == std::f64::MIN || *value == std::f64::NEG_INFINITY {
                s.serialize_entry(entry_key, &std::f64::MIN)?;
            } else {
                s.serialize_entry(entry_key, value)?;
            }
        }
        s.end()
    }
}

impl_serialize_for_array_value![i64, u64, f64,];
impl_serialize_for_array_bucket![i64, u64,];

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{hierarchy, ArrayFormat},
    };

    #[fuchsia::test]
    fn serialize_json() {
        let mut hierarchy = test_hierarchy();
        hierarchy.sort();
        let expected = expected_json();
        let result = serde_json::to_string_pretty(&hierarchy).expect("failed to serialize");
        assert_eq!(result, expected);
    }

    #[fuchsia::test]
    fn serialize_doubles() {
        let hierarchy = hierarchy! {
            root: {
                inf: f64::INFINITY,
                neg_inf: f64::NEG_INFINITY,
                nan: f64::NAN,
            }
        };
        let result = serde_json::to_string_pretty(&hierarchy).expect("serialized");
        assert_eq!(
            result,
            r#"{
  "root": {
    "inf": 1.7976931348623157e308,
    "neg_inf": -1.7976931348623157e308,
    "nan": 1.7976931348623157e308
  }
}"#
        );
    }

    fn test_hierarchy() -> DiagnosticsHierarchy {
        DiagnosticsHierarchy::new(
            "root",
            vec![
                Property::UintArray("array".to_string(), ArrayContent::Values(vec![0, 2, 4])),
                Property::Bool("bool_true".to_string(), true),
                Property::Bool("bool_false".to_string(), false),
                Property::StringList(
                    "string_list".to_string(),
                    vec!["foo".to_string(), "bar".to_string()],
                ),
                Property::StringList("empty_string_list".to_string(), vec![]),
            ],
            vec![
                DiagnosticsHierarchy::new(
                    "a",
                    vec![
                        Property::Double("double".to_string(), 2.5),
                        Property::DoubleArray(
                            "histogram".to_string(),
                            ArrayContent::new(
                                vec![0.0, 2.0, 4.0, 1.0, 3.0, 4.0],
                                ArrayFormat::ExponentialHistogram,
                            )
                            .unwrap(),
                        ),
                        Property::Bytes("bytes".to_string(), vec![5u8, 0xf1, 0xab]),
                    ],
                    vec![],
                ),
                DiagnosticsHierarchy::new(
                    "b",
                    vec![
                        Property::Int("int".to_string(), -2),
                        Property::String("string".to_string(), "some value".to_string()),
                        Property::IntArray(
                            "histogram".to_string(),
                            ArrayContent::new(vec![0, 2, 4, 1, 3], ArrayFormat::LinearHistogram)
                                .unwrap(),
                        ),
                    ],
                    vec![],
                ),
            ],
        )
    }

    fn expected_json() -> String {
        r#"{
  "root": {
    "array": [
      0,
      2,
      4
    ],
    "bool_false": false,
    "bool_true": true,
    "empty_string_list": [],
    "string_list": [
      "foo",
      "bar"
    ],
    "a": {
      "bytes": "b64:BfGr",
      "double": 2.5,
      "histogram": {
        "buckets": [
          {
            "count": 1.0,
            "floor": -1.7976931348623157e308,
            "ceiling": 0.0
          },
          {
            "count": 3.0,
            "floor": 0.0,
            "ceiling": 2.0
          },
          {
            "count": 4.0,
            "floor": 2.0,
            "ceiling": 1.7976931348623157e308
          }
        ]
      }
    },
    "b": {
      "histogram": {
        "buckets": [
          {
            "count": 4,
            "floor": -9223372036854775808,
            "ceiling": 0
          },
          {
            "count": 1,
            "floor": 0,
            "ceiling": 2
          },
          {
            "count": 3,
            "floor": 2,
            "ceiling": 9223372036854775807
          }
        ]
      },
      "int": -2,
      "string": "some value"
    }
  }
}"#
        .to_string()
    }
}
