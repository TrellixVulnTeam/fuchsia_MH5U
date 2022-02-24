// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        error::Error, AnyRef, AsClause, Capability, CapabilityClause, Child, Collection, ConfigKey,
        ConfigNestedValueType, ConfigValueType, DebugRegistration, Document, Environment,
        EnvironmentExtends, EnvironmentRef, EventMode, EventModesClause, EventScope,
        EventSubscriptionsClause, Expose, ExposeFromRef, ExposeToRef, FromClause, Offer, OneOrMany,
        Path, PathClause, Program, ResolverRegistration, RightsClause, RunnerRegistration, Use,
        UseFromRef,
    },
    cm_types::{self as cm, Name},
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_data as fdata, fidl_fuchsia_io as fio,
    serde_json::{Map, Value},
    sha2::{Digest, Sha256},
    std::collections::{BTreeMap, HashSet},
    std::convert::{Into, TryInto},
};

/// Compiles the Document into a FIDL `Component`.
/// Note: This function ignores the `include` section of the document. It is
/// assumed that those entries were already processed.
pub fn compile(
    document: &Document,
    config_package_path: Option<&str>,
) -> Result<fdecl::Component, Error> {
    let all_capability_names = document.all_capability_names();
    let all_children = document.all_children_names().into_iter().collect();
    let all_collections = document.all_collection_names().into_iter().collect();
    Ok(fdecl::Component {
        program: document.program.as_ref().map(|p| translate_program(p)).transpose()?,
        uses: document
            .r#use
            .as_ref()
            .map(|u| translate_use(u, &all_capability_names, &all_children, &all_collections))
            .transpose()?,
        exposes: document
            .expose
            .as_ref()
            .map(|e| translate_expose(e, &all_capability_names, &all_collections))
            .transpose()?,
        offers: document
            .offer
            .as_ref()
            .map(|offer| {
                translate_offer(offer, &all_capability_names, &all_children, &all_collections)
            })
            .transpose()?,
        capabilities: document
            .capabilities
            .as_ref()
            .map(|c| translate_capabilities(c, false))
            .transpose()?,
        children: document.children.as_ref().map(translate_children).transpose()?,
        collections: document.collections.as_ref().map(translate_collections).transpose()?,
        environments: document
            .environments
            .as_ref()
            .map(|env| translate_environments(env, &all_capability_names))
            .transpose()?,
        facets: document.facets.clone().map(dictionary_from_nested_map).transpose()?,
        config: document
            .config
            .as_ref()
            .map(|c| {
                if let Some(p) = config_package_path {
                    Ok(translate_config(c, p))
                } else {
                    Err(Error::invalid_args(
                        "can't translate config: no package path for value file",
                    ))
                }
            })
            .transpose()?,
        ..fdecl::Component::EMPTY
    })
}

// Converts a Map<String, serde_json::Value> to a fuchsia Dictionary.
fn dictionary_from_map(in_obj: Map<String, Value>) -> Result<fdata::Dictionary, Error> {
    let mut entries = vec![];
    for (key, v) in in_obj {
        let value = value_to_dictionary_value(v)?;
        entries.push(fdata::DictionaryEntry { key, value });
    }
    Ok(fdata::Dictionary { entries: Some(entries), ..fdata::Dictionary::EMPTY })
}

// Converts a serde_json::Value into a fuchsia DictionaryValue. Used by `dictionary_from_map`.
fn value_to_dictionary_value(value: Value) -> Result<Option<Box<fdata::DictionaryValue>>, Error> {
    match value {
        Value::Null => Ok(None),
        Value::String(s) => Ok(Some(Box::new(fdata::DictionaryValue::Str(s.clone())))),
        Value::Array(arr) => {
            let mut strs = vec![];
            for val in arr {
                match val {
                    Value::String(s) => strs.push(s),
                    _ => return Err(Error::validate("Value must be string")),
                };
            }
            Ok(Some(Box::new(fdata::DictionaryValue::StrVec(strs))))
        }
        _ => return Err(Error::validate("Value must be string or list of strings")),
    }
}

/// Converts a [`serde_json::Map<String, serde_json::Value>`] to a [`fuchsia.data.Dictionary`].
///
/// Values may be null, strings, arrays of strings, or objects.
/// Object value are flattened with keys separated by a period:
///
/// ```json
/// {
///   "binary": "bin/app",
///   "lifecycle": {
///     "stop_event": "notify",
///     "nested": {
///       "foo": "bar"
///     }
///   }
/// }
/// ```
///
/// is flattened to:
///
/// ```json
/// {
///   "binary": "bin/app",
///   "lifecycle.stop_event": "notify",
///   "lifecycle.nested.foo": "bar"
/// }
/// ```
fn dictionary_from_nested_map(map: Map<String, Value>) -> Result<fdata::Dictionary, Error> {
    fn key_value_to_entries(
        key: String,
        value: Value,
    ) -> Result<Vec<fdata::DictionaryEntry>, Error> {
        if let Value::Object(map) = value {
            let entries = map
                .into_iter()
                .map(|(k, v)| key_value_to_entries([key.clone(), ".".to_string(), k].concat(), v))
                .collect::<Result<Vec<_>, _>>()?
                .into_iter()
                .flatten()
                .collect();
            return Ok(entries);
        }

        let entry_value = match value {
            Value::Null => Ok(None),
            Value::String(s) => Ok(Some(Box::new(fdata::DictionaryValue::Str(s.clone())))),
            Value::Array(arr) => {
                let mut strs = vec![];
                for val in arr {
                    match val {
                        Value::String(s) => strs.push(s),
                        _ => return Err(Error::validate("Value must be string")),
                    };
                }
                Ok(Some(Box::new(fdata::DictionaryValue::StrVec(strs))))
            }
            _ => Err(Error::validate("Value must be string or list of strings")),
        }?;
        Ok(vec![fdata::DictionaryEntry { key, value: entry_value }])
    }

    let entries = map
        .into_iter()
        .map(|(k, v)| key_value_to_entries(k, v))
        .collect::<Result<Vec<_>, _>>()?
        .into_iter()
        .flatten()
        .collect();
    Ok(fdata::Dictionary { entries: Some(entries), ..fdata::Dictionary::EMPTY })
}

/// Translates a [`Program`] to a [`fuchsa.sys2.Program`].
fn translate_program(program: &Program) -> Result<fdecl::Program, Error> {
    Ok(fdecl::Program {
        runner: program.runner.as_ref().map(|r| r.to_string()),
        info: Some(dictionary_from_nested_map(program.info.clone())?),
        ..fdecl::Program::EMPTY
    })
}

/// `use` rules consume a single capability from one source (parent|framework).
fn translate_use(
    use_in: &Vec<Use>,
    all_capability_names: &HashSet<Name>,
    all_children: &HashSet<&Name>,
    all_collections: &HashSet<&Name>,
) -> Result<Vec<fdecl::Use>, Error> {
    let mut out_uses = vec![];
    for use_ in use_in {
        if let Some(n) = &use_.service {
            let source = extract_use_source(use_, all_capability_names, all_children)?;
            let target_paths =
                all_target_use_paths(use_, use_).ok_or_else(|| Error::internal("no capability"))?;
            let source_names = n.to_vec();
            for (source_name, target_path) in source_names.into_iter().zip(target_paths.into_iter())
            {
                out_uses.push(fdecl::Use::Service(fdecl::UseService {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target_path: Some(target_path.into()),
                    dependency_type: Some(
                        use_.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                    ),
                    ..fdecl::UseService::EMPTY
                }));
            }
        } else if let Some(n) = &use_.protocol {
            let source = extract_use_source(use_, all_capability_names, all_children)?;
            let target_paths =
                all_target_use_paths(use_, use_).ok_or_else(|| Error::internal("no capability"))?;
            let source_names = n.to_vec();
            for (source_name, target_path) in source_names.into_iter().zip(target_paths.into_iter())
            {
                out_uses.push(fdecl::Use::Protocol(fdecl::UseProtocol {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target_path: Some(target_path.into()),
                    dependency_type: Some(
                        use_.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                    ),
                    ..fdecl::UseProtocol::EMPTY
                }));
            }
        } else if let Some(n) = &use_.directory {
            let source = extract_use_source(use_, all_capability_names, all_children)?;
            let target_path = one_target_use_path(use_, use_)?;
            let rights = extract_required_rights(use_, "use")?;
            let subdir = extract_use_subdir(use_);
            out_uses.push(fdecl::Use::Directory(fdecl::UseDirectory {
                source: Some(source),
                source_name: Some(n.clone().into()),
                target_path: Some(target_path.into()),
                rights: Some(rights),
                subdir: subdir.map(|s| s.into()),
                dependency_type: Some(
                    use_.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                ),
                ..fdecl::UseDirectory::EMPTY
            }));
        } else if let Some(n) = &use_.storage {
            let target_path = one_target_use_path(use_, use_)?;
            out_uses.push(fdecl::Use::Storage(fdecl::UseStorage {
                source_name: Some(n.clone().into()),
                target_path: Some(target_path.into()),
                ..fdecl::UseStorage::EMPTY
            }));
        } else if let Some(n) = &use_.event {
            let source = extract_use_event_source(use_)?;
            let target_names = all_target_capability_names(use_, use_)
                .ok_or_else(|| Error::internal("no capability"))?;
            let source_names = n.to_vec();
            for target_name in target_names {
                // When multiple source names are provided, there is no way to alias each one, so
                // source_name == target_name,
                // When one source name is provided, source_name may be aliased to a different
                // target_name, so we use source_names[0] to derive the source_name.
                let source_name = if source_names.len() == 1 {
                    source_names[0].clone()
                } else {
                    target_name.clone()
                };
                out_uses.push(fdecl::Use::Event(fdecl::UseEvent {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.into()),
                    target_name: Some(target_name.into()),
                    mode: match use_.event_modes() {
                        Some(modes) => {
                            if modes.0.contains(&EventMode::Sync) {
                                Some(fdecl::EventMode::Sync)
                            } else {
                                Some(fdecl::EventMode::Async)
                            }
                        }
                        None => Some(fdecl::EventMode::Async),
                    },
                    // We have already validated that none will be present if we were using many
                    // events.
                    filter: match use_.filter.clone() {
                        Some(dict) => Some(dictionary_from_map(dict)?),
                        None => None,
                    },
                    dependency_type: Some(
                        use_.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                    ),
                    ..fdecl::UseEvent::EMPTY
                }));
            }
        } else if let Some(name) = &use_.event_stream_deprecated {
            let opt_subscriptions = use_.event_subscriptions();
            out_uses.push(fdecl::Use::EventStreamDeprecated(fdecl::UseEventStreamDeprecated {
                name: Some(name.to_string()),
                subscriptions: opt_subscriptions.map(|subscriptions| {
                    subscriptions
                        .iter()
                        .flat_map(|subscription| {
                            let mode = subscription.mode.as_ref();
                            subscription.event.iter().map(move |event| fdecl::EventSubscription {
                                event_name: Some(event.to_string()),
                                mode: Some(match mode {
                                    Some(EventMode::Sync) => fdecl::EventMode::Sync,
                                    _ => fdecl::EventMode::Async,
                                }),
                                ..fdecl::EventSubscription::EMPTY
                            })
                        })
                        .collect()
                }),
                ..fdecl::UseEventStreamDeprecated::EMPTY
            }));
        } else if let Some(names) = &use_.event_stream {
            let source_names = annotate_type::<Vec<cm_types::Name>>(names.clone().into())
                .iter()
                .map(|name| name.to_string())
                .collect();
            let scopes = match use_.scope.clone() {
                Some(value) => Some(annotate_type::<Vec<EventScope>>(value.into())),
                None => None,
            };
            let internal_error = format!("Internal error in all_target_use_paths when translating an EventStream. Please file a bug.");
            out_uses.push(fdecl::Use::EventStream(fdecl::UseEventStream {
                source_names: Some(source_names),
                scope: match scopes {
                    Some(values) => {
                        let mut output = vec![];
                        for value in &values {
                            output.push(translate_child_or_collection_ref(
                                value.into(),
                                &all_children,
                                &all_collections,
                            )?);
                        }
                        Some(output)
                    }
                    None => None,
                },
                source: Some(extract_use_source(use_, all_capability_names, all_children)?),
                target_path: Some(
                    annotate_type::<Vec<cm_types::Path>>(
                        all_target_use_paths(use_, use_)
                            .ok_or_else(|| Error::internal(internal_error.clone()))?
                            .into(),
                    )
                    .iter()
                    .next()
                    .ok_or_else(|| Error::internal(internal_error.clone()))?
                    .as_str()
                    .to_string(),
                ),
                ..fdecl::UseEventStream::EMPTY
            }));
        } else {
            return Err(Error::internal(format!("no capability in use declaration")));
        };
    }
    Ok(out_uses)
}

/// `expose` rules route a single capability from one or more sources (self|framework|#<child>) to
/// one or more targets (parent|framework).
fn translate_expose(
    expose_in: &Vec<Expose>,
    all_capability_names: &HashSet<Name>,
    all_collections: &HashSet<&Name>,
) -> Result<Vec<fdecl::Expose>, Error> {
    let mut out_exposes = vec![];
    for expose in expose_in.iter() {
        let target = extract_expose_target(expose)?;
        if let Some(source_names) = expose.service() {
            let sources = extract_all_expose_sources(expose, Some(all_collections))?;
            let target_names = all_target_capability_names(expose, expose)
                .ok_or_else(|| Error::internal("no capability"))?;
            for (source_name, target_name) in source_names.into_iter().zip(target_names.into_iter())
            {
                for source in &sources {
                    out_exposes.push(fdecl::Expose::Service(fdecl::ExposeService {
                        source: Some(clone_ref(source)?),
                        source_name: Some(source_name.clone().into()),
                        target_name: Some(target_name.clone().into()),
                        target: Some(clone_ref(&target)?),
                        ..fdecl::ExposeService::EMPTY
                    }))
                }
            }
        } else if let Some(n) = expose.protocol() {
            let source = extract_single_expose_source(expose, Some(all_capability_names))?;
            let source_names = n.to_vec();
            let target_names = all_target_capability_names(expose, expose)
                .ok_or_else(|| Error::internal("no capability"))?;
            for (source_name, target_name) in source_names.into_iter().zip(target_names.into_iter())
            {
                out_exposes.push(fdecl::Expose::Protocol(fdecl::ExposeProtocol {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target_name: Some(target_name.into()),
                    target: Some(clone_ref(&target)?),
                    ..fdecl::ExposeProtocol::EMPTY
                }))
            }
        } else if let Some(n) = expose.directory() {
            let source = extract_single_expose_source(expose, None)?;
            let source_names = n.to_vec();
            let target_names = all_target_capability_names(expose, expose)
                .ok_or_else(|| Error::internal("no capability"))?;
            let rights = extract_expose_rights(expose)?;
            let subdir = extract_expose_subdir(expose);
            for (source_name, target_name) in source_names.into_iter().zip(target_names.into_iter())
            {
                out_exposes.push(fdecl::Expose::Directory(fdecl::ExposeDirectory {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target_name: Some(target_name.into()),
                    target: Some(clone_ref(&target)?),
                    rights,
                    subdir: subdir.as_ref().map(|s| s.clone().into()),
                    ..fdecl::ExposeDirectory::EMPTY
                }))
            }
        } else if let Some(n) = expose.runner() {
            let source = extract_single_expose_source(expose, None)?;
            let source_names = n.to_vec();
            let target_names = all_target_capability_names(expose, expose)
                .ok_or_else(|| Error::internal("no capability"))?;
            for (source_name, target_name) in source_names.into_iter().zip(target_names.into_iter())
            {
                out_exposes.push(fdecl::Expose::Runner(fdecl::ExposeRunner {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target: Some(clone_ref(&target)?),
                    target_name: Some(target_name.into()),
                    ..fdecl::ExposeRunner::EMPTY
                }))
            }
        } else if let Some(n) = expose.resolver() {
            let source = extract_single_expose_source(expose, None)?;
            let source_names = n.to_vec();
            let target_names = all_target_capability_names(expose, expose)
                .ok_or_else(|| Error::internal("no capability"))?;
            for (source_name, target_name) in source_names.into_iter().zip(target_names.into_iter())
            {
                out_exposes.push(fdecl::Expose::Resolver(fdecl::ExposeResolver {
                    source: Some(clone_ref(&source)?),
                    source_name: Some(source_name.clone().into()),
                    target: Some(clone_ref(&target)?),
                    target_name: Some(target_name.into()),
                    ..fdecl::ExposeResolver::EMPTY
                }))
            }
        } else {
            return Err(Error::internal(format!("expose: must specify a known capability")));
        }
    }
    Ok(out_exposes)
}

impl<T> Into<Vec<T>> for OneOrMany<T> {
    fn into(self) -> Vec<T> {
        match self {
            OneOrMany::One(one) => vec![one],
            OneOrMany::Many(many) => many,
        }
    }
}

// Allows the above Into to work by annotating the type.
fn annotate_type<T>(val: T) -> T {
    val
}

/// `offer` rules route multiple capabilities from multiple sources to multiple targets.
fn translate_offer(
    offer_in: &Vec<Offer>,
    all_capability_names: &HashSet<Name>,
    all_children: &HashSet<&Name>,
    all_collections: &HashSet<&Name>,
) -> Result<Vec<fdecl::Offer>, Error> {
    let mut out_offers = vec![];
    for offer in offer_in.iter() {
        if let Some(n) = offer.service() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Service(fdecl::OfferService {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    ..fdecl::OfferService::EMPTY
                }));
            }
        } else if let Some(n) = offer.protocol() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Protocol(fdecl::OfferProtocol {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    dependency_type: Some(
                        offer.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                    ),
                    ..fdecl::OfferProtocol::EMPTY
                }));
            }
        } else if let Some(n) = offer.directory() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Directory(fdecl::OfferDirectory {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    rights: extract_offer_rights(offer)?,
                    subdir: extract_offer_subdir(offer).map(|s| s.into()),
                    dependency_type: Some(
                        offer.dependency.clone().unwrap_or(cm::DependencyType::Strong).into(),
                    ),
                    ..fdecl::OfferDirectory::EMPTY
                }));
            }
        } else if let Some(n) = offer.storage() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Storage(fdecl::OfferStorage {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    ..fdecl::OfferStorage::EMPTY
                }));
            }
        } else if let Some(n) = offer.runner() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Runner(fdecl::OfferRunner {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    ..fdecl::OfferRunner::EMPTY
                }));
            }
        } else if let Some(n) = offer.resolver() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Resolver(fdecl::OfferResolver {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    ..fdecl::OfferResolver::EMPTY
                }));
            }
        } else if let Some(n) = offer.event() {
            let entries = extract_offer_sources_and_targets(
                offer,
                n,
                all_capability_names,
                all_children,
                all_collections,
            )?;
            for (source, source_name, target, target_name) in entries {
                out_offers.push(fdecl::Offer::Event(fdecl::OfferEvent {
                    source: Some(source),
                    source_name: Some(source_name.into()),
                    target: Some(target),
                    target_name: Some(target_name.into()),
                    // We have already validated that none will be present if we were using many
                    // events.
                    filter: match offer.filter.clone() {
                        Some(dict) => Some(dictionary_from_map(dict)?),
                        None => None,
                    },
                    mode: match offer.event_modes() {
                        Some(modes) => {
                            if modes.0.contains(&EventMode::Sync) {
                                Some(fdecl::EventMode::Sync)
                            } else {
                                Some(fdecl::EventMode::Async)
                            }
                        }
                        None => Some(fdecl::EventMode::Async),
                    },
                    ..fdecl::OfferEvent::EMPTY
                }));
            }
        } else {
            return Err(Error::internal(format!("no capability")));
        }
    }
    Ok(out_offers)
}

fn translate_children(children_in: &Vec<Child>) -> Result<Vec<fdecl::Child>, Error> {
    let mut out_children = vec![];
    for child in children_in.iter() {
        out_children.push(fdecl::Child {
            name: Some(child.name.clone().into()),
            url: Some(child.url.clone().into()),
            startup: Some(child.startup.clone().into()),
            environment: extract_environment_ref(child.environment.as_ref()).map(|e| e.into()),
            on_terminate: child.on_terminate.as_ref().map(|r| r.clone().into()),
            ..fdecl::Child::EMPTY
        });
    }
    Ok(out_children)
}

fn translate_collections(
    collections_in: &Vec<Collection>,
) -> Result<Vec<fdecl::Collection>, Error> {
    let mut out_collections = vec![];
    for collection in collections_in.iter() {
        out_collections.push(fdecl::Collection {
            name: Some(collection.name.clone().into()),
            durability: Some(collection.durability.clone().into()),
            allowed_offers: collection.allowed_offers.clone().map(|a| a.into()),
            environment: extract_environment_ref(collection.environment.as_ref()).map(|e| e.into()),
            ..fdecl::Collection::EMPTY
        });
    }
    Ok(out_collections)
}

/// Translates a nested value type to a [`fuchsia.config.decl.ConfigType`]
fn translate_nested_value_type(nested_type: &ConfigNestedValueType) -> fdecl::ConfigType {
    let layout = match nested_type {
        ConfigNestedValueType::Bool => fdecl::ConfigTypeLayout::Bool,
        ConfigNestedValueType::Uint8 => fdecl::ConfigTypeLayout::Uint8,
        ConfigNestedValueType::Uint16 => fdecl::ConfigTypeLayout::Uint16,
        ConfigNestedValueType::Uint32 => fdecl::ConfigTypeLayout::Uint32,
        ConfigNestedValueType::Uint64 => fdecl::ConfigTypeLayout::Uint64,
        ConfigNestedValueType::Int8 => fdecl::ConfigTypeLayout::Int8,
        ConfigNestedValueType::Int16 => fdecl::ConfigTypeLayout::Int16,
        ConfigNestedValueType::Int32 => fdecl::ConfigTypeLayout::Int32,
        ConfigNestedValueType::Int64 => fdecl::ConfigTypeLayout::Int64,
        ConfigNestedValueType::String { .. } => fdecl::ConfigTypeLayout::String,
    };
    let constraints = match nested_type {
        ConfigNestedValueType::String { max_size } => {
            vec![fdecl::LayoutConstraint::MaxSize(max_size.get())]
        }
        _ => vec![],
    };
    fdecl::ConfigType {
        layout,
        constraints,
        // This optional is not necessary, but without it,
        // FIDL compilation complains because of a possible include-cycle.
        // Bug: http://fxbug.dev/66350
        parameters: Some(vec![]),
    }
}

/// Translates a value type to a [`fuchsia.sys2.ConfigType`]
fn translate_value_type(value_type: &ConfigValueType) -> fdecl::ConfigType {
    let layout = match value_type {
        ConfigValueType::Bool => fdecl::ConfigTypeLayout::Bool,
        ConfigValueType::Uint8 => fdecl::ConfigTypeLayout::Uint8,
        ConfigValueType::Uint16 => fdecl::ConfigTypeLayout::Uint16,
        ConfigValueType::Uint32 => fdecl::ConfigTypeLayout::Uint32,
        ConfigValueType::Uint64 => fdecl::ConfigTypeLayout::Uint64,
        ConfigValueType::Int8 => fdecl::ConfigTypeLayout::Int8,
        ConfigValueType::Int16 => fdecl::ConfigTypeLayout::Int16,
        ConfigValueType::Int32 => fdecl::ConfigTypeLayout::Int32,
        ConfigValueType::Int64 => fdecl::ConfigTypeLayout::Int64,
        ConfigValueType::String { .. } => fdecl::ConfigTypeLayout::String,
        ConfigValueType::Vector { .. } => fdecl::ConfigTypeLayout::Vector,
    };
    let (constraints, parameters) = match value_type {
        ConfigValueType::String { max_size } => {
            (vec![fdecl::LayoutConstraint::MaxSize(max_size.get())], vec![])
        }
        ConfigValueType::Vector { max_count, element } => {
            let nested_type = translate_nested_value_type(element);
            (
                vec![fdecl::LayoutConstraint::MaxSize(max_count.get())],
                vec![fdecl::LayoutParameter::NestedType(nested_type)],
            )
        }
        _ => (vec![], vec![]),
    };
    fdecl::ConfigType {
        layout,
        constraints,
        // This optional is not necessary, but without it,
        // FIDL compilation complains because of a possible include-cycle.
        // Bug: http://fxbug.dev/66350
        parameters: Some(parameters),
    }
}

/// Translates a map of [`String`] -> [`ConfigValueType`] to a [`fuchsia.sys2.Config`]
fn translate_config(
    fields: &BTreeMap<ConfigKey, ConfigValueType>,
    package_path: &str,
) -> fdecl::ConfigSchema {
    let mut fidl_fields = vec![];

    // Compute a SHA-256 hash from each field
    let mut hasher = Sha256::new();

    for (key, value) in fields {
        fidl_fields.push(fdecl::ConfigField {
            key: Some(key.to_string()),
            type_: Some(translate_value_type(value)),
            ..fdecl::ConfigField::EMPTY
        });

        hasher.update(key.as_str());

        value.update_digest(&mut hasher);
    }

    let hash = hasher.finalize();
    let checksum = fdecl::ConfigChecksum::Sha256(*hash.as_ref());

    fdecl::ConfigSchema {
        fields: Some(fidl_fields),
        checksum: Some(checksum),
        // for now we only support ELF components that look up config by package path
        value_source: Some(fdecl::ConfigValueSource::PackagePath(package_path.to_owned())),
        ..fdecl::ConfigSchema::EMPTY
    }
}

fn translate_environments(
    envs_in: &Vec<Environment>,
    all_capability_names: &HashSet<Name>,
) -> Result<Vec<fdecl::Environment>, Error> {
    envs_in
        .iter()
        .map(|env| {
            Ok(fdecl::Environment {
                name: Some(env.name.clone().into()),
                extends: match env.extends {
                    Some(EnvironmentExtends::Realm) => Some(fdecl::EnvironmentExtends::Realm),
                    Some(EnvironmentExtends::None) => Some(fdecl::EnvironmentExtends::None),
                    None => Some(fdecl::EnvironmentExtends::None),
                },
                runners: env
                    .runners
                    .as_ref()
                    .map(|runners| {
                        runners
                            .iter()
                            .map(translate_runner_registration)
                            .collect::<Result<Vec<_>, Error>>()
                    })
                    .transpose()?,
                resolvers: env
                    .resolvers
                    .as_ref()
                    .map(|resolvers| {
                        resolvers
                            .iter()
                            .map(translate_resolver_registration)
                            .collect::<Result<Vec<_>, Error>>()
                    })
                    .transpose()?,
                debug_capabilities: env
                    .debug
                    .as_ref()
                    .map(|debug_capabiltities| {
                        translate_debug_capabilities(debug_capabiltities, all_capability_names)
                    })
                    .transpose()?,
                stop_timeout_ms: env.stop_timeout_ms.map(|s| s.0),
                ..fdecl::Environment::EMPTY
            })
        })
        .collect()
}

fn translate_runner_registration(
    reg: &RunnerRegistration,
) -> Result<fdecl::RunnerRegistration, Error> {
    Ok(fdecl::RunnerRegistration {
        source_name: Some(reg.runner.clone().into()),
        source: Some(extract_single_offer_source(reg, None)?),
        target_name: Some(reg.r#as.as_ref().unwrap_or(&reg.runner).clone().into()),
        ..fdecl::RunnerRegistration::EMPTY
    })
}

fn translate_resolver_registration(
    reg: &ResolverRegistration,
) -> Result<fdecl::ResolverRegistration, Error> {
    Ok(fdecl::ResolverRegistration {
        resolver: Some(reg.resolver.clone().into()),
        source: Some(extract_single_offer_source(reg, None)?),
        scheme: Some(
            reg.scheme
                .as_str()
                .parse::<cm_types::UrlScheme>()
                .map_err(|e| Error::internal(format!("invalid URL scheme: {}", e)))?
                .into(),
        ),
        ..fdecl::ResolverRegistration::EMPTY
    })
}

fn translate_debug_capabilities(
    capabilities: &Vec<DebugRegistration>,
    all_capability_names: &HashSet<Name>,
) -> Result<Vec<fdecl::DebugRegistration>, Error> {
    let mut out_capabilities = vec![];
    for capability in capabilities {
        if let Some(n) = capability.protocol() {
            let source = extract_single_offer_source(capability, Some(all_capability_names))?;
            let targets = all_target_capability_names(capability, capability)
                .ok_or_else(|| Error::internal("no capability"))?;
            let source_names = n.to_vec();
            for target_name in targets {
                // When multiple source names are provided, there is no way to alias each one, so
                // source_name == target_name.
                // When one source name is provided, source_name may be aliased to a different
                // target_name, so we source_names[0] to derive the source_name.
                //
                // TODO: This logic could be simplified to use iter::zip() if
                // extract_all_targets_for_each_child returned separate vectors for targets and
                // target_names instead of the cross product of them.
                let source_name = if source_names.len() == 1 {
                    source_names[0].clone()
                } else {
                    target_name.clone()
                };
                out_capabilities.push(fdecl::DebugRegistration::Protocol(
                    fdecl::DebugProtocolRegistration {
                        source: Some(clone_ref(&source)?),
                        source_name: Some(source_name.into()),
                        target_name: Some(target_name.into()),
                        ..fdecl::DebugProtocolRegistration::EMPTY
                    },
                ));
            }
        }
    }
    Ok(out_capabilities)
}

fn extract_use_source(
    in_obj: &Use,
    all_capability_names: &HashSet<Name>,
    all_children_names: &HashSet<&Name>,
) -> Result<fdecl::Ref, Error> {
    match in_obj.from.as_ref() {
        Some(UseFromRef::Parent) => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})),
        Some(UseFromRef::Framework) => Ok(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
        Some(UseFromRef::Debug) => Ok(fdecl::Ref::Debug(fdecl::DebugRef {})),
        Some(UseFromRef::Self_) => Ok(fdecl::Ref::Self_(fdecl::SelfRef {})),
        Some(UseFromRef::Named(name)) => {
            if all_capability_names.contains(&name) {
                Ok(fdecl::Ref::Capability(fdecl::CapabilityRef { name: name.clone().into() }))
            } else if all_children_names.contains(&name) {
                Ok(fdecl::Ref::Child(fdecl::ChildRef {
                    name: name.clone().into(),
                    collection: None,
                }))
            } else {
                Err(Error::internal(format!(
                    "use source \"{:?}\" not supported for \"use from\"",
                    name
                )))
            }
        }
        None => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})), // Default value.
    }
}

// Since fdecl::Ref is not cloneable, write our own clone function.
fn clone_ref(ref_: &fdecl::Ref) -> Result<fdecl::Ref, Error> {
    match ref_ {
        fdecl::Ref::Parent(parent_ref) => Ok(fdecl::Ref::Parent(parent_ref.clone())),
        fdecl::Ref::Self_(self_ref) => Ok(fdecl::Ref::Self_(self_ref.clone())),
        fdecl::Ref::Child(child_ref) => Ok(fdecl::Ref::Child(child_ref.clone())),
        fdecl::Ref::Collection(collection_ref) => {
            Ok(fdecl::Ref::Collection(collection_ref.clone()))
        }
        fdecl::Ref::Framework(framework_ref) => Ok(fdecl::Ref::Framework(framework_ref.clone())),
        fdecl::Ref::Capability(capability_ref) => {
            Ok(fdecl::Ref::Capability(capability_ref.clone()))
        }
        fdecl::Ref::Debug(debug_ref) => Ok(fdecl::Ref::Debug(debug_ref.clone())),
        _ => Err(Error::internal("Unknown fdecl::Ref found.")),
    }
}

fn extract_use_event_source(in_obj: &Use) -> Result<fdecl::Ref, Error> {
    match in_obj.from.as_ref() {
        Some(UseFromRef::Parent) => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})),
        Some(UseFromRef::Framework) => Ok(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
        Some(UseFromRef::Named(name)) => {
            Ok(fdecl::Ref::Capability(fdecl::CapabilityRef { name: name.clone().into() }))
        }
        Some(UseFromRef::Debug) => {
            Err(Error::internal(format!("Debug source provided for \"use event\"")))
        }
        Some(UseFromRef::Self_) => {
            Err(Error::internal(format!("Self source not supported for \"use event\"")))
        }
        None => Err(Error::internal(format!("No source \"from\" provided for \"use\""))),
    }
}

fn extract_use_subdir(in_obj: &Use) -> Option<cm::RelativePath> {
    in_obj.subdir.clone()
}

fn extract_expose_subdir(in_obj: &Expose) -> Option<cm::RelativePath> {
    in_obj.subdir.clone()
}

fn extract_offer_subdir(in_obj: &Offer) -> Option<cm::RelativePath> {
    in_obj.subdir.clone()
}

fn extract_expose_rights(in_obj: &Expose) -> Result<Option<fio::Operations>, Error> {
    match in_obj.rights.as_ref() {
        Some(rights_tokens) => {
            let mut rights = Vec::new();
            for token in rights_tokens.0.iter() {
                rights.append(&mut token.expand())
            }
            if rights.is_empty() {
                return Err(Error::missing_rights(
                    "Rights provided to expose are not well formed.",
                ));
            }
            let mut seen_rights = HashSet::with_capacity(rights.len());
            let mut operations: fio::Operations = fio::Operations::empty();
            for right in rights.iter() {
                if seen_rights.contains(&right) {
                    return Err(Error::duplicate_rights(
                        "Rights provided to expose are not well formed.",
                    ));
                }
                seen_rights.insert(right);
                operations |= *right;
            }

            Ok(Some(operations))
        }
        // Unlike use rights, expose rights can take a None value
        None => Ok(None),
    }
}

fn expose_source_from_ref(
    reference: &ExposeFromRef,
    all_capability_names: Option<&HashSet<Name>>,
    all_collections: Option<&HashSet<&Name>>,
) -> Result<fdecl::Ref, Error> {
    match reference {
        ExposeFromRef::Named(name) => {
            if all_capability_names.is_some() && all_capability_names.unwrap().contains(&name) {
                Ok(fdecl::Ref::Capability(fdecl::CapabilityRef { name: name.clone().into() }))
            } else if all_collections.is_some() && all_collections.unwrap().contains(&name) {
                Ok(fdecl::Ref::Collection(fdecl::CollectionRef { name: name.clone().into() }))
            } else {
                Ok(fdecl::Ref::Child(fdecl::ChildRef {
                    name: name.clone().into(),
                    collection: None,
                }))
            }
        }
        ExposeFromRef::Framework => Ok(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
        ExposeFromRef::Self_ => Ok(fdecl::Ref::Self_(fdecl::SelfRef {})),
    }
}

fn extract_single_expose_source(
    in_obj: &Expose,
    all_capability_names: Option<&HashSet<Name>>,
) -> Result<fdecl::Ref, Error> {
    match &in_obj.from {
        OneOrMany::One(reference) => expose_source_from_ref(&reference, all_capability_names, None),
        OneOrMany::Many(many) => {
            return Err(Error::internal(format!(
                "multiple unexpected \"from\" clauses for \"expose\": {:?}",
                many
            )))
        }
    }
}

fn extract_all_expose_sources(
    in_obj: &Expose,
    all_collections: Option<&HashSet<&Name>>,
) -> Result<Vec<fdecl::Ref>, Error> {
    in_obj
        .from
        .to_vec()
        .into_iter()
        .map(|e| expose_source_from_ref(e, None, all_collections))
        .collect()
}

fn extract_offer_rights(in_obj: &Offer) -> Result<Option<fio::Operations>, Error> {
    match in_obj.rights.as_ref() {
        Some(rights_tokens) => {
            let mut rights = Vec::new();
            for token in rights_tokens.0.iter() {
                rights.append(&mut token.expand())
            }
            if rights.is_empty() {
                return Err(Error::missing_rights("Rights provided to offer are not well formed."));
            }
            let mut seen_rights = HashSet::with_capacity(rights.len());
            let mut operations: fio::Operations = fio::Operations::empty();
            for right in rights.iter() {
                if seen_rights.contains(&right) {
                    return Err(Error::duplicate_rights(
                        "Rights provided to offer are not well formed.",
                    ));
                }
                seen_rights.insert(right);
                operations |= *right;
            }

            Ok(Some(operations))
        }
        // Unlike use rights, offer rights can take a None value
        None => Ok(None),
    }
}

fn extract_single_offer_source<T>(
    in_obj: &T,
    all_capability_names: Option<&HashSet<Name>>,
) -> Result<fdecl::Ref, Error>
where
    T: FromClause,
{
    match in_obj.from_() {
        OneOrMany::One(reference) => offer_source_from_ref(reference, all_capability_names, None),
        many => {
            return Err(Error::internal(format!(
                "multiple unexpected \"from\" clauses for \"offer\": {}",
                many
            )))
        }
    }
}

fn extract_all_offer_sources<T: FromClause>(
    in_obj: &T,
    all_capability_names: &HashSet<Name>,
    all_collections: &HashSet<&Name>,
) -> Result<Vec<fdecl::Ref>, Error> {
    in_obj
        .from_()
        .to_vec()
        .into_iter()
        .map(|r| {
            offer_source_from_ref(r.clone(), Some(all_capability_names), Some(all_collections))
        })
        .collect()
}

fn translate_child_or_collection_ref(
    reference: AnyRef,
    all_children: &HashSet<&Name>,
    all_collections: &HashSet<&Name>,
) -> Result<fdecl::Ref, Error> {
    match reference {
        AnyRef::Named(name) if all_children.contains(name) => {
            Ok(fdecl::Ref::Child(fdecl::ChildRef { name: name.clone().into(), collection: None }))
        }
        AnyRef::Named(name) if all_collections.contains(name) => {
            Ok(fdecl::Ref::Collection(fdecl::CollectionRef { name: name.clone().into() }))
        }
        AnyRef::Named(_) => Err(Error::internal(format!("dangling reference: \"{}\"", reference))),
        _ => Err(Error::internal(format!("invalid child reference: \"{}\"", reference))),
    }
}

// Return a list of (source, source capability id, target, target capability id) expressed in the
// `offer`.
fn extract_offer_sources_and_targets(
    offer: &Offer,
    source_names: OneOrMany<Name>,
    all_capability_names: &HashSet<Name>,
    all_children: &HashSet<&Name>,
    all_collections: &HashSet<&Name>,
) -> Result<Vec<(fdecl::Ref, Name, fdecl::Ref, Name)>, Error> {
    let mut out = vec![];

    let source_names = source_names.to_vec();
    let sources = extract_all_offer_sources(offer, all_capability_names, all_collections)?;
    let target_names = all_target_capability_names(offer, offer)
        .ok_or_else(|| Error::internal("no capability".to_string()))?;

    for source in &sources {
        for to in &offer.to {
            for target_name in &target_names {
                // When multiple source names are provided, there is no way to alias each one,
                // so we can assume source_name == target_name.  When one source name is provided,
                // source_name may be aliased to a different target_name, so we use
                // source_names[0] to obtain the source_name.
                let source_name = if source_names.len() == 1 {
                    source_names[0].clone()
                } else {
                    target_name.clone()
                };
                let target =
                    translate_child_or_collection_ref(to.into(), all_children, all_collections)?;
                out.push((source.clone(), source_name, target, target_name.clone()))
            }
        }
    }
    Ok(out)
}

/// Return the target paths specified in the given use declaration.
fn all_target_use_paths<T, U>(in_obj: &T, to_obj: &U) -> Option<OneOrMany<Path>>
where
    T: CapabilityClause,
    U: AsClause + PathClause,
{
    if let Some(n) = in_obj.service() {
        Some(svc_paths_from_names(n, to_obj))
    } else if let Some(n) = in_obj.protocol() {
        Some(svc_paths_from_names(n, to_obj))
    } else if let Some(_) = in_obj.directory() {
        let path = to_obj.path().expect("no path on use directory");
        Some(OneOrMany::One(path.clone()))
    } else if let Some(_) = in_obj.storage() {
        let path = to_obj.path().expect("no path on use storage");
        Some(OneOrMany::One(path.clone()))
    } else if let Some(_) = in_obj.event_stream_deprecated() {
        let path = to_obj.path().expect("no path on event stream");
        Some(OneOrMany::One(path.clone()))
    } else if let Some(_) = in_obj.event_stream() {
        let default_path = Path::new("/svc/fuchsia.component.EventStream".to_string()).unwrap();
        let path = to_obj.path().unwrap_or(&default_path);
        Some(OneOrMany::One(path.clone()))
    } else {
        None
    }
}

/// Returns the list of paths derived from a `use` declaration with `names` and `to_obj`. `to_obj`
/// must be a declaration that has a `path` clause.
fn svc_paths_from_names<T>(names: OneOrMany<Name>, to_obj: &T) -> OneOrMany<Path>
where
    T: PathClause,
{
    match names {
        OneOrMany::One(n) => {
            if let Some(path) = to_obj.path() {
                OneOrMany::One(path.clone())
            } else {
                OneOrMany::One(format!("/svc/{}", n).parse().unwrap())
            }
        }
        OneOrMany::Many(v) => {
            let many = v.iter().map(|n| format!("/svc/{}", n).parse().unwrap()).collect();
            OneOrMany::Many(many)
        }
    }
}

/// Return the single target path specified in the given use declaration.
fn one_target_use_path<T, U>(in_obj: &T, to_obj: &U) -> Result<Path, Error>
where
    T: CapabilityClause,
    U: AsClause + PathClause,
{
    match all_target_use_paths(in_obj, to_obj) {
        Some(OneOrMany::One(target_name)) => Ok(target_name),
        Some(OneOrMany::Many(_)) => {
            Err(Error::internal("expecting one capability, but multiple provided"))
        }
        _ => Err(Error::internal("expecting one capability, but none provided")),
    }
}

/// Return the target names or paths specified in the given capability.
fn all_target_capability_names<T, U>(in_obj: &T, to_obj: &U) -> Option<OneOrMany<Name>>
where
    T: CapabilityClause,
    U: AsClause + PathClause,
{
    if let Some(as_) = to_obj.r#as() {
        // We've already validated that when `as` is specified, only 1 source id exists.
        Some(OneOrMany::One(as_.clone()))
    } else {
        if let Some(n) = in_obj.service() {
            Some(n.clone())
        } else if let Some(n) = in_obj.protocol() {
            Some(n.clone())
        } else if let Some(n) = in_obj.directory() {
            Some(n.clone())
        } else if let Some(n) = in_obj.storage() {
            Some(n.clone())
        } else if let Some(n) = in_obj.runner() {
            Some(n.clone())
        } else if let Some(n) = in_obj.resolver() {
            Some(n.clone())
        } else if let Some(n) = in_obj.event() {
            Some(n.clone())
        } else {
            None
        }
    }
}

fn extract_expose_target(in_obj: &Expose) -> Result<fdecl::Ref, Error> {
    match &in_obj.to {
        Some(ExposeToRef::Parent) => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})),
        Some(ExposeToRef::Framework) => Ok(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
        None => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})),
    }
}

fn extract_environment_ref(r: Option<&EnvironmentRef>) -> Option<cm::Name> {
    r.map(|r| {
        let EnvironmentRef::Named(name) = r;
        name.clone()
    })
}

pub fn translate_capabilities(
    capabilities_in: &Vec<Capability>,
    as_builtin: bool,
) -> Result<Vec<fdecl::Capability>, Error> {
    let mut out_capabilities = vec![];
    for capability in capabilities_in {
        if let Some(service) = &capability.service {
            for n in service.to_vec() {
                let source_path = match as_builtin {
                    true => None,
                    false => Some(
                        capability
                            .path
                            .clone()
                            .unwrap_or_else(|| format!("/svc/{}", n).parse().unwrap())
                            .into(),
                    ),
                };
                out_capabilities.push(fdecl::Capability::Service(fdecl::Service {
                    name: Some(n.clone().into()),
                    source_path: source_path,
                    ..fdecl::Service::EMPTY
                }));
            }
        } else if let Some(protocol) = &capability.protocol {
            for n in protocol.to_vec() {
                let source_path = match as_builtin {
                    true => None,
                    false => Some(
                        capability
                            .path
                            .clone()
                            .unwrap_or_else(|| format!("/svc/{}", n).parse().unwrap())
                            .into(),
                    ),
                };
                out_capabilities.push(fdecl::Capability::Protocol(fdecl::Protocol {
                    name: Some(n.clone().into()),
                    source_path: source_path,
                    ..fdecl::Protocol::EMPTY
                }));
            }
        } else if let Some(n) = &capability.directory {
            let source_path = match as_builtin {
                true => None,
                false => {
                    Some(capability.path.as_ref().expect("missing source path").clone().into())
                }
            };
            let rights = extract_required_rights(capability, "capability")?;
            out_capabilities.push(fdecl::Capability::Directory(fdecl::Directory {
                name: Some(n.clone().into()),
                source_path: source_path,
                rights: Some(rights),
                ..fdecl::Directory::EMPTY
            }));
        } else if let Some(n) = &capability.storage {
            if as_builtin {
                return Err(Error::internal(format!(
                    "built-in storage capabilities are not supported"
                )));
            }
            let backing_dir = capability
                .backing_dir
                .as_ref()
                .expect("storage has no path or backing_dir")
                .clone()
                .into();
            out_capabilities.push(fdecl::Capability::Storage(fdecl::Storage {
                name: Some(n.clone().into()),
                backing_dir: Some(backing_dir),
                source: Some(offer_source_from_ref(
                    capability.from.as_ref().unwrap().into(),
                    None,
                    None,
                )?),
                subdir: capability.subdir.clone().map(Into::into),
                storage_id: Some(
                    capability.storage_id.clone().expect("storage is missing storage_id").into(),
                ),
                ..fdecl::Storage::EMPTY
            }));
        } else if let Some(n) = &capability.runner {
            let source_path = match as_builtin {
                true => None,
                false => {
                    Some(capability.path.as_ref().expect("missing source path").clone().into())
                }
            };
            out_capabilities.push(fdecl::Capability::Runner(fdecl::Runner {
                name: Some(n.clone().into()),
                source_path: source_path,
                ..fdecl::Runner::EMPTY
            }));
        } else if let Some(n) = &capability.resolver {
            let source_path = match as_builtin {
                true => None,
                false => {
                    Some(capability.path.as_ref().expect("missing source path").clone().into())
                }
            };
            out_capabilities.push(fdecl::Capability::Resolver(fdecl::Resolver {
                name: Some(n.clone().into()),
                source_path: source_path,
                ..fdecl::Resolver::EMPTY
            }));
        } else if let Some(n) = &capability.event {
            if !as_builtin {
                return Err(Error::internal(format!(
                    "event capabilities may only be declared as built-in capabilities"
                )));
            }
            out_capabilities.push(fdecl::Capability::Event(fdecl::Event {
                name: Some(n.clone().into()),
                ..fdecl::Event::EMPTY
            }));
        } else {
            return Err(Error::internal(format!("no capability declaration recognized")));
        }
    }
    Ok(out_capabilities)
}

pub fn extract_required_rights<T>(in_obj: &T, keyword: &str) -> Result<fio::Operations, Error>
where
    T: RightsClause,
{
    match in_obj.rights() {
        Some(rights_tokens) => {
            let mut rights = Vec::new();
            for token in rights_tokens.0.iter() {
                rights.append(&mut token.expand())
            }
            if rights.is_empty() {
                return Err(Error::missing_rights(format!(
                    "Rights provided to `{}` are not well formed.",
                    keyword
                )));
            }
            let mut seen_rights = HashSet::with_capacity(rights.len());
            let mut operations: fio::Operations = fio::Operations::empty();
            for right in rights.iter() {
                if seen_rights.contains(&right) {
                    return Err(Error::duplicate_rights(format!(
                        "Rights provided to `{}` are not well formed.",
                        keyword
                    )));
                }
                seen_rights.insert(right);
                operations |= *right;
            }

            Ok(operations)
        }
        None => Err(Error::internal(format!(
            "No `{}` rights provided but required for directories",
            keyword
        ))),
    }
}

pub fn offer_source_from_ref(
    reference: AnyRef<'_>,
    all_capability_names: Option<&HashSet<Name>>,
    all_collection_names: Option<&HashSet<&Name>>,
) -> Result<fdecl::Ref, Error> {
    match reference {
        AnyRef::Named(name) => {
            if all_capability_names.is_some() && all_capability_names.unwrap().contains(&name) {
                Ok(fdecl::Ref::Capability(fdecl::CapabilityRef { name: name.clone().into() }))
            } else if all_collection_names.is_some()
                && all_collection_names.unwrap().contains(&name)
            {
                Ok(fdecl::Ref::Collection(fdecl::CollectionRef { name: name.clone().into() }))
            } else {
                Ok(fdecl::Ref::Child(fdecl::ChildRef {
                    name: name.clone().into(),
                    collection: None,
                }))
            }
        }
        AnyRef::Framework => Ok(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
        AnyRef::Debug => Ok(fdecl::Ref::Debug(fdecl::DebugRef {})),
        AnyRef::Parent => Ok(fdecl::Ref::Parent(fdecl::ParentRef {})),
        AnyRef::Self_ => Ok(fdecl::Ref::Self_(fdecl::SelfRef {})),
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            error::Error, AnyRef, AsClause, Capability, CapabilityClause, Child, Collection,
            DebugRegistration, Document, Environment, EnvironmentExtends, EnvironmentRef,
            EventMode, EventModesClause, EventSubscriptionsClause, Expose, ExposeFromRef,
            ExposeToRef, FromClause, Offer, OneOrMany, Path, PathClause, Program,
            ResolverRegistration, RightsClause, RunnerRegistration, Use, UseFromRef,
        },
        cm_types::{self as cm, Name},
        fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_data as fdata, fidl_fuchsia_io as fio,
        serde_json::{json, Map, Value},
        std::collections::HashSet,
        std::convert::Into,
    };

    macro_rules! test_compile {
    (
        $(
            $(#[$m:meta])*
            $test_name:ident => {
                input = $input:expr,
                output = $expected:expr,
            },
        )+
    ) => {
        $(
            $(#[$m])*
            #[test]
            fn $test_name() {
                let input = serde_json::from_str(&$input.to_string()).expect("deserialization failed");
                let actual = compile(&input, Some("fake.cvf")).expect("compilation failed");
                assert_eq!(actual, $expected);
            }
        )+
    }
}

    fn default_component_decl() -> fdecl::Component {
        fdecl::Component::EMPTY
    }

    test_compile! {
        test_compile_empty => {
            input = json!({}),
            output = default_component_decl(),
        },

        test_compile_empty_includes => {
            input = json!({ "include": [] }),
            output = default_component_decl(),
        },

        test_compile_program => {
            input = json!({
                "program": {
                    "runner": "elf",
                    "binary": "bin/app",
                },
            }),
            output = fdecl::Component {
                program: Some(fdecl::Program {
                    runner: Some("elf".to_string()),
                    info: Some(fdata::Dictionary {
                        entries: Some(vec![fdata::DictionaryEntry {
                            key: "binary".to_string(),
                            value: Some(Box::new(fdata::DictionaryValue::Str("bin/app".to_string()))),
                        }]),
                        ..fdata::Dictionary::EMPTY
                    }),
                    ..fdecl::Program::EMPTY
                }),
                ..default_component_decl()
            },
        },

        test_compile_program_with_nested_objects => {
            input = json!({
                "program": {
                    "runner": "elf",
                    "binary": "bin/app",
                    "one": {
                        "two": {
                            "three.four": {
                                "five": "six"
                            }
                        },
                    }
                },
            }),
            output = fdecl::Component {
                program: Some(fdecl::Program {
                    runner: Some("elf".to_string()),
                    info: Some(fdata::Dictionary {
                        entries: Some(vec![
                            fdata::DictionaryEntry {
                                key: "binary".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("bin/app".to_string()))),
                            },
                            fdata::DictionaryEntry {
                                key: "one.two.three.four.five".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("six".to_string()))),
                            },
                        ]),
                        ..fdata::Dictionary::EMPTY
                    }),
                    ..fdecl::Program::EMPTY
                }),
                ..default_component_decl()
            },
        },

        test_compile_use => {
            input = json!({
                "use": [
                    { "protocol": "LegacyCoolFonts", "path": "/svc/fuchsia.fonts.LegacyProvider" },
                    { "protocol": "fuchsia.sys2.LegacyRealm", "from": "framework" },
                    { "protocol": "fuchsia.sys2.StorageAdmin", "from": "#data-storage" },
                    { "protocol": "fuchsia.sys2.DebugProto", "from": "debug" },
                    { "protocol": "fuchsia.sys2.Echo", "from": "self"},
                    { "directory": "assets", "rights" : ["read_bytes"], "path": "/data/assets" },
                    {
                        "directory": "config",
                        "path": "/data/config",
                        "from": "parent",
                        "rights": ["read_bytes"],
                        "subdir": "fonts",
                    },
                    { "storage": "hippos", "path": "/hippos" },
                    { "storage": "cache", "path": "/tmp" },
                    { "event": "destroyed", "from": "parent" },
                    { "event": ["started", "stopped"], "from": "framework" },
                    {
                        "event": "directory_ready",
                        "as": "diagnostics",
                        "from": "parent",
                        "filter": { "name": "diagnostics" }
                    },
                    {
                        "event_stream_deprecated": "foo_stream",
                        "subscriptions": [
                            {
                                "event": [ "started", "diagnostics" ],
                                "mode": "async",
                            },
                            {
                                "event": [ "destroyed" ],
                                "mode": "sync"
                            }
                        ]
                    },
                    {
                        "event_stream": "bar_stream",
                    },
                    {
                        "event_stream": ["foobar", "stream"],
                        "scope": ["#logger", "#modular"]
                    }
                ],
                "capabilities": [
                    {
                        "storage": "data-storage",
                        "from": "parent",
                        "backing_dir": "minfs",
                        "storage_id": "static_instance_id_or_moniker",
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "environment": "#env_one"
                    }
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                ],
            }
        ),
            output = fdecl::Component {
                uses: Some(vec![
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("LegacyCoolFonts".to_string()),
                            target_path: Some("/svc/fuchsia.fonts.LegacyProvider".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("fuchsia.sys2.LegacyRealm".to_string()),
                            target_path: Some("/svc/fuchsia.sys2.LegacyRealm".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Capability(fdecl::CapabilityRef { name: "data-storage".to_string() })),
                            source_name: Some("fuchsia.sys2.StorageAdmin".to_string()),
                            target_path: Some("/svc/fuchsia.sys2.StorageAdmin".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Debug(fdecl::DebugRef {})),
                            source_name: Some("fuchsia.sys2.DebugProto".to_string()),
                            target_path: Some("/svc/fuchsia.sys2.DebugProto".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            source_name: Some("fuchsia.sys2.Echo".to_string()),
                            target_path: Some("/svc/fuchsia.sys2.Echo".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Directory (
                        fdecl::UseDirectory {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets".to_string()),
                            target_path: Some("/data/assets".to_string()),
                            rights: Some(fio::Operations::READ_BYTES),
                            subdir: None,
                            ..fdecl::UseDirectory::EMPTY
                        }
                    ),
                    fdecl::Use::Directory (
                        fdecl::UseDirectory {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("config".to_string()),
                            target_path: Some("/data/config".to_string()),
                            rights: Some(fio::Operations::READ_BYTES),
                            subdir: Some("fonts".to_string()),
                            ..fdecl::UseDirectory::EMPTY
                        }
                    ),
                    fdecl::Use::Storage (
                        fdecl::UseStorage {
                            source_name: Some("hippos".to_string()),
                            target_path: Some("/hippos".to_string()),
                            ..fdecl::UseStorage::EMPTY
                        }
                    ),
                    fdecl::Use::Storage (
                        fdecl::UseStorage {
                            source_name: Some("cache".to_string()),
                            target_path: Some("/tmp".to_string()),
                            ..fdecl::UseStorage::EMPTY
                        }
                    ),
                    fdecl::Use::Event (
                        fdecl::UseEvent {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("destroyed".to_string()),
                            target_name: Some("destroyed".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::UseEvent::EMPTY
                        }
                    ),
                    fdecl::Use::Event (
                        fdecl::UseEvent {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("started".to_string()),
                            target_name: Some("started".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::UseEvent::EMPTY
                        }
                    ),
                    fdecl::Use::Event (
                        fdecl::UseEvent {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("stopped".to_string()),
                            target_name: Some("stopped".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::UseEvent::EMPTY
                        }
                    ),
                    fdecl::Use::Event (
                        fdecl::UseEvent {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("directory_ready".to_string()),
                            target_name: Some("diagnostics".to_string()),
                            filter: Some(fdata::Dictionary {
                                entries: Some(vec![
                                    fdata::DictionaryEntry {
                                        key: "name".to_string(),
                                        value: Some(Box::new(fdata::DictionaryValue::Str("diagnostics".to_string()))),
                                    },
                                ]),
                                ..fdata::Dictionary::EMPTY
                            }),
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::UseEvent::EMPTY
                        }
                    ),
                    fdecl::Use::EventStreamDeprecated(fdecl::UseEventStreamDeprecated {
                        name: Some("foo_stream".to_string()),
                        subscriptions: Some(vec![
                            fdecl::EventSubscription {
                                event_name: Some("started".to_string()),
                                mode: Some(fdecl::EventMode::Async),
                                ..fdecl::EventSubscription::EMPTY
                            },
                            fdecl::EventSubscription {
                                event_name: Some("diagnostics".to_string()),
                                mode: Some(fdecl::EventMode::Async),
                                ..fdecl::EventSubscription::EMPTY
                            },
                            fdecl::EventSubscription {
                                event_name: Some("destroyed".to_string()),
                                mode: Some(fdecl::EventMode::Sync),
                                ..fdecl::EventSubscription::EMPTY
                            },
                        ]),
                        ..fdecl::UseEventStreamDeprecated::EMPTY
                    }),
                    fdecl::Use::EventStream(fdecl::UseEventStream {
                        source_names: Some(vec!["bar_stream".to_string()]),
                        source: Some(fdecl::Ref::Parent(fdecl::ParentRef{})),
                        target_path: Some("/svc/fuchsia.component.EventStream".to_string()),
                        ..fdecl::UseEventStream::EMPTY
                    }),
                    fdecl::Use::EventStream(fdecl::UseEventStream {
                        source_names: Some(vec!["foobar".to_string(), "stream".to_string()]),
                        scope: Some(vec![fdecl::Ref::Child(fdecl::ChildRef{name:"logger".to_string(), collection: None}), fdecl::Ref::Collection(fdecl::CollectionRef{name:"modular".to_string()})]),
                        source: Some(fdecl::Ref::Parent(fdecl::ParentRef{})),
                        target_path: Some("/svc/fuchsia.component.EventStream".to_string()),
                        ..fdecl::UseEventStream::EMPTY
                    })
                ]),
                collections:Some(vec![
                    fdecl::Collection{
                        name:Some("modular".to_string()),
                        durability:Some(fdecl::Durability::Persistent),
                        ..fdecl::Collection::EMPTY
                    },
                ]),
                capabilities: Some(vec![
                    fdecl::Capability::Storage(fdecl::Storage {
                        name: Some("data-storage".to_string()),
                        source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                        backing_dir: Some("minfs".to_string()),
                        subdir: None,
                        storage_id: Some(fdecl::StorageId::StaticInstanceIdOrMoniker),
                        ..fdecl::Storage::EMPTY
                    }),
                ]),
                children: Some(vec![
                    fdecl::Child{
                        name:Some("logger".to_string()),
                        url:Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup:Some(fdecl::StartupMode::Lazy),
                        environment: Some("env_one".to_string()),
                        ..fdecl::Child::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_expose => {
            input = json!({
                "expose": [
                    {
                        "protocol": "fuchsia.logger.Log",
                        "from": "#logger",
                        "as": "fuchsia.logger.LegacyLog",
                        "to": "parent"
                    },
                    {
                        "protocol": [ "A", "B" ],
                        "from": "self",
                        "to": "parent"
                    },
                    {
                        "protocol": "C",
                        "from": "#data-storage",
                    },
                    {
                        "directory": "blob",
                        "from": "self",
                        "to": "framework",
                        "rights": ["r*"],
                    },
                    {
                        "directory": [ "blob2", "blob3" ],
                        "from": "#logger",
                        "to": "parent",
                    },
                    { "directory": "hub", "from": "framework" },
                    { "runner": "web", "from": "#logger", "to": "parent", "as": "web-rename" },
                    { "runner": [ "runner_a", "runner_b" ], "from": "#logger" },
                    { "resolver": "my_resolver", "from": "#logger", "to": "parent", "as": "pkg_resolver" },
                    { "resolver": [ "resolver_a", "resolver_b" ], "from": "#logger" },
                ],
                "capabilities": [
                    { "protocol": "A" },
                    { "protocol": "B" },
                    {
                        "directory": "blob",
                        "path": "/volumes/blobfs/blob",
                        "rights": ["r*"],
                    },
                    {
                        "runner": "web",
                        "path": "/svc/fuchsia.component.ComponentRunner",
                    },
                    {
                        "storage": "data-storage",
                        "from": "parent",
                        "backing_dir": "minfs",
                        "storage_id": "static_instance_id_or_moniker",
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                ],
            }),
            output = fdecl::Component {
                exposes: Some(vec![
                    fdecl::Expose::Protocol (
                        fdecl::ExposeProtocol {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("fuchsia.logger.Log".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            ..fdecl::ExposeProtocol::EMPTY
                        }
                    ),
                    fdecl::Expose::Protocol (
                        fdecl::ExposeProtocol {
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            source_name: Some("A".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("A".to_string()),
                            ..fdecl::ExposeProtocol::EMPTY
                        }
                    ),
                    fdecl::Expose::Protocol (
                        fdecl::ExposeProtocol {
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            source_name: Some("B".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("B".to_string()),
                            ..fdecl::ExposeProtocol::EMPTY
                        }
                    ),
                    fdecl::Expose::Protocol (
                        fdecl::ExposeProtocol {
                            source: Some(fdecl::Ref::Capability(fdecl::CapabilityRef {
                                name: "data-storage".to_string(),
                            })),
                            source_name: Some("C".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("C".to_string()),
                            ..fdecl::ExposeProtocol::EMPTY
                        }
                    ),
                    fdecl::Expose::Directory (
                        fdecl::ExposeDirectory {
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            source_name: Some("blob".to_string()),
                            target: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            target_name: Some("blob".to_string()),
                            rights: Some(
                                fio::Operations::CONNECT | fio::Operations::ENUMERATE |
                                fio::Operations::TRAVERSE | fio::Operations::READ_BYTES |
                                fio::Operations::GET_ATTRIBUTES
                            ),
                            subdir: None,
                            ..fdecl::ExposeDirectory::EMPTY
                        }
                    ),
                    fdecl::Expose::Directory (
                        fdecl::ExposeDirectory {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("blob2".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("blob2".to_string()),
                            rights: None,
                            subdir: None,
                            ..fdecl::ExposeDirectory::EMPTY
                        }
                    ),
                    fdecl::Expose::Directory (
                        fdecl::ExposeDirectory {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("blob3".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("blob3".to_string()),
                            rights: None,
                            subdir: None,
                            ..fdecl::ExposeDirectory::EMPTY
                        }
                    ),
                    fdecl::Expose::Directory (
                        fdecl::ExposeDirectory {
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("hub".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("hub".to_string()),
                            rights: None,
                            subdir: None,
                            ..fdecl::ExposeDirectory::EMPTY
                        }
                    ),
                    fdecl::Expose::Runner (
                        fdecl::ExposeRunner {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("web".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("web-rename".to_string()),
                            ..fdecl::ExposeRunner::EMPTY
                        }
                    ),
                    fdecl::Expose::Runner (
                        fdecl::ExposeRunner {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("runner_a".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("runner_a".to_string()),
                            ..fdecl::ExposeRunner::EMPTY
                        }
                    ),
                    fdecl::Expose::Runner (
                        fdecl::ExposeRunner {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("runner_b".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("runner_b".to_string()),
                            ..fdecl::ExposeRunner::EMPTY
                        }
                    ),
                    fdecl::Expose::Resolver (
                        fdecl::ExposeResolver {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("my_resolver".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("pkg_resolver".to_string()),
                            ..fdecl::ExposeResolver::EMPTY
                        }
                    ),
                    fdecl::Expose::Resolver (
                        fdecl::ExposeResolver {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("resolver_a".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("resolver_a".to_string()),
                            ..fdecl::ExposeResolver::EMPTY
                        }
                    ),
                    fdecl::Expose::Resolver (
                        fdecl::ExposeResolver {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("resolver_b".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("resolver_b".to_string()),
                            ..fdecl::ExposeResolver::EMPTY
                        }
                    ),
                ]),
                offers: None,
                capabilities: Some(vec![
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("A".to_string()),
                            source_path: Some("/svc/A".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("B".to_string()),
                            source_path: Some("/svc/B".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Directory (
                        fdecl::Directory {
                            name: Some("blob".to_string()),
                            source_path: Some("/volumes/blobfs/blob".to_string()),
                            rights: Some(fio::Operations::CONNECT | fio::Operations::ENUMERATE |
                                fio::Operations::TRAVERSE | fio::Operations::READ_BYTES |
                                fio::Operations::GET_ATTRIBUTES
                            ),
                            ..fdecl::Directory::EMPTY
                        }
                    ),
                    fdecl::Capability::Runner (
                        fdecl::Runner {
                            name: Some("web".to_string()),
                            source_path: Some("/svc/fuchsia.component.ComponentRunner".to_string()),
                            ..fdecl::Runner::EMPTY
                        }
                    ),
                    fdecl::Capability::Storage(fdecl::Storage {
                        name: Some("data-storage".to_string()),
                        source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                        backing_dir: Some("minfs".to_string()),
                        subdir: None,
                        storage_id: Some(fdecl::StorageId::StaticInstanceIdOrMoniker),
                        ..fdecl::Storage::EMPTY
                    }),
                ]),
                children: Some(vec![
                    fdecl::Child {
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_offer => {
            input = json!({
                "offer": [
                    {
                        "protocol": "fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": "#netstack", // Verifies compilation of singleton "to:".
                        "dependency": "weak"
                    },
                    {
                        "protocol": "fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": [ "#modular" ], // Verifies compilation of "to:" as array of one element.
                        "as": "fuchsia.logger.LegacySysLog",
                        "dependency": "strong"
                    },
                    {
                        "protocol": [
                            "fuchsia.setui.SetUiService",
                            "fuchsia.test.service.Name"
                        ],
                        "from": "parent",
                        "to": [ "#modular" ]
                    },
                    {
                        "protocol": "fuchsia.sys2.StorageAdmin",
                        "from": "#data",
                        "to": [ "#modular" ],
                    },
                    {
                        "directory": "assets",
                        "from": "parent",
                        "to": [ "#netstack" ],
                        "dependency": "weak_for_migration"
                    },
                    {
                        "directory": [ "assets2", "assets3" ],
                        "from": "parent",
                        "to": [ "#modular", "#netstack" ],
                    },
                    {
                        "directory": "data",
                        "from": "parent",
                        "to": [ "#modular" ],
                        "as": "assets",
                        "subdir": "index/file",
                        "dependency": "strong"
                    },
                    {
                        "directory": "hub",
                        "from": "framework",
                        "to": [ "#modular" ],
                        "as": "hub",
                    },
                    {
                        "storage": "data",
                        "from": "self",
                        "to": [
                            "#netstack",
                            "#modular"
                        ],
                    },
                    {
                        "storage": [ "storage_a", "storage_b" ],
                        "from": "parent",
                        "to": "#netstack",
                    },
                    {
                        "runner": "elf",
                        "from": "parent",
                        "to": [ "#modular" ],
                        "as": "elf-renamed",
                    },
                    {
                        "runner": [ "runner_a", "runner_b" ],
                        "from": "parent",
                        "to": "#netstack",
                    },
                    {
                        "event": "destroyed",
                        "from": "framework",
                        "to": [ "#netstack"],
                        "as": "destroyed_net"
                    },
                    {
                        "event": [ "stopped", "started" ],
                        "from": "parent",
                        "to": [ "#modular" ],
                    },
                    {
                        "event": "directory_ready",
                        "from": "parent",
                        "to": [ "#netstack" ],
                        "as": "net-ready",
                        "filter": {
                            "name": [
                                "diagnostics",
                                "foo"
                            ],
                        }
                    },
                    {
                        "resolver": "my_resolver",
                        "from": "parent",
                        "to": [ "#modular" ],
                        "as": "pkg_resolver",
                    },
                    {
                        "resolver": [ "resolver_a", "resolver_b" ],
                        "from": "parent",
                        "to": "#netstack",
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
                    },
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                ],
                "capabilities": [
                    {
                        "storage": "data",
                        "backing_dir": "minfs",
                        "from": "#logger",
                        "storage_id": "static_instance_id_or_moniker",
                    },
                ],
            }),
            output = fdecl::Component {
                offers: Some(vec![
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Weak),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("fuchsia.logger.LegacySysLog".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("fuchsia.setui.SetUiService".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("fuchsia.setui.SetUiService".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("fuchsia.test.service.Name".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("fuchsia.test.service.Name".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Capability(fdecl::CapabilityRef {
                                name: "data".to_string(),
                            })),
                            source_name: Some("fuchsia.sys2.StorageAdmin".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("fuchsia.sys2.StorageAdmin".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("assets".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::WeakForMigration),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets2".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("assets2".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets3".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("assets3".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets2".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("assets2".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("assets3".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("assets3".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("data".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("assets".to_string()),
                            rights: None,
                            subdir: Some("index/file".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Directory (
                        fdecl::OfferDirectory {
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("hub".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("hub".to_string()),
                            rights: None,
                            subdir: None,
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            ..fdecl::OfferDirectory::EMPTY
                        }
                    ),
                    fdecl::Offer::Storage (
                        fdecl::OfferStorage {
                            source_name: Some("data".to_string()),
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("data".to_string()),
                            ..fdecl::OfferStorage::EMPTY
                        }
                    ),
                    fdecl::Offer::Storage (
                        fdecl::OfferStorage {
                            source_name: Some("data".to_string()),
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("data".to_string()),
                            ..fdecl::OfferStorage::EMPTY
                        }
                    ),
                    fdecl::Offer::Storage (
                        fdecl::OfferStorage {
                            source_name: Some("storage_a".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("storage_a".to_string()),
                            ..fdecl::OfferStorage::EMPTY
                        }
                    ),
                    fdecl::Offer::Storage (
                        fdecl::OfferStorage {
                            source_name: Some("storage_b".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("storage_b".to_string()),
                            ..fdecl::OfferStorage::EMPTY
                        }
                    ),
                    fdecl::Offer::Runner (
                        fdecl::OfferRunner {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("elf".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("elf-renamed".to_string()),
                            ..fdecl::OfferRunner::EMPTY
                        }
                    ),
                    fdecl::Offer::Runner (
                        fdecl::OfferRunner {
                            source_name: Some("runner_a".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("runner_a".to_string()),
                            ..fdecl::OfferRunner::EMPTY
                        }
                    ),
                    fdecl::Offer::Runner (
                        fdecl::OfferRunner {
                            source_name: Some("runner_b".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("runner_b".to_string()),
                            ..fdecl::OfferRunner::EMPTY
                        }
                    ),
                    fdecl::Offer::Event (
                        fdecl::OfferEvent {
                            source: Some(fdecl::Ref::Framework(fdecl::FrameworkRef {})),
                            source_name: Some("destroyed".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("destroyed_net".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::OfferEvent::EMPTY
                        }
                    ),
                    fdecl::Offer::Event (
                        fdecl::OfferEvent {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("stopped".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("stopped".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::OfferEvent::EMPTY
                        }
                    ),
                    fdecl::Offer::Event (
                        fdecl::OfferEvent {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("started".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("started".to_string()),
                            filter: None,
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::OfferEvent::EMPTY
                        }
                    ),
                    fdecl::Offer::Event (
                        fdecl::OfferEvent {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("directory_ready".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("net-ready".to_string()),
                            filter: Some(fdata::Dictionary {
                                entries: Some(vec![
                                    fdata::DictionaryEntry {
                                        key: "name".to_string(),
                                        value: Some(Box::new(fdata::DictionaryValue::StrVec(
                                            vec!["diagnostics".to_string(), "foo".to_string()]
                                        ))),
                                    },
                                ]),
                                ..fdata::Dictionary::EMPTY
                            }),
                            mode: Some(fdecl::EventMode::Async),
                            ..fdecl::OfferEvent::EMPTY
                        }
                    ),
                    fdecl::Offer::Resolver (
                        fdecl::OfferResolver {
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("my_resolver".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("pkg_resolver".to_string()),
                            ..fdecl::OfferResolver::EMPTY
                        }
                    ),
                    fdecl::Offer::Resolver (
                        fdecl::OfferResolver {
                            source_name: Some("resolver_a".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("resolver_a".to_string()),
                            ..fdecl::OfferResolver::EMPTY
                        }
                    ),
                    fdecl::Offer::Resolver (
                        fdecl::OfferResolver {
                            source_name: Some("resolver_b".to_string()),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("resolver_b".to_string()),
                            ..fdecl::OfferResolver::EMPTY
                        }
                    ),
                ]),
                capabilities: Some(vec![
                    fdecl::Capability::Storage (
                        fdecl::Storage {
                            name: Some("data".to_string()),
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            backing_dir: Some("minfs".to_string()),
                            subdir: None,
                            storage_id: Some(fdecl::StorageId::StaticInstanceIdOrMoniker),
                            ..fdecl::Storage::EMPTY
                        }
                    )
                ]),
                children: Some(vec![
                    fdecl::Child {
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                    fdecl::Child {
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                ]),
                collections: Some(vec![
                    fdecl::Collection {
                        name: Some("modular".to_string()),
                        durability: Some(fdecl::Durability::Persistent),
                        allowed_offers: None,
                        environment: None,
                        ..fdecl::Collection::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_children => {
            input = json!({
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                    },
                    {
                        "name": "gmail",
                        "url": "https://www.google.com/gmail",
                        "startup": "eager",
                    },
                    {
                        "name": "echo",
                        "url": "fuchsia-pkg://fuchsia.com/echo/stable#meta/echo.cm",
                        "startup": "lazy",
                        "on_terminate": "reboot",
                        "environment": "#myenv",
                    },
                ],
                "environments": [
                    {
                        "name": "myenv",
                        "extends": "realm",
                    },
                ],
            }),
            output = fdecl::Component {
                children: Some(vec![
                    fdecl::Child {
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                    fdecl::Child {
                        name: Some("gmail".to_string()),
                        url: Some("https://www.google.com/gmail".to_string()),
                        startup: Some(fdecl::StartupMode::Eager),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                    fdecl::Child {
                        name: Some("echo".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/echo/stable#meta/echo.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: Some("myenv".to_string()),
                        on_terminate: Some(fdecl::OnTerminate::Reboot),
                        ..fdecl::Child::EMPTY
                    }
                ]),
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::Realm),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_collections => {
            input = json!({
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                    {
                        "name": "tests",
                        "durability": "transient",
                        "environment": "#myenv",
                    },
                ],
                "environments": [
                    {
                        "name": "myenv",
                        "extends": "realm",
                    }
                ],
            }),
            output = fdecl::Component {
                collections: Some(vec![
                    fdecl::Collection {
                        name: Some("modular".to_string()),
                        durability: Some(fdecl::Durability::Persistent),
                        allowed_offers: None,
                        environment: None,
                        ..fdecl::Collection::EMPTY
                    },
                    fdecl::Collection {
                        name: Some("tests".to_string()),
                        durability: Some(fdecl::Durability::Transient),
                        allowed_offers: None,
                        environment: Some("myenv".to_string()),
                        ..fdecl::Collection::EMPTY
                    }
                ]),
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::Realm),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_capabilities => {
            input = json!({
                "capabilities": [
                    {
                        "protocol": "myprotocol",
                        "path": "/protocol",
                    },
                    {
                        "protocol": "myprotocol2",
                    },
                    {
                        "protocol": [ "myprotocol3", "myprotocol4" ],
                    },
                    {
                        "directory": "mydirectory",
                        "path": "/directory",
                        "rights": [ "connect" ],
                    },
                    {
                        "storage": "mystorage",
                        "backing_dir": "storage",
                        "from": "#minfs",
                        "storage_id": "static_instance_id_or_moniker",
                    },
                    {
                        "storage": "mystorage2",
                        "backing_dir": "storage2",
                        "from": "#minfs",
                        "storage_id": "static_instance_id",
                    },
                    {
                        "runner": "myrunner",
                        "path": "/runner",
                    },
                    {
                        "resolver": "myresolver",
                        "path": "/resolver"
                    },
                ],
                "children": [
                    {
                        "name": "minfs",
                        "url": "fuchsia-pkg://fuchsia.com/minfs/stable#meta/minfs.cm",
                    },
                ]
            }),
            output = fdecl::Component {
                capabilities: Some(vec![
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("myprotocol".to_string()),
                            source_path: Some("/protocol".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("myprotocol2".to_string()),
                            source_path: Some("/svc/myprotocol2".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("myprotocol3".to_string()),
                            source_path: Some("/svc/myprotocol3".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Protocol (
                        fdecl::Protocol {
                            name: Some("myprotocol4".to_string()),
                            source_path: Some("/svc/myprotocol4".to_string()),
                            ..fdecl::Protocol::EMPTY
                        }
                    ),
                    fdecl::Capability::Directory (
                        fdecl::Directory {
                            name: Some("mydirectory".to_string()),
                            source_path: Some("/directory".to_string()),
                            rights: Some(fio::Operations::CONNECT),
                            ..fdecl::Directory::EMPTY
                        }
                    ),
                    fdecl::Capability::Storage (
                        fdecl::Storage {
                            name: Some("mystorage".to_string()),
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "minfs".to_string(),
                                collection: None,
                            })),
                            backing_dir: Some("storage".to_string()),
                            subdir: None,
                            storage_id: Some(fdecl::StorageId::StaticInstanceIdOrMoniker),
                            ..fdecl::Storage::EMPTY
                        }
                    ),
                    fdecl::Capability::Storage (
                        fdecl::Storage {
                            name: Some("mystorage2".to_string()),
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "minfs".to_string(),
                                collection: None,
                            })),
                            backing_dir: Some("storage2".to_string()),
                            subdir: None,
                            storage_id: Some(fdecl::StorageId::StaticInstanceId),
                            ..fdecl::Storage::EMPTY
                        }
                    ),
                    fdecl::Capability::Runner (
                        fdecl::Runner {
                            name: Some("myrunner".to_string()),
                            source_path: Some("/runner".to_string()),
                            ..fdecl::Runner::EMPTY
                        }
                    ),
                    fdecl::Capability::Resolver (
                        fdecl::Resolver {
                            name: Some("myresolver".to_string()),
                            source_path: Some("/resolver".to_string()),
                            ..fdecl::Resolver::EMPTY
                        }
                    )
                ]),
                children: Some(vec![
                    fdecl::Child {
                        name: Some("minfs".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/minfs/stable#meta/minfs.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    }
                ]),
                ..default_component_decl()
            },
        },

        test_compile_facets => {
            input = json!({
                "facets": {
                    "title": "foo",
                    "authors": [ "me", "you" ],
                    "year": "2018",
                    "metadata": {
                        "publisher": "The Books Publisher",
                    }
                }
            }),
            output = fdecl::Component {
                facets: Some(fdata::Dictionary {
                        entries: Some(vec![
                            fdata::DictionaryEntry {
                                key: "authors".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::StrVec(vec!["me".to_owned(), "you".to_owned()]))),
                            },
                            fdata::DictionaryEntry {
                                key: "metadata.publisher".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("The Books Publisher".to_string()))),
                            },
                            fdata::DictionaryEntry {
                                key: "title".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("foo".to_string()))),
                            },
                            fdata::DictionaryEntry {
                                key: "year".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("2018".to_string()))),
                            },
                        ]),
                        ..fdata::Dictionary::EMPTY
                    }
            ),
            ..default_component_decl()
            },
        },

        test_compile_environment => {
            input = json!({
                "environments": [
                    {
                        "name": "myenv",
                    },
                    {
                        "name": "myenv2",
                        "extends": "realm",
                    },
                    {
                        "name": "myenv3",
                        "extends": "none",
                        "__stop_timeout_ms": 8000u32,
                    }
                ],
            }),
            output = fdecl::Component {
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::None),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    },
                    fdecl::Environment {
                        name: Some("myenv2".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::Realm),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    },
                    fdecl::Environment {
                        name: Some("myenv3".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::None),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: Some(8000),
                        ..fdecl::Environment::EMPTY
                    },
                ]),
                ..default_component_decl()
            },
        },

        test_compile_environment_with_runner_and_resolver => {
            input = json!({
                "environments": [
                    {
                        "name": "myenv",
                        "runners": [
                            {
                                "runner": "dart",
                                "from": "parent",
                            }
                        ],
                        "resolvers": [
                            {
                                "resolver": "pkg_resolver",
                                "from": "parent",
                                "scheme": "fuchsia-pkg",
                            }
                        ],
                    },
                ],
            }),
            output = fdecl::Component {
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::None),
                        runners: Some(vec![
                            fdecl::RunnerRegistration {
                                source_name: Some("dart".to_string()),
                                source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                                target_name: Some("dart".to_string()),
                                ..fdecl::RunnerRegistration::EMPTY
                            }
                        ]),
                        resolvers: Some(vec![
                            fdecl::ResolverRegistration {
                                resolver: Some("pkg_resolver".to_string()),
                                source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                                scheme: Some("fuchsia-pkg".to_string()),
                                ..fdecl::ResolverRegistration::EMPTY
                            }
                        ]),
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    },
                ]),
                ..default_component_decl()
            },
        },

        test_compile_environment_with_runner_alias => {
            input = json!({
                "environments": [
                    {
                        "name": "myenv",
                        "runners": [
                            {
                                "runner": "dart",
                                "from": "parent",
                                "as": "my-dart",
                            }
                        ],
                    },
                ],
            }),
            output = fdecl::Component {
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::None),
                        runners: Some(vec![
                            fdecl::RunnerRegistration {
                                source_name: Some("dart".to_string()),
                                source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                                target_name: Some("my-dart".to_string()),
                                ..fdecl::RunnerRegistration::EMPTY
                            }
                        ]),
                        resolvers: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    },
                ]),
                ..default_component_decl()
            },
        },

        test_compile_environment_with_debug => {
            input = json!({
                "capabilities": [
                    {
                        "protocol": "fuchsia.serve.service",
                    },
                ],
                "environments": [
                    {
                        "name": "myenv",
                        "debug": [
                            {
                                "protocol": "fuchsia.serve.service",
                                "from": "self",
                                "as": "my-service",
                            }
                        ],
                    },
                ],
            }),
            output = fdecl::Component {
                capabilities: Some(vec![
                    fdecl::Capability::Protocol(
                        fdecl::Protocol {
                            name : Some("fuchsia.serve.service".to_owned()),
                            source_path: Some("/svc/fuchsia.serve.service".to_owned()),
                            ..fdecl::Protocol::EMPTY
                        }
                    )
                ]),
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::None),
                        debug_capabilities: Some(vec![
                            fdecl::DebugRegistration::Protocol( fdecl::DebugProtocolRegistration {
                                source_name: Some("fuchsia.serve.service".to_string()),
                                source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                                target_name: Some("my-service".to_string()),
                                ..fdecl::DebugProtocolRegistration::EMPTY
                            }),
                        ]),
                        resolvers: None,
                        runners: None,
                        stop_timeout_ms: None,
                        ..fdecl::Environment::EMPTY
                    },
                ]),
                ..default_component_decl()
            },
        },

        test_compile_all_sections => {
            input = json!({
                "program": {
                    "runner": "elf",
                    "binary": "bin/app",
                },
                "use": [
                    { "protocol": "LegacyCoolFonts", "path": "/svc/fuchsia.fonts.LegacyProvider" },
                    { "protocol": [ "ReallyGoodFonts", "IWouldNeverUseTheseFonts"]},
                    { "protocol":  "DebugProtocol", "from": "debug"},
                ],
                "expose": [
                    { "directory": "blobfs", "from": "self", "rights": ["r*"]},
                ],
                "offer": [
                    {
                        "protocol": "fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": [ "#netstack", "#modular" ],
                        "dependency": "weak"
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                    },
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                ],
                "capabilities": [
                    {
                        "directory": "blobfs",
                        "path": "/volumes/blobfs",
                        "rights": [ "r*" ],
                    },
                    {
                        "runner": "myrunner",
                        "path": "/runner",
                    },
                    {
                        "protocol": "fuchsia.serve.service",
                    }
                ],
                "facets": {
                    "author": "Fuchsia",
                    "year": "2018",
                },
                "environments": [
                    {
                        "name": "myenv",
                        "extends": "realm",
                        "debug": [
                            {
                                "protocol": "fuchsia.serve.service",
                                "from": "self",
                                "as": "my-service",
                            },
                            {
                                "protocol": "fuchsia.logger.LegacyLog",
                                "from": "#logger",
                            }
                        ]
                    }
                ],
            }),
            output = fdecl::Component {
                program: Some(fdecl::Program {
                    runner: Some("elf".to_string()),
                    info: Some(fdata::Dictionary {
                        entries: Some(vec![fdata::DictionaryEntry {
                            key: "binary".to_string(),
                            value: Some(Box::new(fdata::DictionaryValue::Str("bin/app".to_string()))),
                        }]),
                        ..fdata::Dictionary::EMPTY
                    }),
                    ..fdecl::Program::EMPTY
                }),
                uses: Some(vec![
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("LegacyCoolFonts".to_string()),
                            target_path: Some("/svc/fuchsia.fonts.LegacyProvider".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("ReallyGoodFonts".to_string()),
                            target_path: Some("/svc/ReallyGoodFonts".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            source_name: Some("IWouldNeverUseTheseFonts".to_string()),
                            target_path: Some("/svc/IWouldNeverUseTheseFonts".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                    fdecl::Use::Protocol (
                        fdecl::UseProtocol {
                            dependency_type: Some(fdecl::DependencyType::Strong),
                            source: Some(fdecl::Ref::Debug(fdecl::DebugRef {})),
                            source_name: Some("DebugProtocol".to_string()),
                            target_path: Some("/svc/DebugProtocol".to_string()),
                            ..fdecl::UseProtocol::EMPTY
                        }
                    ),
                ]),
                exposes: Some(vec![
                    fdecl::Expose::Directory (
                        fdecl::ExposeDirectory {
                            source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                            source_name: Some("blobfs".to_string()),
                            target: Some(fdecl::Ref::Parent(fdecl::ParentRef {})),
                            target_name: Some("blobfs".to_string()),
                            rights: Some(
                                fio::Operations::CONNECT | fio::Operations::ENUMERATE |
                                fio::Operations::TRAVERSE | fio::Operations::READ_BYTES |
                                fio::Operations::GET_ATTRIBUTES
                            ),
                            subdir: None,
                            ..fdecl::ExposeDirectory::EMPTY
                        }
                    ),
                ]),
                offers: Some(vec![
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            target: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "netstack".to_string(),
                                collection: None,
                            })),
                            target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Weak),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                    fdecl::Offer::Protocol (
                        fdecl::OfferProtocol {
                            source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                name: "logger".to_string(),
                                collection: None,
                            })),
                            source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            target: Some(fdecl::Ref::Collection(fdecl::CollectionRef {
                                name: "modular".to_string(),
                            })),
                            target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                            dependency_type: Some(fdecl::DependencyType::Weak),
                            ..fdecl::OfferProtocol::EMPTY
                        }
                    ),
                ]),
                capabilities: Some(vec![
                    fdecl::Capability::Directory (
                        fdecl::Directory {
                            name: Some("blobfs".to_string()),
                            source_path: Some("/volumes/blobfs".to_string()),
                            rights: Some(fio::Operations::CONNECT | fio::Operations::ENUMERATE |
                                fio::Operations::TRAVERSE | fio::Operations::READ_BYTES |
                                fio::Operations::GET_ATTRIBUTES
                            ),
                            ..fdecl::Directory::EMPTY
                        }
                    ),
                    fdecl::Capability::Runner (
                        fdecl::Runner {
                            name: Some("myrunner".to_string()),
                            source_path: Some("/runner".to_string()),
                            ..fdecl::Runner::EMPTY
                        }
                    ),
                    fdecl::Capability::Protocol(
                        fdecl::Protocol {
                            name : Some("fuchsia.serve.service".to_owned()),
                            source_path: Some("/svc/fuchsia.serve.service".to_owned()),
                            ..fdecl::Protocol::EMPTY
                        }
                    )
                ]),
                children: Some(vec![
                    fdecl::Child {
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                    fdecl::Child {
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fdecl::StartupMode::Lazy),
                        environment: None,
                        on_terminate: None,
                        ..fdecl::Child::EMPTY
                    },
                ]),
                collections: Some(vec![
                    fdecl::Collection {
                        name: Some("modular".to_string()),
                        durability: Some(fdecl::Durability::Persistent),
                        allowed_offers: None,
                        environment: None,
                        ..fdecl::Collection::EMPTY
                    }
                ]),
                environments: Some(vec![
                    fdecl::Environment {
                        name: Some("myenv".to_string()),
                        extends: Some(fdecl::EnvironmentExtends::Realm),
                        runners: None,
                        resolvers: None,
                        stop_timeout_ms: None,
                        debug_capabilities: Some(vec![
                            fdecl::DebugRegistration::Protocol( fdecl::DebugProtocolRegistration {
                                source_name: Some("fuchsia.serve.service".to_string()),
                                source: Some(fdecl::Ref::Self_(fdecl::SelfRef {})),
                                target_name: Some("my-service".to_string()),
                                ..fdecl::DebugProtocolRegistration::EMPTY
                            }),
                            fdecl::DebugRegistration::Protocol( fdecl::DebugProtocolRegistration {
                                source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                                source: Some(fdecl::Ref::Child(fdecl::ChildRef {
                                    name: "logger".to_string(),
                                    collection: None,
                                })),
                                target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                                ..fdecl::DebugProtocolRegistration::EMPTY
                            }),
                        ]),
                        ..fdecl::Environment::EMPTY
                    }
                ]),
                facets: Some(fdata::Dictionary {
                        entries: Some(vec![
                            fdata::DictionaryEntry {
                                key: "author".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("Fuchsia".to_string()))),
                            },
                            fdata::DictionaryEntry {
                                key: "year".to_string(),
                                value: Some(Box::new(fdata::DictionaryValue::Str("2018".to_string()))),
                            },
                        ]),
                        ..fdata::Dictionary::EMPTY
                    }),
                ..fdecl::Component::EMPTY
            },
        },
    }
}
