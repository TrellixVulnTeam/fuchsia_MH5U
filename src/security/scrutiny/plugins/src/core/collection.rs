// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    core::slice::Iter,
    scrutiny::prelude::*,
    scrutiny_utils::zbi::ZbiSection,
    serde::{Deserialize, Serialize},
    std::collections::{HashMap, HashSet},
    uuid::Uuid,
};

/// Captures metadata about where a component was loaded from.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub enum ComponentSource {
    /// Component manifest was not found, but the component was designated as a
    /// service provider in the service mappings in a Component Framework v1
    /// sysmgr config file.
    Inferred,
    /// Component was loaded ZBI bootfs.
    ZbiBootfs,
    /// Component was loaded from a package with the given merkle hash.
    Package(String),
    /// Component was loaded from a package with the given merkle hash. The
    /// package is listed in the static packages index.
    StaticPackage(String),
}

/// Defines a component. Each component has a unique id which is used to link
/// it in the Route table. Each component also has a url and a version. This
/// structure is intended to be lightweight and general purpose if you need to
/// append additional information about a component make another table and
/// index it on the `component.id`.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Component {
    pub id: i32,
    pub url: String,
    pub version: i32,
    pub source: ComponentSource,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Components {
    pub entries: Vec<Component>,
}

impl Components {
    pub fn new(entries: Vec<Component>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, Component> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: Component) {
        self.entries.push(value)
    }
}

impl DataCollection for Components {
    fn uuid() -> Uuid {
        Uuid::parse_str("559f0e26-5ff2-45ce-a5e8-ce0281da8681").unwrap()
    }
    fn collection_name() -> String {
        "Components Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the components found in all packages".to_string()
    }
}

/// Defines a fuchsia package. Each package has a unique url. This provides an
/// expanded meta/contents so you can see all of the files defined in this
/// package.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Package {
    pub url: String,
    pub merkle: String,
    pub contents: HashMap<String, String>,
    pub meta: HashMap<String, String>,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Packages {
    pub entries: Vec<Package>,
}

impl Packages {
    pub fn new(entries: Vec<Package>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, Package> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: Package) {
        self.entries.push(value)
    }
}

impl DataCollection for Packages {
    fn uuid() -> Uuid {
        Uuid::parse_str("80d8b6ab-6ba5-45bc-9461-ba9cc9e0c55b").unwrap()
    }
    fn collection_name() -> String {
        "Packages Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the packages found in the build".to_string()
    }
}

/// A component instance is a specific instantiation of a component. These
/// may run in a particular realm with certain restrictions.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct ComponentInstance {
    pub id: i32,
    pub moniker: String,
    pub component_id: i32,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct ComponentInstances {
    pub entries: Vec<ComponentInstance>,
}

impl ComponentInstances {
    pub fn new(entries: Vec<ComponentInstance>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, ComponentInstance> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: ComponentInstance) {
        self.entries.push(value)
    }
}

impl DataCollection for ComponentInstances {
    fn uuid() -> Uuid {
        Uuid::parse_str("d621f0a5-79e2-432d-8954-f5c9923c0544").unwrap()
    }
    fn collection_name() -> String {
        "Component Instance Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the v1 instances of components found in the build".to_string()
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub enum Capability {
    Service(ServiceCapability),
    Protocol(ProtocolCapability),
    Directory(DirectoryCapability),
    Storage(StorageCapability),
    Runner(RunnerCapability),
    Resolver(ResolverCapability),
    Event(EventCapability),
    EventStream(EventStreamCapability),
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct ServiceCapability {
    pub source_name: String,
}

impl ServiceCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct ProtocolCapability {
    pub source_name: String,
}

impl ProtocolCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct DirectoryCapability {
    pub source_name: String,
}

impl DirectoryCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct StorageCapability {
    pub source_name: String,
}

impl StorageCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct RunnerCapability {
    pub source_name: String,
}

impl RunnerCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct ResolverCapability {
    pub source_name: String,
}

impl ResolverCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct EventCapability {
    pub source_name: String,
}

impl EventCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq, Hash)]
pub struct EventStreamCapability {
    pub source_name: String,
}

impl EventStreamCapability {
    pub fn new(source_name: String) -> Self {
        Self { source_name }
    }
}

/// Defines the manifest data in terms of the component framework version it
/// represents.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub enum ManifestData {
    Version1(String),
    Version2(String),
}

/// Defines a component manifest. The `component_id` maps 1:1 to
/// `component.id` indexes. This is stored in a different table as most queries
/// don't need the raw manifest.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub component_id: i32,
    pub manifest: ManifestData,
    pub uses: Vec<Capability>,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Manifests {
    pub entries: Vec<Manifest>,
}

impl Manifests {
    pub fn new(entries: Vec<Manifest>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, Manifest> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: Manifest) {
        self.entries.push(value)
    }
}

impl DataCollection for Manifests {
    fn uuid() -> Uuid {
        Uuid::parse_str("324da08b-5ab8-43f1-8ff1-4687f32c7712").unwrap()
    }
    fn collection_name() -> String {
        "Manifest Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the v1 & v2 manifests found in the build".to_string()
    }
}

// TODO(benwright) - Add support for "first class" capabilities such as runners,
// resolvers and events.
/// Defines a link between two components. The `src_id` is the `component_instance.id`
/// of the component giving a service or directory to the `dst_id`. The
/// `protocol_id` refers to the Protocol with this link.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Route {
    pub id: i32,
    pub src_id: i32,
    pub dst_id: i32,
    pub service_name: String,
    pub protocol_id: i32,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Routes {
    pub entries: Vec<Route>,
}

impl Routes {
    pub fn new(entries: Vec<Route>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, Route> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: Route) {
        self.entries.push(value)
    }
}

impl DataCollection for Routes {
    fn uuid() -> Uuid {
        Uuid::parse_str("6def84c2-afea-458d-bd36-7dc550e84e90").unwrap()
    }
    fn collection_name() -> String {
        "Routes v1 Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the v1 component routes found in the build".to_string()
    }
}

/// Defines either a FIDL or Directory protocol with some interface name such
/// as fuchshia.foo.Bar and an optional path such as "/dev".
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Protocol {
    pub id: i32,
    pub interface: String,
    pub path: String,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Protocols {
    pub entries: Vec<Protocol>,
}

impl Protocols {
    pub fn new(entries: Vec<Protocol>) -> Self {
        Self { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn iter(&self) -> Iter<'_, Protocol> {
        self.entries.iter()
    }

    pub fn push(&mut self, value: Protocol) {
        self.entries.push(value)
    }
}

impl DataCollection for Protocols {
    fn uuid() -> Uuid {
        Uuid::parse_str("8a14a6ce-3357-43d7-b4fb-7e005062dfda").unwrap()
    }
    fn collection_name() -> String {
        "Protocols v1 Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the v1 protocols found in the build".to_string()
    }
}

/// Defines all of the parsed information in the ZBI.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Zbi {
    // Raw section data for each zbi section. This section isn't serialized to
    // disk because it occupies a large amount of space.
    #[serde(skip)]
    pub sections: Vec<ZbiSection>,
    // File names to data contained in bootfs.
    // TODO(benwright) - Work out how to optimize this for speed.
    #[serde(skip)]
    pub bootfs: HashMap<String, Vec<u8>>,
    pub cmdline: String,
}

impl DataCollection for Zbi {
    fn uuid() -> Uuid {
        Uuid::parse_str("df9ec25f-63b7-4d88-8e79-5ff9deb0afa8").unwrap()
    }
    fn collection_name() -> String {
        "ZBI  Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the items found in the zircon boot image (ZBI) in the update package"
            .to_string()
    }
}

/// Defines all the services exposed by sysmgr.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Sysmgr {
    /// Mapping from service-name -> url.
    pub services: HashMap<String, String>,
    /// Url of sys realm apps, started when the sys realm starts
    pub apps: Vec<String>,
}

impl Sysmgr {
    pub fn new(services: HashMap<String, String>, apps: Vec<String>) -> Self {
        Self { services, apps }
    }

    pub fn iter(&self) -> std::collections::hash_map::Iter<'_, String, String> {
        self.services.iter()
    }
}

impl DataCollection for Sysmgr {
    fn uuid() -> Uuid {
        Uuid::parse_str("422bcffa-395d-4ed6-a9ad-960bb11f79c2").unwrap()
    }
    fn collection_name() -> String {
        "Sysmgr Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains all the service and app mappings found in the sysmgr config".to_string()
    }
}

/// Defines the set of files touched by core plugin data collection. This set
/// can be important when integrating with tooling that demands a complete set
/// of dependencies during tool execution.
#[derive(Debug, Deserialize, PartialEq, Serialize)]
pub struct CoreDataDeps {
    pub deps: HashSet<String>,
}

impl CoreDataDeps {
    pub fn new(deps: HashSet<String>) -> Self {
        Self { deps }
    }
}

impl DataCollection for CoreDataDeps {
    fn uuid() -> Uuid {
        Uuid::parse_str("c6894ef7-ea97-429b-b850-125f77d098ac").unwrap()
    }
    fn collection_name() -> String {
        "Core Data Dependencies".to_string()
    }
    fn collection_description() -> String {
        "Contains a set of paths core data collection read from".to_string()
    }
}

#[cfg(test)]
pub mod testing {
    use super::ComponentSource;

    const FAKE_PKG_MERKLE: &str =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    pub fn fake_component_src_pkg() -> ComponentSource {
        ComponentSource::Package(FAKE_PKG_MERKLE.to_string())
    }
}
