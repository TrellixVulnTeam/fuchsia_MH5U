// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        actions::{Action, ActionKey, ActionSet},
        component::{ComponentInstance, InstanceState, ResolvedInstanceState},
        error::ModelError,
    },
    async_trait::async_trait,
    cm_moniker::InstancedChildMoniker,
    cm_rust::{
        CapabilityDecl, CapabilityName, ChildRef, CollectionDecl, DependencyType, EnvironmentDecl,
        ExposeDecl, OfferDecl, OfferDeclCommon, OfferDirectoryDecl, OfferProtocolDecl,
        OfferResolverDecl, OfferRunnerDecl, OfferServiceDecl, OfferSource, OfferStorageDecl,
        OfferTarget, RegistrationDeclCommon, RegistrationSource, SourceName,
        StorageDirectorySource, UseDecl, UseDirectoryDecl, UseEventDecl, UseProtocolDecl,
        UseServiceDecl, UseSource,
    },
    futures::future::select_all,
    maplit::hashset,
    moniker::ChildMonikerBase,
    std::collections::{HashMap, HashSet},
    std::fmt,
    std::sync::Arc,
};

/// Shuts down all component instances in this component (stops them and guarantees they will never
/// be started again).
pub struct ShutdownAction {}

impl ShutdownAction {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl Action for ShutdownAction {
    type Output = Result<(), ModelError>;
    async fn handle(&self, component: &Arc<ComponentInstance>) -> Self::Output {
        do_shutdown(component).await
    }
    fn key(&self) -> ActionKey {
        ActionKey::Shutdown
    }
}

/// A DependencyNode represents a provider or user of a capability. This
/// may be either a component or a component collection.
#[derive(Debug, PartialEq, Eq, Hash, Clone, PartialOrd, Ord)]
enum DependencyNode {
    Parent,
    Child(String),
    Collection(String),
}

impl DependencyNode {
    fn from_offer_target(target: &OfferTarget) -> Self {
        match target {
            OfferTarget::Child(ChildRef { name, collection: None }) => {
                DependencyNode::Child(name.clone())
            }
            OfferTarget::Child(ChildRef { collection: Some(_), .. }) => {
                // TODO(fxbug.dev/84678): It's safe to not support dynamic
                // children here, because the shutdown logic only sees
                // `OfferDecl`s listed in the static `ComponentDecl`, which
                // doesn't include dynamic offers. So effectively, dynamic
                // offers are ignored. We'll need to properly support dynamic
                // children once dynamic offers are read by the shutdown logic.
                panic!(
                    "A dynamic child appeared in an OfferDecl contained in a ComponentDecl: {:?}",
                    target
                )
            }
            OfferTarget::Collection(collection) => DependencyNode::Collection(collection.clone()),
        }
    }
}

/// Examines a group of StorageDecls looking for one whose name matches the
/// String passed in and whose source is a child. `None` is returned if either
/// no declaration has the specified name or the declaration represents an
/// offer from Self or Parent.
fn find_storage_provider(
    capabilities: &Vec<CapabilityDecl>,
    name: &CapabilityName,
) -> Option<String> {
    for decl in capabilities {
        match decl {
            CapabilityDecl::Storage(decl) if &decl.name == name => match &decl.source {
                StorageDirectorySource::Child(child) => {
                    return Some(child.to_string());
                }
                StorageDirectorySource::Self_ | StorageDirectorySource::Parent => {
                    return None;
                }
            },
            _ => {}
        }
    }
    None
}

async fn shutdown_component(target: ShutdownInfo) -> Result<ParentOrChildMoniker, ModelError> {
    match target.moniker {
        ParentOrChildMoniker::Parent => {
            // TODO: Put the parent in a "shutting down" state so that if it creates new instances
            // after this point, they are created in a shut down state.
            target.component.stop_instance(true, false).await?;
        }
        ParentOrChildMoniker::ChildMoniker(_) => {
            ActionSet::register(target.component, ShutdownAction::new()).await?;
        }
    }

    Ok(target.moniker.clone())
}

/// Structure which holds bidirectional capability maps used during the
/// shutdown process.
struct ShutdownJob {
    /// A map from users of capabilities to the components that provide those
    /// capabilities
    target_to_sources: HashMap<ParentOrChildMoniker, Vec<ParentOrChildMoniker>>,
    /// A map from providers of capabilities to those components which use the
    /// capabilities
    source_to_targets: HashMap<ParentOrChildMoniker, ShutdownInfo>,
}

/// ShutdownJob encapsulates the logic and state require to shutdown a component.
impl ShutdownJob {
    /// Creates a new ShutdownJob by examining the Component's declaration and
    /// runtime state to build up the necessary data structures to stop
    /// components in the component in dependency order.
    pub async fn new(
        instance: &Arc<ComponentInstance>,
        state: &ResolvedInstanceState,
    ) -> ShutdownJob {
        // `dependency_map` represents the dependency relationships between the
        // nodes in this realm (the children, and the parent).
        // `dependency_map` maps server => clients (a.k.a. provider => consumers,
        // or source => targets)
        let dependency_map = process_component_dependencies(state);
        let mut source_to_targets: HashMap<ParentOrChildMoniker, ShutdownInfo> = HashMap::new();

        for (source, targets) in dependency_map {
            let component = match &source {
                ParentOrChildMoniker::Parent => instance.clone(),
                ParentOrChildMoniker::ChildMoniker(moniker) => {
                    state.get_child(&moniker).expect("component not found in children").clone()
                }
            };

            source_to_targets.insert(
                source.clone(),
                ShutdownInfo { moniker: source, dependents: targets, component },
            );
        }
        // `target_to_sources` is the inverse of `source_to_targets`, and maps a target to all of
        // its dependencies. This inverse mapping gives us a way to do quick lookups when updating
        // `source_to_targets` as we shutdown components in execute().
        let mut target_to_sources: HashMap<ParentOrChildMoniker, Vec<ParentOrChildMoniker>> =
            HashMap::new();
        for provider in source_to_targets.values() {
            // All listed siblings are ones that depend on this child
            // and all those siblings must stop before this one
            for consumer in &provider.dependents {
                // Make or update a map entry for the consumer that points to the
                // list of siblings that offer it capabilities
                target_to_sources
                    .entry(consumer.clone())
                    .or_insert(vec![])
                    .push(provider.moniker.clone());
            }
        }
        let new_job = ShutdownJob { source_to_targets, target_to_sources };
        return new_job;
    }

    /// Perform shutdown of the Component that was used to create this ShutdownJob A Component must
    /// wait to shut down until all its children are shut down.  The shutdown procedure looks at
    /// the children, if any, and determines the dependency relationships of the children.
    pub async fn execute(&mut self) -> Result<(), ModelError> {
        // Relationship maps are maintained to track dependencies. A map is
        // maintained both from a Component to its dependents and from a Component to
        // that Component's dependencies. With this dependency tracking, the
        // children of the Component can be shut down progressively in dependency
        // order.
        //
        // The progressive shutdown of Component is performed in this order:
        // Note: These steps continue until the shutdown process is no longer
        // asynchronously waiting for any shut downs to complete.
        //   * Identify the one or more Component that have no dependents
        //   * A shutdown action is set to the identified components. During the
        //     shut down process, the result of the process is received
        //     asynchronously.
        //   * After a Component is shut down, the Component are removed from the list
        //     of dependents of the Component on which they had a dependency.
        //   * The list of Component is checked again to see which Component have no
        //     remaining dependents.

        // Look for any children that have no dependents
        let mut stop_targets = vec![];

        for moniker in self.source_to_targets.keys().map(|key| key.clone()).collect::<Vec<_>>() {
            let no_dependents = {
                let info = self.source_to_targets.get(&moniker).expect("key disappeared from map");
                info.dependents.is_empty()
            };
            if no_dependents {
                stop_targets.push(
                    self.source_to_targets.remove(&moniker).expect("key disappeared from map"),
                );
            }
        }

        let mut futs = vec![];
        // Continue while we have new stop targets or unfinished futures
        while !stop_targets.is_empty() || !futs.is_empty() {
            for target in stop_targets.drain(..) {
                futs.push(Box::pin(shutdown_component(target)));
            }

            let (moniker, _, remaining) = select_all(futs).await;
            futs = remaining;

            let moniker = moniker?;

            // Look up the dependencies of the component that stopped
            match self.target_to_sources.remove(&moniker) {
                Some(sources) => {
                    for source in sources {
                        let ready_to_stop = {
                            if let Some(info) = self.source_to_targets.get_mut(&source) {
                                info.dependents.remove(&moniker);
                                // Have all of this components dependents stopped?
                                info.dependents.is_empty()
                            } else {
                                // The component that provided a capability to
                                // the stopped component doesn't exist or
                                // somehow already stopped. This is unexpected.
                                panic!(
                                    "The component '{:?}' appears to have stopped before its \
                                     dependency '{:?}'",
                                    moniker, source
                                );
                            }
                        };

                        // This components had zero remaining dependents
                        if ready_to_stop {
                            stop_targets.push(
                                self.source_to_targets
                                    .remove(&source)
                                    .expect("A key that was just available has disappeared."),
                            );
                        }
                    }
                }
                None => {
                    // Oh well, component didn't have any dependencies
                }
            }
        }

        // We should have stopped all children, if not probably there is a
        // dependency cycle
        if !self.source_to_targets.is_empty() {
            panic!(
                "Something failed, all children should have been removed! {:?}",
                self.source_to_targets
            );
        }
        Ok(())
    }
}

async fn do_shutdown(component: &Arc<ComponentInstance>) -> Result<(), ModelError> {
    {
        let state = component.lock_state().await;
        {
            let exec_state = component.lock_execution().await;
            if exec_state.is_shut_down() {
                return Ok(());
            }
        }
        match *state {
            InstanceState::Resolved(ref s) => {
                let mut shutdown_job = ShutdownJob::new(component, s).await;
                drop(state);
                Box::pin(shutdown_job.execute()).await?;
                return Ok(());
            }
            InstanceState::New | InstanceState::Discovered | InstanceState::Purged => {}
        }
    }
    // Control flow arrives here if the component isn't resolved.
    // TODO: Put this component in a "shutting down" state so that if it creates new instances
    // after this point, they are created in a shut down state.
    component.stop_instance(true, false).await?;

    Ok(())
}

/// Identifies a component in this realm. This can either be the component
/// itself, or one of its children, identified by an instanced moniker.
///
/// TODO(fxbug.dev/94048): Replace "parent" with "self" here and elsewhere.
#[derive(Debug, Eq, PartialEq, Hash, Clone)]
pub enum ParentOrChildMoniker {
    Parent,
    ChildMoniker(InstancedChildMoniker),
}

/// Used to track information during the shutdown process. The dependents
/// are all the component which must stop before the component represented
/// by this struct.
struct ShutdownInfo {
    // TODO(jmatt) reduce visibility of fields
    /// The identifier for this component
    pub moniker: ParentOrChildMoniker,
    /// The components that this component offers capabilities to
    pub dependents: HashSet<ParentOrChildMoniker>,
    pub component: Arc<ComponentInstance>,
}

impl fmt::Debug for ShutdownInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "moniker: '{:?}'", self.moniker)
    }
}

/// Trait exposing all component state necessary to compute shutdown order.
///
/// This trait largely mirrors `ComponentDecl`, but will reflect changes made to
/// the component's state at runtime (e.g., dynamically created children,
/// dynamic offers).
///
/// In production, this will probably only be implemented for
/// `ResolvedInstanceState`, but exposing this trait allows for easier testing.
pub trait Component {
    /// Current view of this component's `uses` declarations.
    fn uses(&self) -> Vec<UseDecl>;

    /// Current view of this component's `exposes` declarations.
    fn exposes(&self) -> Vec<ExposeDecl>;

    /// Current view of this component's `offers` declarations.
    fn offers(&self) -> Vec<OfferDecl>;

    /// Current view of this component's `capabilities` declarations.
    fn capabilities(&self) -> Vec<CapabilityDecl>;

    /// Current view of this component's `collections` declarations.
    fn collections(&self) -> Vec<CollectionDecl>;

    /// Current view of this component's `environments` declarations.
    fn environments(&self) -> Vec<EnvironmentDecl>;

    /// Returns metadata about each child of this component.
    fn children(&self) -> Vec<Child>;

    /// Filters `offers()` down to remove any runtime-created dynamic offers.
    fn static_offers(&self) -> Vec<OfferDecl> {
        self.offers()
            .into_iter()
            .filter(|o| {
                !matches!(o.target(), OfferTarget::Child(ChildRef { name: _, collection: Some(_) }))
            })
            .collect()
    }

    /// Filters `children()` down to remove any runtime-created dynamic children
    /// (i.e., children in collections).
    fn static_children(&self) -> Vec<Child> {
        self.children().into_iter().filter(|o| o.moniker.collection().is_none()).collect()
    }
}

/// Child metadata necessary to compute shutdown order.
///
/// A `Component` returns information about its children by returning a vector
/// of these.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct Child {
    /// The moniker identifying the name of the child, complete with
    /// `instance_id`.
    pub moniker: InstancedChildMoniker,

    /// Name of the environment associated with this child, if any.
    pub environment_name: Option<String>,

    /// True if this child is static, or dynamic and "live". False if this is a
    /// dynamic component that is being deleted, but has not yet been purged.
    pub is_live: bool,
}

/// Given a set of DependencyNodes, find all the matching ParentOrChildMonikers in the supplied
/// Component.
fn get_shutdown_monikers(
    nodes: &HashSet<DependencyNode>,
    component_state: &impl Component,
) -> HashSet<ParentOrChildMoniker> {
    let mut deps: HashSet<ParentOrChildMoniker> = HashSet::new();
    let all_children = component_state.children();

    for node in nodes {
        match node {
            DependencyNode::Child(name) => {
                deps.insert(ParentOrChildMoniker::ChildMoniker(
                    InstancedChildMoniker::static_child(name.clone()),
                ));
            }
            DependencyNode::Collection(name) => {
                for c in &all_children {
                    match c.moniker.collection() {
                        Some(m) => {
                            if m == name {
                                deps.insert(ParentOrChildMoniker::ChildMoniker(c.moniker.clone()));
                            }
                        }
                        None => {}
                    }
                }
            }
            DependencyNode::Parent => {
                deps.insert(ParentOrChildMoniker::Parent);
            }
        }
    }
    deps
}

/// Maps a dependency node (parent, child or collection) to the nodes that depend on it.
type DependencyMap = HashMap<DependencyNode, HashSet<DependencyNode>>;

/// For a given Component, identify capability dependencies between the
/// component itself and its children. A map is returned which maps from a
/// "source" component (represented by a `ParentOrChildMoniker`) to a set of
/// "target" components to which the source component provides capabilities. The
/// targets must be shut down before the source.
///
/// This function panics if there is a capability routing where either the
/// source or target is not present among the Component's children. Panics are
/// not expected because Component should be validated before this function is
/// called.
pub fn process_component_dependencies(
    instance: &impl Component,
) -> HashMap<ParentOrChildMoniker, HashSet<ParentOrChildMoniker>> {
    let mut dependency_map: DependencyMap = instance
        .static_children()
        .iter()
        .map(|c| (DependencyNode::Child(c.moniker.name().to_string()), HashSet::new()))
        .collect();
    dependency_map.extend(
        instance
            .collections()
            .iter()
            .map(|c| (DependencyNode::Collection(c.name.clone()), HashSet::new())),
    );
    dependency_map.insert(DependencyNode::Parent, HashSet::new());

    get_dependencies_from_offers(instance, &mut dependency_map);
    get_dependencies_from_environments(instance, &mut dependency_map);
    get_dependencies_from_uses(instance, &mut dependency_map);

    let mut expanded = HashMap::new();

    for (source, targets) in dependency_map {
        let expanded_targets = get_shutdown_monikers(&targets, instance);

        let singleton_source = hashset![source];
        // The shutdown target may be a collection, if so this will expand
        // the collection out into a list of all its members, otherwise it
        // contains a single component.
        let matching_sources: Vec<_> =
            get_shutdown_monikers(&singleton_source, instance).into_iter().collect();
        for source in matching_sources {
            expanded.insert(source.clone(), expanded_targets.clone());
        }
    }

    expanded
}

/// Loops through all the use declarations to determine if parents depend on child capabilities,
/// and vice-versa.
fn get_dependencies_from_uses(instance: &impl Component, dependency_map: &mut DependencyMap) {
    // First, find all the children that the parent has a strong dependency on and add them to our
    // dependency map
    let mut children_the_parent_depends_on = HashSet::new();
    for use_ in &instance.uses() {
        let child_name = match use_ {
            UseDecl::Service(UseServiceDecl { source: UseSource::Child(name), .. })
            | UseDecl::Protocol(UseProtocolDecl { source: UseSource::Child(name), .. })
            | UseDecl::Directory(UseDirectoryDecl { source: UseSource::Child(name), .. })
            | UseDecl::Event(UseEventDecl { source: UseSource::Child(name), .. }) => name,
            UseDecl::Service(_)
            | UseDecl::Protocol(_)
            | UseDecl::Directory(_)
            | UseDecl::Event(_)
            | UseDecl::Storage(_)
            | UseDecl::EventStreamDeprecated(_) => {
                // capabilities which cannot or are not used from a child can be ignored.
                continue;
            }
            UseDecl::EventStream(_) => {
                continue;
            }
        };
        match use_ {
            UseDecl::Protocol(UseProtocolDecl { dependency_type, .. })
            | UseDecl::Service(UseServiceDecl { dependency_type, .. })
            | UseDecl::Directory(UseDirectoryDecl { dependency_type, .. })
            | UseDecl::Event(UseEventDecl { dependency_type, .. })
                if dependency_type == &DependencyType::Weak
                    || dependency_type == &DependencyType::WeakForMigration =>
            {
                // Weak dependencies are ignored when determining shutdown ordering
                continue;
            }
            _ => {
                // Any other capability type cannot be marked as weak, so we can proceed
            }
        }
        children_the_parent_depends_on.insert(DependencyNode::Child(child_name.clone()));
    }
    for child_node in children_the_parent_depends_on.iter() {
        match dependency_map.get_mut(&child_node) {
            Some(targets) => {
                targets.insert(DependencyNode::Parent);
            }
            _ => {
                panic!("A dependency went off the map!");
            }
        }
    }

    // TODO(82689): the rest of this function is likely unnecessary, as it deals with children that
    // have no direct dependency links with their parent

    // Next, we want to find any children that the parent transitively depends on through other
    // children, as we'll need to keep those around. These dependencies will be added by the
    // `get_dependencies_from_offers` function, so we don't need to add the dependencies here, but
    // we do need to know which children to not add as dependents of the parent.
    let mut children_the_parent_transitively_depends_on = children_the_parent_depends_on;
    let mut last_loop_added_dependencies = true;
    while last_loop_added_dependencies {
        last_loop_added_dependencies = false;
        for offer in &instance.static_offers() {
            match offer {
                OfferDecl::Protocol(OfferProtocolDecl { dependency_type, .. })
                | OfferDecl::Directory(OfferDirectoryDecl { dependency_type, .. })
                    if dependency_type == &DependencyType::Weak
                        || dependency_type == &DependencyType::WeakForMigration =>
                {
                    // Weak dependencies are ignored when determining shutdown ordering
                    continue;
                }
                _ => {
                    // Any other capability type cannot be marked as weak, so we can proceed
                }
            }
            let offer_target_node = DependencyNode::from_offer_target(offer.target());
            if children_the_parent_transitively_depends_on.contains(&offer_target_node) {
                // The target for this is in our transitive dependency set, so we also
                // transitively depend on the source
                let offer_source_node = match offer.source() {
                    OfferSource::Child(ChildRef { name: source_name, collection: None }) => {
                        DependencyNode::Child(source_name.clone())
                    }
                    OfferSource::Child(ChildRef { collection: Some(_), .. }) => {
                        // TODO(fxbug.dev/84678): It's safe to not support
                        // dynamic children here, because the shutdown logic
                        // only sees `OfferDecl`s listed in the static
                        // `ComponentDecl`, which doesn't include dynamic
                        // offers. So effectively, dynamic offers are ignored.
                        // We'll need to properly support dynamic children once
                        // dynamic offers are read by the shutdown logic.
                        panic!(
                            "A dynamic child appeared in an OfferDecl contained in a \
                            ComponentDecl: {:?}",
                            offer
                        )
                    }
                    OfferSource::Collection(name) => DependencyNode::Collection(name.clone()),
                    OfferSource::Capability(name) => {
                        // The only valid use for an OfferSource::Capability today is for a storage
                        // capability declaration, and its presence should be enforced by manifest
                        // validation.
                        if let Some(s) = find_storage_source(instance, name) {
                            s
                        } else {
                            continue;
                        }
                    }
                    OfferSource::Framework => {
                        // The framework outlives all components, so it doesn't matter if we
                        // transitively depend on it. Proceed to the next offer.
                        continue;
                    }
                    OfferSource::Parent => {
                        // It's irrelevant if we transitively depend on our parent (the child's
                        // grand-parent). Shutdown ordering between us and our parent is handled in
                        // the parent's shutdown ordering logic. Proceed to the next offer.
                        continue;
                    }
                    OfferSource::Self_ => {
                        if let Some(s) = find_storage_source(instance, offer.source_name()) {
                            s
                        } else {
                            // If the `offer` was not for storage, it should be impossible to get
                            // here since that would indicate a cycle. Therefore, getting here
                            // implies that the storage source was the parent, in which case
                            // there's no dependency to add.
                            continue;
                        }
                    }
                };
                last_loop_added_dependencies |=
                    children_the_parent_transitively_depends_on.insert(offer_source_node);
            }
        }
    }

    // Finally, any children that the parent doesn't depend on either transitively or directly are
    // made the parent's dependents.
    let mut parent_dependents = dependency_map
        .remove(&DependencyNode::Parent)
        .expect("Parent was not found in the dependency_map");
    for (source, targets) in dependency_map.iter() {
        if children_the_parent_transitively_depends_on.contains(source) {
            continue;
        }
        if !targets.contains(&DependencyNode::Parent) {
            parent_dependents.insert(source.clone());
        }
    }
    dependency_map.insert(DependencyNode::Parent, parent_dependents);
}

fn find_storage_source(instance: &impl Component, name: &CapabilityName) -> Option<DependencyNode> {
    instance
        .capabilities()
        .into_iter()
        .find_map(|c| match c {
            CapabilityDecl::Storage(s) if &s.name == name => Some(s),
            _ => None,
        })
        .map(|s| match &s.source {
            StorageDirectorySource::Parent => {
                // See comment for OfferSource::Parent
                None
            }
            StorageDirectorySource::Self_ => {
                // See comment for OfferSource::Self_
                panic!("dependency cycle detected when processing transitive child dependencies");
            }
            StorageDirectorySource::Child(name) => Some(DependencyNode::Child(name.clone())),
        })
        .flatten()
}

/// Loops through all the offer declarations to determine which siblings
/// provide capabilities to other siblings.
fn get_dependencies_from_offers(instance: &impl Component, dependency_map: &mut DependencyMap) {
    for dep in &instance.static_offers() {
        // Identify the source and target of the offer, if this offer is
        // actually relevant to shutdown order.
        if let Some((capability_provider, capability_target)) =
            get_dependency_from_offer(instance, dep)
        {
            if !dependency_map.contains_key(&capability_target) {
                panic!(
                    "This capability routing seems invalid, the target \
                     does not exist in this component. Source: {:?} Target: {:?}",
                    capability_provider, capability_target,
                );
            }

            let sibling_deps = dependency_map.get_mut(&capability_provider).expect(&format!(
                "This capability routing seems invalid, the source \
                 does not exist in this component. Source: {:?} Target: {:?}",
                capability_provider, capability_target,
            ));
            sibling_deps.insert(capability_target);
        }
    }
}

/// Extracts a (capability_provider, capability_target) pair from a single
/// `OfferDecl`, or returns `None` if the offer has no impact on shutdown
/// ordering. The `Component` provides context that may be necessary to
/// understand the `OfferDecl`.
fn get_dependency_from_offer(
    instance: &impl Component,
    offer_decl: &OfferDecl,
) -> Option<(DependencyNode, DependencyNode)> {
    // We only care about dependencies where the provider of the dependency is
    // another child, otherwise the capability comes from the parent or
    // component manager itself in which case the relationship is not relevant
    // for ordering here.
    match offer_decl {
        OfferDecl::Protocol(OfferProtocolDecl {
            dependency_type: DependencyType::Strong,
            source,
            target,
            ..
        })
        | OfferDecl::Service(OfferServiceDecl { source, target, .. })
        | OfferDecl::Directory(OfferDirectoryDecl {
            dependency_type: DependencyType::Strong,
            source,
            target,
            ..
        })
        | OfferDecl::Runner(OfferRunnerDecl { source, target, .. })
        | OfferDecl::Resolver(OfferResolverDecl { source, target, .. }) => {
            match source {
                OfferSource::Child(ChildRef { name: source_name, collection: None }) => Some((
                    DependencyNode::Child(source_name.clone()),
                    DependencyNode::from_offer_target(target),
                )),
                OfferSource::Child(ChildRef { collection: Some(_), .. }) => {
                    // TODO(fxbug.dev/84678): It's safe to not support dynamic
                    // children here, because the shutdown logic only sees
                    // `OfferDecl`s listed in the static `ComponentDecl`, which
                    // doesn't include dynamic offers. So effectively, dynamic
                    // offers are ignored. We'll need to properly support
                    // dynamic children once dynamic offers are read by the
                    // shutdown logic.
                    panic!(
                        "A dynamic child appeared in an OfferDecl contained in a ComponentDecl: \
                        {:?}",
                        offer_decl
                    )
                }
                OfferSource::Collection(_) => {
                    // TODO(fxbug.dev/84766): Consider services routed from
                    // collections in shutdown order.
                    None
                }
                OfferSource::Framework
                | OfferSource::Parent
                | OfferSource::Self_
                | OfferSource::Capability(_) => {
                    // Capabilities offered by the parent, routed in from the
                    // component, or provided by the framework (based on some
                    // other capability) are not relevant.
                    None
                }
            }
        }

        OfferDecl::Protocol(OfferProtocolDecl {
            dependency_type: DependencyType::Weak | DependencyType::WeakForMigration,
            ..
        })
        | OfferDecl::Directory(OfferDirectoryDecl {
            dependency_type: DependencyType::Weak | DependencyType::WeakForMigration,
            ..
        }) => {
            // weak dependencies are ignored by this algorithm, because weak
            // dependencies can be broken arbitrarily.
            None
        }

        // Storage is special.
        OfferDecl::Storage(OfferStorageDecl {
            source: OfferSource::Self_,
            source_name,
            target,
            ..
        }) => find_storage_provider(&instance.capabilities(), &source_name).map(
            |storage_source_child_name| {
                (
                    DependencyNode::Child(storage_source_child_name),
                    DependencyNode::from_offer_target(target),
                )
            },
        ),
        OfferDecl::Storage(OfferStorageDecl {
            source:
                OfferSource::Child(_)
                | OfferSource::Parent
                | OfferSource::Capability(_)
                | OfferSource::Collection(_)
                | OfferSource::Framework,
            ..
        }) => {
            // The storage offer is not from `self`, so it can be ignored.
            //
            // NOTE: It may seem weird that storage offers from "child" are
            // ignored, but storage offers that come from children work
            // differently than other kinds of offers. A Storage offer always
            // comes from either `parent` or `self`, but the storage capability
            // _itself_ (see `StorageDecl`) may reference a child. But in that
            // case, the `OfferSource` is still listed as `self`, in which case
            // it is handled above.
            None
        }

        OfferDecl::Event(_) => {
            // Events aren't tracked as dependencies for shutdown.
            None
        }
    }
}

/// Add the provider of an environment registration to the map of environments
/// to components that provide something to the environment.
fn add_env_provider<T>(
    decls: &Vec<T>,
    env_name: String,
    tracking_map: &mut HashMap<&String, Vec<String>>,
) where
    T: RegistrationDeclCommon,
{
    for decl in decls {
        match decl.source() {
            RegistrationSource::Child(source_child) => {
                tracking_map.get_mut(&env_name).unwrap().push(source_child.to_string());
            }
            RegistrationSource::Parent | RegistrationSource::Self_ => {}
        }
    }
}

/// Loops through all the child and collection declarations to determine what siblings provide
/// capabilities to other siblings through an environment.
fn get_dependencies_from_environments(
    instance: &impl Component,
    dependency_map: &mut DependencyMap,
) {
    let mut env_source_children = HashMap::new();

    let environments = instance.environments();
    for env in &environments {
        env_source_children.insert(&env.name, vec![]);
        for source in env.get_dependency_sources() {
            match source {
                cm_rust::DependencySource::Runner { registry: runners } => {
                    add_env_provider(runners, env.name.clone(), &mut env_source_children);
                }

                cm_rust::DependencySource::Resolver { registry: resolvers } => {
                    add_env_provider(resolvers, env.name.clone(), &mut env_source_children);
                }

                cm_rust::DependencySource::Debug { registry: debug_capabilities } => {
                    add_env_provider(
                        debug_capabilities,
                        env.name.clone(),
                        &mut env_source_children,
                    );
                }
            }
        }
    }

    for dest_child in &instance.static_children() {
        if let Some(env_name) = dest_child.environment_name.as_ref() {
            for source_child in env_source_children.get(env_name).expect(&format!(
                "environment `{}` from child `{}` is not a valid environment",
                env_name,
                dest_child.moniker.name(),
            )) {
                dependency_map
                    .entry(DependencyNode::Child((*source_child).clone()))
                    .or_insert(HashSet::new())
                    .insert(DependencyNode::Child(dest_child.moniker.name().to_string()));
            }
        }
    }
    for dest_collection in &instance.collections() {
        if let Some(env_name) = dest_collection.environment.as_ref() {
            for source_child in env_source_children.get(env_name).expect(&format!(
                "environment `{}` from collection `{}` is not a valid environment",
                env_name, dest_collection.name,
            )) {
                dependency_map
                    .entry(DependencyNode::Child((*source_child).clone()))
                    .or_insert(HashSet::new())
                    .insert(DependencyNode::Collection(dest_collection.name.clone()));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::{
            actions::{
                test_utils::{is_executing, is_unresolved},
                StopAction,
            },
            component::StartReason,
            hooks::{self, EventPayload, EventType, Hook, HooksRegistration},
            starter::Starter,
            testing::{
                test_helpers::{
                    component_decl_with_test_runner, default_component_decl,
                    execution_is_shut_down, has_child, ActionsTest, ComponentInfo,
                },
                test_hook::Lifecycle,
            },
        },
        async_trait::async_trait,
        cm_moniker::InstancedAbsoluteMoniker,
        cm_rust::{
            CapabilityName, CapabilityPath, ChildDecl, ComponentDecl, DependencyType, ExposeDecl,
            ExposeProtocolDecl, ExposeSource, ExposeTarget, OfferDecl, OfferProtocolDecl,
            OfferResolverDecl, OfferSource, OfferStorageDecl, OfferTarget, ProtocolDecl,
            StorageDecl, StorageDirectorySource, UseDecl, UseSource,
        },
        cm_rust_testing::{
            ChildDeclBuilder, CollectionDeclBuilder, ComponentDeclBuilder, EnvironmentDeclBuilder,
        },
        fidl_fuchsia_component_decl as fdecl,
        maplit::{hashmap, hashset},
        moniker::ChildMoniker,
        std::collections::HashMap,
        std::{convert::TryFrom, sync::Weak},
        test_case::test_case,
    };

    /// Implementation of `super::Component` based on a `ComponentDecl` and
    /// minimal information about runtime state.
    struct FakeComponent {
        decl: ComponentDecl,
        dynamic_children: Vec<Child>,
        dynamic_offers: Vec<OfferDecl>,
    }

    impl FakeComponent {
        /// Returns a `FakeComponent` with no dynamic children or offers.
        fn from_decl(decl: ComponentDecl) -> Self {
            Self { decl, dynamic_children: vec![], dynamic_offers: vec![] }
        }
    }

    impl Component for FakeComponent {
        fn uses(&self) -> Vec<UseDecl> {
            self.decl.uses.clone()
        }

        fn exposes(&self) -> Vec<ExposeDecl> {
            self.decl.exposes.clone()
        }

        fn offers(&self) -> Vec<OfferDecl> {
            self.decl.offers.iter().cloned().chain(self.dynamic_offers.iter().cloned()).collect()
        }

        fn capabilities(&self) -> Vec<CapabilityDecl> {
            self.decl.capabilities.clone()
        }

        fn collections(&self) -> Vec<cm_rust::CollectionDecl> {
            self.decl.collections.clone()
        }

        fn environments(&self) -> Vec<cm_rust::EnvironmentDecl> {
            self.decl.environments.clone()
        }

        fn children(&self) -> Vec<Child> {
            self.decl
                .children
                .iter()
                .map(|c| Child {
                    moniker: InstancedChildMoniker::static_child(c.name.clone()),
                    environment_name: c.environment.clone(),
                    is_live: true,
                })
                .chain(self.dynamic_children.iter().cloned())
                .collect()
        }
    }

    // TODO(jmatt) Add tests for all capability types

    /// Returns a `ParentOrChildMoniker` for a child by parsing the moniker.
    /// Panics if the moniker is malformed.
    fn child(moniker: &str) -> ParentOrChildMoniker {
        ParentOrChildMoniker::ChildMoniker(InstancedChildMoniker::from(moniker))
    }

    #[test]
    fn test_service_from_parent() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Strong,
            })],
            children: vec![ChildDecl {
                name: "childA".to_string(),
                url: "ignored:///child".to_string(),
                startup: fdecl::StartupMode::Lazy,
                environment: None,
                on_terminate: None,
            }],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0")],
                child("childA:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test_case(DependencyType::Weak)]
    #[test_case(DependencyType::WeakForMigration)]
    fn test_weak_service_from_parent(weak_dep: DependencyType) {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: weak_dep,
            })],
            children: vec![ChildDecl {
                name: "childA".to_string(),
                url: "ignored:///child".to_string(),
                startup: fdecl::StartupMode::Lazy,
                environment: None,
                on_terminate: None,
            }],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0")],
                child("childA:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_service_from_child() {
        let decl = ComponentDecl {
            exposes: vec![ExposeDecl::Protocol(ExposeProtocolDecl {
                target: ExposeTarget::Parent,
                source_name: "serviceFromChild".into(),
                target_name: "serviceFromChild".into(),
                source: ExposeSource::Child("childA".to_string()),
            })],
            children: vec![ChildDecl {
                name: "childA".to_string(),
                url: "ignored:///child".to_string(),
                startup: fdecl::StartupMode::Lazy,
                environment: None,
                on_terminate: None,
            }],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0")],
                child("childA:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_single_dependency() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::Self_,
                    source_name: "serviceParent".into(),
                    target_name: "serviceParent".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
            ],
            children: vec![child_a.clone(), child_b.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![child("childA:0")],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_runner_from_parent() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("env")
                .add_runner(cm_rust::RunnerRegistration {
                    source: RegistrationSource::Parent,
                    source_name: "foo".into(),
                    target_name: "foo".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("env").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_runner_from_child() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("env")
                .add_runner(cm_rust::RunnerRegistration {
                    source: RegistrationSource::Child("childA".to_string()),
                    source_name: "foo".into(),
                    target_name: "foo".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("env").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_runner_from_child_to_collection() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("env")
                .add_runner(cm_rust::RunnerRegistration {
                    source: RegistrationSource::Child("childA".to_string()),
                    source_name: "foo".into(),
                    target_name: "foo".into(),
                })
                .build()],
            collections: vec![CollectionDeclBuilder::new().name("coll").environment("env").build()],
            children: vec![ChildDeclBuilder::new_lazy_child("childA").build()],
            ..default_component_decl()
        };

        let instance = FakeComponent {
            decl,
            dynamic_children: vec![
                // NOTE: The environment must be set in the `Child`, even though
                // it can theoretically be inferred from the collection
                // declaration.
                Child {
                    moniker: "coll:dyn1:0".into(),
                    environment_name: Some("env".to_string()),
                    is_live: true,
                },
                Child {
                    moniker: "coll:dyn2:1".into(),
                    environment_name: Some("env".to_string()),
                    is_live: true,
                },
            ],
            dynamic_offers: vec![],
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("coll:dyn1:0"),
                    child("coll:dyn2:1"),
                ],
                child("childA:0") => hashset![
                    child("coll:dyn1:0"),
                    child("coll:dyn2:1"),
                ],
                child("coll:dyn1:0") => hashset![],
                child("coll:dyn2:1") => hashset![],
            },
            process_component_dependencies(&instance)
        )
    }

    #[test]
    fn test_chained_environments() {
        let decl = ComponentDecl {
            environments: vec![
                EnvironmentDeclBuilder::new()
                    .name("env")
                    .add_runner(cm_rust::RunnerRegistration {
                        source: RegistrationSource::Child("childA".to_string()),
                        source_name: "foo".into(),
                        target_name: "foo".into(),
                    })
                    .build(),
                EnvironmentDeclBuilder::new()
                    .name("env2")
                    .add_runner(cm_rust::RunnerRegistration {
                        source: RegistrationSource::Child("childB".to_string()),
                        source_name: "bar".into(),
                        target_name: "bar".into(),
                    })
                    .build(),
            ],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("env").build(),
                ChildDeclBuilder::new_lazy_child("childC").environment("env2").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0")
                ],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_and_offer() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::static_child("childB".to_string()),
                source_name: "childBOffer".into(),
                target_name: "serviceSibling".into(),
                target: OfferTarget::static_child("childC".to_string()),
                dependency_type: DependencyType::Strong,
            })],
            environments: vec![EnvironmentDeclBuilder::new()
                .name("env")
                .add_runner(cm_rust::RunnerRegistration {
                    source: RegistrationSource::Child("childA".into()),
                    source_name: "foo".into(),
                    target_name: "foo".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("env").build(),
                ChildDeclBuilder::new_lazy_child("childC").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0")
                ],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_resolver_from_parent() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("resolver_env")
                .add_resolver(cm_rust::ResolverRegistration {
                    source: RegistrationSource::Parent,
                    resolver: "foo".into(),
                    scheme: "httweeeeees".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("resolver_env").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_resolver_from_child() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("resolver_env")
                .add_resolver(cm_rust::ResolverRegistration {
                    source: RegistrationSource::Child("childA".to_string()),
                    resolver: "foo".into(),
                    scheme: "httweeeeees".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("resolver_env").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    // add test where B depends on A via environment and C depends on B via environment

    #[test]
    fn test_environment_with_chain_of_resolvers() {
        let decl = ComponentDecl {
            environments: vec![
                EnvironmentDeclBuilder::new()
                    .name("env1")
                    .add_resolver(cm_rust::ResolverRegistration {
                        source: RegistrationSource::Child("childA".to_string()),
                        resolver: "foo".into(),
                        scheme: "httweeeeees".into(),
                    })
                    .build(),
                EnvironmentDeclBuilder::new()
                    .name("env2")
                    .add_resolver(cm_rust::ResolverRegistration {
                        source: RegistrationSource::Child("childB".to_string()),
                        resolver: "bar".into(),
                        scheme: "httweeeeee".into(),
                    })
                    .build(),
            ],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").environment("env1").build(),
                ChildDeclBuilder::new_lazy_child("childC").environment("env2").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0")
                ],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_resolver_and_runner_from_child() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("multi_env")
                .add_resolver(cm_rust::ResolverRegistration {
                    source: RegistrationSource::Child("childA".to_string()),
                    resolver: "foo".into(),
                    scheme: "httweeeeees".into(),
                })
                .add_runner(cm_rust::RunnerRegistration {
                    source: RegistrationSource::Child("childB".to_string()),
                    source_name: "bar".into(),
                    target_name: "bar".into(),
                })
                .build()],
            children: vec![
                ChildDeclBuilder::new_lazy_child("childA").build(),
                ChildDeclBuilder::new_lazy_child("childB").build(),
                ChildDeclBuilder::new_lazy_child("childC").environment("multi_env").build(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0")
                ],
                child("childA:0") => hashset![child("childC:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_environment_with_collection_resolver_from_child() {
        let decl = ComponentDecl {
            environments: vec![EnvironmentDeclBuilder::new()
                .name("resolver_env")
                .add_resolver(cm_rust::ResolverRegistration {
                    source: RegistrationSource::Child("childA".to_string()),
                    resolver: "foo".into(),
                    scheme: "httweeeeees".into(),
                })
                .build()],
            children: vec![ChildDeclBuilder::new_lazy_child("childA").build()],
            collections: vec![CollectionDeclBuilder::new()
                .name("coll")
                .environment("resolver_env")
                .build()],
            ..default_component_decl()
        };

        let instance = FakeComponent {
            decl,
            dynamic_children: vec![
                // NOTE: The environment must be set in the `Child`, even though
                // it can theoretically be inferred from the collection declaration.
                Child {
                    moniker: "coll:dyn1:0".into(),
                    environment_name: Some("resolver_env".to_string()),
                    is_live: true,
                },
                Child {
                    moniker: "coll:dyn2:1".into(),
                    environment_name: Some("resolver_env".to_string()),
                    is_live: true,
                },
            ],
            dynamic_offers: vec![],
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("coll:dyn1:0"),
                    child("coll:dyn2:1"),
                ],
                child("childA:0") => hashset![child("coll:dyn1:0"), child("coll:dyn2:1")],
                child("coll:dyn1:0") => hashset![],
                child("coll:dyn2:1") => hashset![],
            },
            process_component_dependencies(&instance)
        )
    }

    #[test_case(DependencyType::Weak)]
    #[test_case(DependencyType::WeakForMigration)]
    fn test_single_weak_dependency(weak_dep: DependencyType) {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::Self_,
                    source_name: "serviceParent".into(),
                    target_name: "serviceParent".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: weak_dep.clone(),
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: weak_dep.clone(),
                }),
            ],
            children: vec![child_a.clone(), child_b.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_multiple_dependencies_same_source() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::Self_,
                    source_name: "serviceParent".into(),
                    target_name: "serviceParent".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBOtherOffer".into(),
                    target_name: "serviceOtherSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
            ],
            children: vec![child_a.clone(), child_b.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![child("childA:0")],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_multiple_dependents_same_source() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_c = ChildDecl {
            name: "childC".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBToC".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childC".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
            ],
            children: vec![child_a.clone(), child_b.clone(), child_c.clone()],

            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0"),
                ],
                child("childA:0") => hashset![],
                child("childB:0") => hashset![child("childA:0"), child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test_case(DependencyType::Weak)]
    #[test_case(DependencyType::WeakForMigration)]
    fn test_multiple_dependencies(weak_dep: DependencyType) {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_c = ChildDecl {
            name: "childC".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childA".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childC".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBToC".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childC".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childC".to_string()),
                    source_name: "childCToA".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                    dependency_type: weak_dep,
                }),
            ],
            children: vec![child_a.clone(), child_b.clone(), child_c.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0"),
                ],
                child("childA:0") => hashset![child("childC:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_component_is_source_and_target() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_c = ChildDecl {
            name: "childC".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childA".to_string()),
                    source_name: "childBOffer".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childB".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBToC".into(),
                    target_name: "serviceSibling".into(),
                    target: OfferTarget::static_child("childC".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
            ],
            children: vec![child_a.clone(), child_b.clone(), child_c.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                    child("childA:0"),
                    child("childB:0"),
                    child("childC:0"),
                ],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![child("childC:0")],
                child("childC:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    /// Tests a graph that looks like the below, tildes indicate a
    /// capability route. Route point toward the target of the capability
    /// offer. The manifest constructed is for 'P'.
    ///       P
    ///    ___|___
    ///  /  / | \  \
    /// e<~c<~a~>b~>d
    ///     \      /
    ///      *>~~>*
    #[test]
    fn test_complex_routing() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_c = ChildDecl {
            name: "childC".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_d = ChildDecl {
            name: "childD".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_e = ChildDecl {
            name: "childE".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childA".to_string()),
                    source_name: "childAService".into(),
                    target_name: "childAService".into(),
                    target: OfferTarget::static_child("childB".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childA".to_string()),
                    source_name: "childAService".into(),
                    target_name: "childAService".into(),
                    target: OfferTarget::static_child("childC".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childB".to_string()),
                    source_name: "childBService".into(),
                    target_name: "childBService".into(),
                    target: OfferTarget::static_child("childD".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childC".to_string()),
                    source_name: "childAService".into(),
                    target_name: "childAService".into(),
                    target: OfferTarget::static_child("childD".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
                OfferDecl::Protocol(OfferProtocolDecl {
                    source: OfferSource::static_child("childC".to_string()),
                    source_name: "childAService".into(),
                    target_name: "childAService".into(),
                    target: OfferTarget::static_child("childE".to_string()),
                    dependency_type: DependencyType::Strong,
                }),
            ],
            children: vec![
                child_a.clone(),
                child_b.clone(),
                child_c.clone(),
                child_d.clone(),
                child_e.clone(),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![
                   child("childA:0"),
                   child("childB:0"),
                   child("childC:0"),
                   child("childD:0"),
                   child("childE:0"),
                ],
                child("childA:0") => hashset![child("childB:0"), child("childC:0")],
                child("childB:0") => hashset![child("childD:0")],
                child("childC:0") => hashset![child("childD:0"), child("childE:0")],
                child("childD:0") => hashset![],
                child("childE:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    #[should_panic]
    fn test_target_does_not_exist() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        // This declaration is invalid because the offer target doesn't exist
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::static_child("childA".to_string()),
                source_name: "childBOffer".into(),
                target_name: "serviceSibling".into(),
                target: OfferTarget::static_child("childB".to_string()),
                dependency_type: DependencyType::Strong,
            })],
            children: vec![child_a.clone()],
            ..default_component_decl()
        };

        process_component_dependencies(&FakeComponent::from_decl(decl));
    }

    #[test]
    #[should_panic]
    fn test_source_does_not_exist() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        // This declaration is invalid because the offer target doesn't exist
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::static_child("childB".to_string()),
                source_name: "childBOffer".into(),
                target_name: "serviceSibling".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Strong,
            })],
            children: vec![child_a.clone()],
            ..default_component_decl()
        };

        process_component_dependencies(&FakeComponent::from_decl(decl));
    }

    #[test]
    fn test_use_from_child() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Weak,
            })],
            children: vec![ChildDecl {
                name: "childA".to_string(),
                url: "ignored:///child".to_string(),
                startup: fdecl::StartupMode::Lazy,
                environment: None,
                on_terminate: None,
            }],
            uses: vec![UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Child("childA".to_string()),
                source_name: "test.protocol".into(),
                target_path: CapabilityPath::try_from("/svc/test.protocol").unwrap(),
                dependency_type: DependencyType::Strong,
            })],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![],
                child("childA:0") => hashset![ParentOrChildMoniker::Parent],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_use_from_some_children() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Weak,
            })],
            children: vec![
                ChildDecl {
                    name: "childA".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
                ChildDecl {
                    name: "childB".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
            ],
            uses: vec![UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Child("childA".to_string()),
                source_name: "test.protocol".into(),
                target_path: CapabilityPath::try_from("/svc/test.protocol").unwrap(),
                dependency_type: DependencyType::Strong,
            })],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                // childB is a dependent because we consider all children
                // dependent, unless the parent uses something from the child.
                ParentOrChildMoniker::Parent => hashset![child("childB:0")],
                child("childA:0") => hashset![ParentOrChildMoniker::Parent],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        );
    }

    #[test]
    fn test_use_from_child_offer_storage() {
        let decl = ComponentDecl {
            capabilities: vec![
                CapabilityDecl::Storage(StorageDecl {
                    name: "cdata".into(),
                    source: StorageDirectorySource::Child("childB".to_string()),
                    backing_dir: "directory".into(),
                    subdir: None,
                    storage_id: fdecl::StorageId::StaticInstanceIdOrMoniker,
                }),
                CapabilityDecl::Storage(StorageDecl {
                    name: "pdata".into(),
                    source: StorageDirectorySource::Parent,
                    backing_dir: "directory".into(),
                    subdir: None,
                    storage_id: fdecl::StorageId::StaticInstanceIdOrMoniker,
                }),
            ],
            offers: vec![
                OfferDecl::Storage(OfferStorageDecl {
                    source: OfferSource::Self_,
                    source_name: "cdata".into(),
                    target_name: "cdata".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                }),
                OfferDecl::Storage(OfferStorageDecl {
                    source: OfferSource::Self_,
                    source_name: "pdata".into(),
                    target_name: "pdata".into(),
                    target: OfferTarget::static_child("childA".to_string()),
                }),
            ],
            children: vec![
                ChildDecl {
                    name: "childA".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
                ChildDecl {
                    name: "childB".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
            ],
            uses: vec![UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Child("childA".to_string()),
                source_name: "test.protocol".into(),
                target_path: CapabilityPath::try_from("/svc/test.protocol").unwrap(),
                dependency_type: DependencyType::Strong,
            })],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![],
                child("childA:0") => hashset![ParentOrChildMoniker::Parent],
                child("childB:0") => hashset![child("childA:0")],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_use_from_child_weak() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Strong,
            })],
            children: vec![ChildDecl {
                name: "childA".to_string(),
                url: "ignored:///child".to_string(),
                startup: fdecl::StartupMode::Lazy,
                environment: None,
                on_terminate: None,
            }],
            uses: vec![UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Child("childA".to_string()),
                source_name: "test.protocol".into(),
                target_path: CapabilityPath::try_from("/svc/test.protocol").unwrap(),
                dependency_type: DependencyType::Weak,
            })],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0")],
                child("childA:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_use_from_some_children_weak() {
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Self_,
                source_name: "serviceParent".into(),
                target_name: "serviceParent".into(),
                target: OfferTarget::static_child("childA".to_string()),
                dependency_type: DependencyType::Weak,
            })],
            children: vec![
                ChildDecl {
                    name: "childA".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
                ChildDecl {
                    name: "childB".to_string(),
                    url: "ignored:///child".to_string(),
                    startup: fdecl::StartupMode::Lazy,
                    environment: None,
                    on_terminate: None,
                },
            ],
            uses: vec![
                UseDecl::Protocol(UseProtocolDecl {
                    source: UseSource::Child("childA".to_string()),
                    source_name: "test.protocol".into(),
                    target_path: CapabilityPath::try_from("/svc/test.protocol").unwrap(),
                    dependency_type: DependencyType::Strong,
                }),
                UseDecl::Protocol(UseProtocolDecl {
                    source: UseSource::Child("childB".to_string()),
                    source_name: "test.protocol2".into(),
                    target_path: CapabilityPath::try_from("/svc/test.protocol2").unwrap(),
                    dependency_type: DependencyType::Weak,
                }),
            ],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                // childB is a dependent because its use-from-child has a 'weak' dependency.
                ParentOrChildMoniker::Parent => hashset![child("childB:0")],
                child("childA:0") => hashset![ParentOrChildMoniker::Parent],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[test]
    fn test_resolver_capability_creates_dependency() {
        let child_a = ChildDecl {
            name: "childA".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let child_b = ChildDecl {
            name: "childB".to_string(),
            url: "ignored:///child".to_string(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
        };
        let decl = ComponentDecl {
            offers: vec![OfferDecl::Resolver(OfferResolverDecl {
                source: OfferSource::static_child("childA".to_string()),
                source_name: CapabilityName::try_from("resolver").unwrap(),
                target_name: CapabilityName::try_from("resolver").unwrap(),
                target: OfferTarget::static_child("childB".to_string()),
            })],
            children: vec![child_a.clone(), child_b.clone()],
            ..default_component_decl()
        };

        pretty_assertions::assert_eq!(
            hashmap! {
                ParentOrChildMoniker::Parent => hashset![child("childA:0"), child("childB:0")],
                child("childA:0") => hashset![child("childB:0")],
                child("childB:0") => hashset![],
            },
            process_component_dependencies(&FakeComponent::from_decl(decl))
        )
    }

    #[fuchsia::test]
    async fn action_shutdown_blocks_stop() {
        let test = ActionsTest::new("root", vec![], None).await;
        let component = test.model.root().clone();
        let mut action_set = component.lock_actions().await;

        // Register some actions, and get notifications. Use `register_inner` so we can register
        // the action without immediately running it.
        let (task1, nf1) = action_set.register_inner(&component, ShutdownAction::new());
        let (task2, nf2) = action_set.register_inner(&component, StopAction::new(false, false));

        drop(action_set);

        // Complete actions, while checking futures.
        ActionSet::finish(&component, &ActionKey::Shutdown).await;

        // nf2 should be blocked on task1 completing.
        assert!(nf1.fut.peek().is_none());
        assert!(nf2.fut.peek().is_none());
        task1.unwrap().tx.send(Ok(())).unwrap();
        task2.unwrap().spawn();
        nf1.await.unwrap();
        nf2.await.unwrap();
    }

    #[fuchsia::test]
    async fn action_shutdown_stop_stop() {
        let test = ActionsTest::new("root", vec![], None).await;
        let component = test.model.root().clone();
        let mut action_set = component.lock_actions().await;

        // Register some actions, and get notifications. Use `register_inner` so we can register
        // the action without immediately running it.
        let (task1, nf1) = action_set.register_inner(&component, ShutdownAction::new());
        let (task2, nf2) = action_set.register_inner(&component, StopAction::new(false, false));
        let (task3, nf3) = action_set.register_inner(&component, StopAction::new(false, false));

        drop(action_set);

        // Complete actions, while checking notifications.
        ActionSet::finish(&component, &ActionKey::Shutdown).await;

        // nf2 and nf3 should be blocked on task1 completing.
        assert!(nf1.fut.peek().is_none());
        assert!(nf2.fut.peek().is_none());
        task1.unwrap().tx.send(Ok(())).unwrap();
        task2.unwrap().spawn();
        assert!(task3.is_none());
        nf1.await.unwrap();
        nf2.await.unwrap();
        nf3.await.unwrap();
    }

    #[fuchsia::test]
    async fn shutdown_one_component() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        // Start the component. This should cause the component to have an `Execution`.
        let component = test.look_up(vec!["a"].into()).await;
        test.model
            .start_instance(&component.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component).await);
        let a_info = ComponentInfo::new(component.clone()).await;

        // Register shutdown action, and wait for it. Component should shut down (no more
        // `Execution`).
        ActionSet::register(a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        a_info.check_is_shut_down(&test.runner).await;

        // Trying to start the component should fail because it's shut down.
        test.model
            .start_instance(&a_info.component.abs_moniker, &StartReason::Eager)
            .await
            .expect_err("successfully bound to a after shutdown");

        // Shut down the component again. This succeeds, but has no additional effect.
        ActionSet::register(a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        a_info.check_is_shut_down(&test.runner).await;
    }

    #[fuchsia::test]
    async fn shutdown_collection() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("container").build()),
            (
                "container",
                ComponentDeclBuilder::new()
                    .add_transient_collection("coll")
                    .add_lazy_child("c")
                    .build(),
            ),
            ("a", component_decl_with_test_runner()),
            ("b", component_decl_with_test_runner()),
            ("c", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, Some(vec!["container"].into())).await;

        // Create dynamic instances in "coll".
        test.create_dynamic_child("coll", "a").await;
        test.create_dynamic_child("coll", "b").await;

        // Start the components. This should cause them to have an `Execution`.
        let component_container = test.look_up(vec!["container"].into()).await;
        let component_a = test.look_up(vec!["container", "coll:a"].into()).await;
        let component_b = test.look_up(vec!["container", "coll:b"].into()).await;
        let component_c = test.look_up(vec!["container", "c"].into()).await;
        test.model
            .start_instance(&component_container.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start container");
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start coll:a");
        test.model
            .start_instance(&component_b.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start coll:b");
        test.model
            .start_instance(&component_c.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start coll:b");
        assert!(is_executing(&component_container).await);
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(has_child(&component_container, "coll:a:1").await);
        assert!(has_child(&component_container, "coll:b:2").await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_container_info = ComponentInfo::new(component_container).await;

        // Register shutdown action, and wait for it. Components should shut down (no more
        // `Execution`). Also, the instances in the collection should have been destroyed because
        // they were transient.
        ActionSet::register(component_container_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_container_info.check_is_shut_down(&test.runner).await;
        assert!(!has_child(&component_container_info.component, "coll:a:1").await);
        assert!(!has_child(&component_container_info.component, "coll:b:2").await);
        assert!(has_child(&component_container_info.component, "c:0").await);
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;

        // Verify events.
        {
            let mut events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) | Lifecycle::Destroy(_) => true,
                    _ => false,
                })
                .collect();
            // The leaves could be stopped in any order.
            let mut next: Vec<_> = events.drain(0..3).collect();
            next.sort_unstable();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["container:0", "c:0"].into()),
                Lifecycle::Stop(vec!["container:0", "coll:a:1"].into()),
                Lifecycle::Stop(vec!["container:0", "coll:b:2"].into()),
            ];
            assert_eq!(next, expected);

            // These components were destroyed because they lived in a transient collection.
            let mut next: Vec<_> = events.drain(0..2).collect();
            next.sort_unstable();
            let expected: Vec<_> = vec![
                Lifecycle::Destroy(vec!["container:0", "coll:a:1"].into()),
                Lifecycle::Destroy(vec!["container:0", "coll:b:2"].into()),
            ];
            assert_eq!(next, expected);
        }
    }

    #[fuchsia::test]
    async fn shutdown_not_started() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_lazy_child("b").build()),
            ("b", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        assert!(!is_executing(&component_a).await);
        assert!(!is_executing(&component_b).await);

        // Register shutdown action on "a", and wait for it.
        ActionSet::register(component_a.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        assert!(execution_is_shut_down(&component_a).await);
        assert!(execution_is_shut_down(&component_b).await);

        // Now "a" is shut down. There should be no events though because the component was
        // never started.
        ActionSet::register(component_a.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        assert!(execution_is_shut_down(&component_a).await);
        assert!(execution_is_shut_down(&component_b).await);
        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            assert_eq!(events, Vec::<Lifecycle>::new());
        }
    }

    #[fuchsia::test]
    async fn shutdown_not_resolved() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_lazy_child("b").build()),
            ("b", ComponentDeclBuilder::new().add_lazy_child("c").build()),
            ("c", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);

        // Register shutdown action on "a", and wait for it.
        ActionSet::register(component_a.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        assert!(execution_is_shut_down(&component_a).await);
        // Get component without resolving it.
        let component_b = {
            let state = component_a.lock_state().await;
            match *state {
                InstanceState::Resolved(ref s) => {
                    s.get_live_child(&ChildMoniker::from("b")).expect("child b not found")
                }
                _ => panic!("not resolved"),
            }
        };
        assert!(execution_is_shut_down(&component_b).await);
        assert!(is_unresolved(&component_b).await);

        // Now "a" is shut down. There should be no event for "b" because it was never started
        // (or resolved).
        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            assert_eq!(events, vec![Lifecycle::Stop(vec!["a:0"].into())]);
        }
    }

    /// Shut down `a`:
    ///  a
    ///   \
    ///    b
    ///   / \
    ///  c   d
    #[fuchsia::test]
    async fn shutdown_hierarchy() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            ("b", ComponentDeclBuilder::new().add_eager_child("c").add_eager_child("d").build()),
            ("c", component_decl_with_test_runner()),
            ("d", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "b", "c"].into()).await;
        let component_d = test.look_up(vec!["a", "b", "d"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(is_executing(&component_d).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        {
            let mut events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let mut first: Vec<_> = events.drain(0..2).collect();
            first.sort_unstable();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0", "d:0"].into()),
            ];
            assert_eq!(first, expected);
            assert_eq!(
                events,
                vec![
                    Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                    Lifecycle::Stop(vec!["a:0"].into())
                ]
            );
        }
    }

    /// Shut down `a`:
    ///   a
    ///    \
    ///     b
    ///   / | \
    ///  c<-d->e
    /// In this case C and E use a service provided by d
    #[fuchsia::test]
    async fn shutdown_with_multiple_deps() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_eager_child("c")
                    .add_eager_child("d")
                    .add_eager_child("e")
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("c".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("e".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "d",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceD".into(),
                        source_path: Some("/svc/serviceD".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "e",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "b", "c"].into()).await;
        let component_d = test.look_up(vec!["a", "b", "d"].into()).await;
        let component_e = test.look_up(vec!["a", "b", "e"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(is_executing(&component_d).await);
        assert!(is_executing(&component_e).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;
        let component_e_info = ComponentInfo::new(component_e).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        component_e_info.check_is_shut_down(&test.runner).await;

        {
            let mut events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let mut first: Vec<_> = events.drain(0..2).collect();
            first.sort_unstable();
            let mut expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0", "e:0"].into()),
            ];
            assert_eq!(first, expected);

            let next: Vec<_> = events.drain(0..1).collect();
            expected = vec![Lifecycle::Stop(vec!["a:0", "b:0", "d:0"].into())];
            assert_eq!(next, expected);

            assert_eq!(
                events,
                vec![
                    Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                    Lifecycle::Stop(vec!["a:0"].into())
                ]
            );
        }
    }

    /// Shut down `a`:
    ///    a
    ///     \
    ///      b
    ///   / / \  \
    ///  c<-d->e->f
    /// In this case C and E use a service provided by D and
    /// F uses a service provided by E, shutdown order should be
    /// {F}, {C, E}, {D}, {B}, {A}
    /// Note that C must stop before D, but may stop before or after
    /// either of F and E.
    #[fuchsia::test]
    async fn shutdown_with_multiple_out_and_longer_chain() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_eager_child("c")
                    .add_eager_child("d")
                    .add_eager_child("e")
                    .add_eager_child("f")
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("c".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("e".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("e".to_string()),
                        source_name: "serviceE".into(),
                        target_name: "serviceE".into(),
                        target: OfferTarget::static_child("f".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "d",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceD".into(),
                        source_path: Some("/svc/serviceD".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "e",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceE".into(),
                        source_path: Some("/svc/serviceE".parse().unwrap()),
                    })
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceE".into(),
                        target_name: "serviceE".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "f",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceE".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceE").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
        ];
        let moniker_a: InstancedAbsoluteMoniker = vec!["a:0"].into();
        let moniker_b: InstancedAbsoluteMoniker = vec!["a:0", "b:0"].into();
        let moniker_c: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "c:0"].into();
        let moniker_d: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "d:0"].into();
        let moniker_e: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "e:0"].into();
        let moniker_f: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "f:0"].into();
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(moniker_a.to_absolute_moniker()).await;
        let component_b = test.look_up(moniker_b.to_absolute_moniker()).await;
        let component_c = test.look_up(moniker_c.to_absolute_moniker()).await;
        let component_d = test.look_up(moniker_d.to_absolute_moniker()).await;
        let component_e = test.look_up(moniker_e.to_absolute_moniker()).await;
        let component_f = test.look_up(moniker_f.to_absolute_moniker()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(is_executing(&component_d).await);
        assert!(is_executing(&component_e).await);
        assert!(is_executing(&component_f).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;
        let component_e_info = ComponentInfo::new(component_e).await;
        let component_f_info = ComponentInfo::new(component_f).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        component_e_info.check_is_shut_down(&test.runner).await;
        component_f_info.check_is_shut_down(&test.runner).await;

        let mut comes_after: HashMap<InstancedAbsoluteMoniker, Vec<InstancedAbsoluteMoniker>> =
            HashMap::new();
        comes_after.insert(moniker_a.clone(), vec![moniker_b.clone()]);
        // technically we could just depend on 'D' since it is the last of b's
        // children, but we add all the children for resilence against the
        // future
        comes_after.insert(
            moniker_b.clone(),
            vec![moniker_c.clone(), moniker_d.clone(), moniker_e.clone(), moniker_f.clone()],
        );
        comes_after.insert(moniker_d.clone(), vec![moniker_c.clone(), moniker_e.clone()]);
        comes_after.insert(moniker_c.clone(), vec![]);
        comes_after.insert(moniker_e.clone(), vec![moniker_f.clone()]);
        comes_after.insert(moniker_f.clone(), vec![]);
        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();

            for e in events {
                match e {
                    Lifecycle::Stop(moniker) => match comes_after.remove(&moniker) {
                        Some(dependents) => {
                            for d in dependents {
                                if comes_after.contains_key(&d) {
                                    panic!("{} stopped before its dependent {}", moniker, d);
                                }
                            }
                        }
                        None => {
                            panic!("{} was unknown or shut down more than once", moniker);
                        }
                    },
                    _ => {
                        panic!("Unexpected lifecycle type");
                    }
                }
            }
        }
    }

    /// Shut down `a`:
    ///           a
    ///
    ///           |
    ///
    ///     +---- b ----+
    ///    /             \
    ///   /     /   \     \
    ///
    ///  c <~~ d ~~> e ~~> f
    ///          \       /
    ///           +~~>~~+
    /// In this case C and E use a service provided by D and
    /// F uses a services provided by E and D, shutdown order should be F must
    /// stop before E and {C,E,F} must stop before D. C may stop before or
    /// after either of {F, E}.
    #[fuchsia::test]
    async fn shutdown_with_multiple_out_multiple_in() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_eager_child("c")
                    .add_eager_child("d")
                    .add_eager_child("e")
                    .add_eager_child("f")
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("c".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("e".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("d".to_string()),
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: OfferTarget::static_child("f".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("e".to_string()),
                        source_name: "serviceE".into(),
                        target_name: "serviceE".into(),
                        target: OfferTarget::static_child("f".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "d",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceD".into(),
                        source_path: Some("/svc/serviceD".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceD".into(),
                        target_name: "serviceD".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "e",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceE".into(),
                        source_path: Some("/svc/serviceE".parse().unwrap()),
                    })
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceE".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceE").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceE".into(),
                        target_name: "serviceE".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "f",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceE".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceE").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceD".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceD").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
        ];
        let moniker_a: InstancedAbsoluteMoniker = vec!["a:0"].into();
        let moniker_b: InstancedAbsoluteMoniker = vec!["a:0", "b:0"].into();
        let moniker_c: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "c:0"].into();
        let moniker_d: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "d:0"].into();
        let moniker_e: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "e:0"].into();
        let moniker_f: InstancedAbsoluteMoniker = vec!["a:0", "b:0", "f:0"].into();
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(moniker_a.to_absolute_moniker()).await;
        let component_b = test.look_up(moniker_b.to_absolute_moniker()).await;
        let component_c = test.look_up(moniker_c.to_absolute_moniker()).await;
        let component_d = test.look_up(moniker_d.to_absolute_moniker()).await;
        let component_e = test.look_up(moniker_e.to_absolute_moniker()).await;
        let component_f = test.look_up(moniker_f.to_absolute_moniker()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(is_executing(&component_d).await);
        assert!(is_executing(&component_e).await);
        assert!(is_executing(&component_f).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;
        let component_e_info = ComponentInfo::new(component_e).await;
        let component_f_info = ComponentInfo::new(component_f).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        component_e_info.check_is_shut_down(&test.runner).await;
        component_f_info.check_is_shut_down(&test.runner).await;

        let mut comes_after: HashMap<InstancedAbsoluteMoniker, Vec<InstancedAbsoluteMoniker>> =
            HashMap::new();
        comes_after.insert(moniker_a.clone(), vec![moniker_b.clone()]);
        // technically we could just depend on 'D' since it is the last of b's
        // children, but we add all the children for resilence against the
        // future
        comes_after.insert(
            moniker_b.clone(),
            vec![moniker_c.clone(), moniker_d.clone(), moniker_e.clone(), moniker_f.clone()],
        );
        comes_after.insert(
            moniker_d.clone(),
            vec![moniker_c.clone(), moniker_e.clone(), moniker_f.clone()],
        );
        comes_after.insert(moniker_c.clone(), vec![]);
        comes_after.insert(moniker_e.clone(), vec![moniker_f.clone()]);
        comes_after.insert(moniker_f.clone(), vec![]);
        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();

            for e in events {
                match e {
                    Lifecycle::Stop(moniker) => {
                        let dependents = comes_after.remove(&moniker).expect(&format!(
                            "{} was unknown or shut down more than once",
                            moniker
                        ));
                        for d in dependents {
                            if comes_after.contains_key(&d) {
                                panic!("{} stopped before its dependent {}", moniker, d);
                            }
                        }
                    }
                    _ => {
                        panic!("Unexpected lifecycle type");
                    }
                }
            }
        }
    }

    /// Shut down `a`:
    ///  a
    ///   \
    ///    b
    ///   / \
    ///  c-->d
    /// In this case D uses a resource exposed by C
    #[fuchsia::test]
    async fn shutdown_with_dependency() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_eager_child("c")
                    .add_eager_child("d")
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("c".to_string()),
                        source_name: "serviceC".into(),
                        target_name: "serviceC".into(),
                        target: OfferTarget::static_child("d".to_string()),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceC".into(),
                        source_path: Some("/svc/serviceC".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceC".into(),
                        target_name: "serviceC".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            (
                "d",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceC".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceC").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "b", "c"].into()).await;
        let component_d = test.look_up(vec!["a", "b", "d"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up and dependency order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;

        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0", "d:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                Lifecycle::Stop(vec!["a:0"].into()),
            ];
            assert_eq!(events, expected);
        }
    }

    /// Shut down `a`:
    ///   a     (a use b)
    ///  / \
    /// b    c
    /// In this case, c shuts down first, then a, then b.
    #[fuchsia::test]
    async fn shutdown_use_from_child() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_eager_child("b")
                    .add_eager_child("c")
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Child("b".to_string()),
                        source_name: "serviceC".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceC").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceC".into(),
                        source_path: Some("/svc/serviceC".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceC".into(),
                        target_name: "serviceC".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            ("c", ComponentDeclBuilder::new().build()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "c"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;

        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0"].into()),
            ];
            assert_eq!(events, expected);
        }
    }

    /// Shut down `a`:
    ///   a     (a use b, and b use c)
    ///  / \
    /// b    c
    /// In this case, a shuts down first, then b, then c.
    #[fuchsia::test]
    async fn shutdown_use_from_child_that_uses_from_sibling() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_eager_child("b")
                    .add_eager_child("c")
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Child("b".to_string()),
                        source_name: "serviceB".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceB").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .offer(OfferDecl::Protocol(OfferProtocolDecl {
                        source: OfferSource::static_child("c".to_string()),
                        source_name: "serviceC".into(),
                        target: OfferTarget::static_child("b".to_string()),
                        target_name: "serviceB".into(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceB".into(),
                        source_path: Some("/svc/serviceB".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceB".into(),
                        target_name: "serviceB".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Parent,
                        source_name: "serviceC".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceC").unwrap(),
                        dependency_type: DependencyType::Strong,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceC".into(),
                        source_path: Some("/svc/serviceC".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceC".into(),
                        target_name: "serviceC".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "c"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;

        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                Lifecycle::Stop(vec!["a:0", "c:0"].into()),
            ];
            assert_eq!(events, expected);
        }
    }

    /// Shut down `a`:
    ///   a     (a use b weak)
    ///  / \
    /// b    c
    /// In this case, b or c shutdown first (arbitrary order), then a.
    #[fuchsia::test]
    async fn shutdown_use_from_child_weak() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_eager_child("b")
                    .add_eager_child("c")
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Child("b".to_string()),
                        source_name: "serviceC".into(),
                        target_path: CapabilityPath::try_from("/svc/serviceC").unwrap(),
                        dependency_type: DependencyType::Weak,
                    }))
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .protocol(ProtocolDecl {
                        name: "serviceC".into(),
                        source_path: Some("/svc/serviceC".parse().unwrap()),
                    })
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_name: "serviceC".into(),
                        target_name: "serviceC".into(),
                        target: ExposeTarget::Parent,
                    }))
                    .build(),
            ),
            ("c", ComponentDeclBuilder::new().build()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "c"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;

        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            let expected1: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                Lifecycle::Stop(vec!["a:0"].into()),
            ];
            let expected2: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                Lifecycle::Stop(vec!["a:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0"].into()),
            ];
            assert!(events == expected1 || events == expected2);
        }
    }

    /// Shut down `b`:
    ///  a
    ///   \
    ///    b
    ///     \
    ///      b
    ///       \
    ///      ...
    ///
    /// `b` is a child of itself, but shutdown should still be able to complete.
    #[fuchsia::test]
    async fn shutdown_self_referential() {
        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_lazy_child("b").build()),
            ("b", ComponentDeclBuilder::new().add_lazy_child("b").build()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_b2 = test.look_up(vec!["a", "b", "b"].into()).await;

        // Start second `b`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start b2");
        test.model
            .start_instance(&component_b.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start b2");
        test.model
            .start_instance(&component_b2.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start b2");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_b2).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_b2_info = ComponentInfo::new(component_b2).await;

        // Register shutdown action on "a", and wait for it. This should cause all components
        // to shut down, in bottom-up and dependency order.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_b2_info.check_is_shut_down(&test.runner).await;
        {
            let events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            assert_eq!(
                events,
                vec![
                    Lifecycle::Stop(vec!["a:0", "b:0", "b:0"].into()),
                    Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                    Lifecycle::Stop(vec!["a:0"].into())
                ]
            );
        }
    }

    /// Shut down `a`:
    ///  a
    ///   \
    ///    b
    ///   / \
    ///  c   d
    ///
    /// `b` fails to finish shutdown the first time, but succeeds the second time.
    #[fuchsia::test]
    async fn shutdown_error() {
        struct StopErrorHook {
            moniker: InstancedAbsoluteMoniker,
        }

        impl StopErrorHook {
            fn new(moniker: InstancedAbsoluteMoniker) -> Self {
                Self { moniker }
            }

            fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
                vec![HooksRegistration::new(
                    "StopErrorHook",
                    vec![EventType::Stopped],
                    Arc::downgrade(self) as Weak<dyn Hook>,
                )]
            }

            async fn on_shutdown_instance_async(
                &self,
                target_moniker: &InstancedAbsoluteMoniker,
            ) -> Result<(), ModelError> {
                if *target_moniker == self.moniker {
                    return Err(ModelError::unsupported("ouch"));
                }
                Ok(())
            }
        }

        #[async_trait]
        impl Hook for StopErrorHook {
            async fn on(self: Arc<Self>, event: &hooks::Event) -> Result<(), ModelError> {
                let target_moniker = event
                    .target_moniker
                    .unwrap_instance_moniker_or(ModelError::UnexpectedComponentManagerMoniker)?;
                if let Ok(EventPayload::Stopped { .. }) = event.result {
                    self.on_shutdown_instance_async(target_moniker).await?;
                }
                Ok(())
            }
        }

        let components = vec![
            ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
            ("a", ComponentDeclBuilder::new().add_eager_child("b").build()),
            ("b", ComponentDeclBuilder::new().add_eager_child("c").add_eager_child("d").build()),
            ("c", component_decl_with_test_runner()),
            ("d", component_decl_with_test_runner()),
        ];
        let error_hook = Arc::new(StopErrorHook::new(vec!["a:0", "b:0"].into()));
        let test = ActionsTest::new_with_hooks("root", components, None, error_hook.hooks()).await;
        let component_a = test.look_up(vec!["a"].into()).await;
        let component_b = test.look_up(vec!["a", "b"].into()).await;
        let component_c = test.look_up(vec!["a", "b", "c"].into()).await;
        let component_d = test.look_up(vec!["a", "b", "d"].into()).await;

        // Component startup was eager, so they should all have an `Execution`.
        test.model
            .start_instance(&component_a.abs_moniker, &StartReason::Eager)
            .await
            .expect("could not start a");
        assert!(is_executing(&component_a).await);
        assert!(is_executing(&component_b).await);
        assert!(is_executing(&component_c).await);
        assert!(is_executing(&component_d).await);

        let component_a_info = ComponentInfo::new(component_a).await;
        let component_b_info = ComponentInfo::new(component_b).await;
        let component_c_info = ComponentInfo::new(component_c).await;
        let component_d_info = ComponentInfo::new(component_d).await;

        // Register shutdown action on "a", and wait for it. "b"'s component shuts down, but "b"
        // returns an error so "a" does not.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect_err("shutdown succeeded unexpectedly");
        component_a_info.check_not_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        {
            let mut events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            // The leaves could be stopped in any order.
            let mut first: Vec<_> = events.drain(0..2).collect();
            first.sort_unstable();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0", "d:0"].into()),
            ];
            assert_eq!(first, expected);
            assert_eq!(events, vec![Lifecycle::Stop(vec!["a:0", "b:0"].into())],);
        }

        // Register shutdown action on "a" again. "b"'s shutdown succeeds (it's a no-op), and
        // "a" is allowed to shut down this time.
        ActionSet::register(component_a_info.component.clone(), ShutdownAction::new())
            .await
            .expect("shutdown failed");
        component_a_info.check_is_shut_down(&test.runner).await;
        component_b_info.check_is_shut_down(&test.runner).await;
        component_c_info.check_is_shut_down(&test.runner).await;
        component_d_info.check_is_shut_down(&test.runner).await;
        {
            let mut events: Vec<_> = test
                .test_hook
                .lifecycle()
                .into_iter()
                .filter(|e| match e {
                    Lifecycle::Stop(_) => true,
                    _ => false,
                })
                .collect();
            // The leaves could be stopped in any order.
            let mut first: Vec<_> = events.drain(0..2).collect();
            first.sort_unstable();
            let expected: Vec<_> = vec![
                Lifecycle::Stop(vec!["a:0", "b:0", "c:0"].into()),
                Lifecycle::Stop(vec!["a:0", "b:0", "d:0"].into()),
            ];
            assert_eq!(first, expected);
            assert_eq!(
                events,
                vec![
                    Lifecycle::Stop(vec!["a:0", "b:0"].into()),
                    Lifecycle::Stop(vec!["a:0"].into())
                ]
            );
        }
    }
}
