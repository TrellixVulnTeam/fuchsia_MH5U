// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A trie data structure used for matching inspect properties.

use {
    core::marker::PhantomData,
    std::collections::HashMap,
    std::{cmp::Eq, fmt::Debug, hash::Hash},
};

//TODO(fxbug.dev/38342): Move this mod to its own lib to avoid leaking its existance in the
//             fuchsia_inspect documentation.

/// Trie mapping a sequence of key fragments to nodes that
/// are able to store a vector of multiple values which
/// are all identifiable by the same key sequence.
pub struct Trie<K, V>
where
    K: Hash + Eq,
{
    root: TrieNode<K, V>,
}

impl<K, V> Trie<K, V>
where
    K: Hash + Eq,
{
    /// Creates a new empty Trie
    pub fn new() -> Self
    where
        K: Hash + Eq,
    {
        Trie { root: TrieNode::new() }
    }

    /// Creates an iterator of the Trie which
    /// will return an Item which is a tuple of the vector
    /// of key fragments that define the sequence up to
    /// a node containing values, and a single value in that node.
    ///
    /// The iterator will visit all values in a given node, one at a time,
    /// before moving on to a new node.
    ///
    /// The iterator performs a stack based DFS through the trie to
    /// allow reconstruction of the key sequence defining each node.
    pub fn iter(&self) -> TrieIterableType<'_, K, V, TrieIterator<'_, K, V>> {
        TrieIterableType {
            iterator: TrieIterator {
                trie: self,
                iterator_initialized: false,
                work_stack: vec![],
                curr_key: vec![],
                curr_node: None,
                curr_val_index: 0,
            },
            _marker: PhantomData,
        }
    }

    fn remove_helper(curr_node: &mut TrieNode<K, V>, key: &Vec<K>, level: usize) -> bool
    where
        K: Hash + Eq + Debug,
    {
        if level == key.len() {
            curr_node.values = Vec::new();
            return curr_node.children.is_empty();
        }
        let mut delete_self: bool = false;
        if let Some(mut child_node) = curr_node.children.get_mut(&key[level]) {
            // TODO(lukenicholson): Consider implementing as stack and provide
            // limits to stack size?
            if Trie::remove_helper(&mut child_node, key, level + 1) {
                curr_node.children.remove(&key[level]);
            }

            if curr_node.children.is_empty() && curr_node.values.is_empty() {
                delete_self = true;
            }
        }
        delete_self
    }

    /// Takes a key fragment sequence in vector form and removes the node identified
    /// by that key fragment sequence if it exists. Removes all values that exist on the
    /// node.
    pub fn remove(&mut self, key: &Vec<K>)
    where
        K: Hash + Eq + Debug,
    {
        if key.len() < 1 {
            return;
        }

        Trie::remove_helper(&mut self.root, &key, 0);
    }

    /// Retrieves a node identified by the key fragment vector `key` if it
    /// exists in the prefix trie, else None.
    pub fn get(&self, key: &Vec<K>) -> Option<&TrieNode<K, V>>
    where
        K: Hash + Eq + Debug,
    {
        key.into_iter()
            .try_fold(&self.root, |curr_node, key_fragment| curr_node.children.get(&key_fragment))
    }

    /// Retrieves a mutable node identified by the key fragment vector `key` if it
    /// exists in the prefix trie, else None.
    pub fn get_mut(&mut self, key: &Vec<K>) -> Option<&mut TrieNode<K, V>>
    where
        K: Hash + Eq + Debug,
    {
        key.into_iter().try_fold(&mut self.root, |curr_node, key_fragment| {
            curr_node.children.get_mut(&key_fragment)
        })
    }

    /// Takes a key fragment sequence in vector form, and a value defined by the
    /// key sequence, and populates the trie creating new nodes where needed, before
    /// inserting the value into the vector of values defined by the provided sequence.
    pub fn insert(&mut self, key: Vec<K>, value: V)
    where
        K: Hash + Eq + Debug,
    {
        let key_trie_node = key.into_iter().fold(&mut self.root, |curr_node, key_fragment| {
            curr_node.children.entry(key_fragment).or_insert_with(|| TrieNode::new())
        });

        key_trie_node.values.push(value);
    }

    pub fn get_root(&self) -> &TrieNode<K, V> {
        return &self.root;
    }
}

/// A node of a `Trie`.
pub struct TrieNode<K, V>
where
    K: Hash + Eq,
{
    values: Vec<V>,
    children: HashMap<K, TrieNode<K, V>>,
}

impl<K, V> TrieNode<K, V>
where
    K: Hash + Eq,
{
    /// Creates a new empty `Node`.
    pub fn new() -> Self
    where
        K: Hash + Eq,
    {
        TrieNode { values: Vec::new(), children: HashMap::new() }
    }

    /// Returns a mutable reference to the values stored in this node.
    pub fn get_values_mut(&mut self) -> &mut Vec<V> {
        return &mut self.values;
    }
}

/// A trie iteration work event.
pub struct TrieIterableWorkEvent<'a, K, N> {
    pub key_state: TrieIterableKeyState<'a, K>,
    pub potential_child: Option<&'a N>,
}

/// Trait defining the properties a graph
/// node must have in order to be used by
/// the generic Trie Iterator.
pub trait TrieIterableNode<K, V>
where
    K: Hash + Eq,
{
    /// Returns a map from a graph's node
    /// to the children of that node, indexed
    /// by their Trie-equivalent prefix.
    fn get_children(&self) -> HashMap<&K, &Self>;

    /// Returns the vector of values that a graph's
    /// node is storing.
    fn get_values(&self) -> &[V];
}

/// Trait defining the methods a graph's iterator must
/// implement in order to be used by the TrieIterator.
pub trait TrieIterable<'a, K, V>
where
    K: Hash + Eq,
{
    type Node: 'a + TrieIterableNode<K, V>;

    /// Returns whether the iterator has been initialized.
    fn is_initialized(&self) -> bool;

    /// Initializes the iterator.
    fn initialize(&mut self);

    /// Pushes a Trie iteration work event onto
    /// the iterators work stack. This method is only needed because traits
    /// cannot currently have their own fields.
    ///
    /// To implement, take the work_event argument and push it onto your stack.
    fn add_work_event(&mut self, work_event: TrieIterableWorkEvent<'a, K, Self::Node>);

    /// Pops a Trie iteration work event off of
    /// the iterators work stack. This method is only needed because traits
    /// cannot currently have their own fields.
    ///
    /// To implement, pop the most recent work event off of your workstack and return it.
    fn expect_work_event(&mut self) -> TrieIterableWorkEvent<'a, K, Self::Node>;

    /// Returns the iterator's current-node being processed for iteration.
    fn expect_curr_node(&self) -> &'a Self::Node;

    /// Sets the iterator's current node being processed. Also responsible
    /// for resetting any node-specific state, like an index into the values.
    fn set_curr_node(&mut self, new_node: &'a Self::Node);

    /// Returns whether the node either has no values or has processed all
    /// of the values.
    fn is_curr_node_fully_processed(&self) -> bool;

    /// Returns whether there are any more work events to process.
    fn is_work_stack_empty(&self) -> bool;

    /// Removes the most recently added key_fragment from the
    /// key fragment aggregator.
    fn pop_curr_key_fragment(&mut self);

    /// Pushes a new key fragment onto the key fragment aggregator.
    fn extend_curr_key(&mut self, new_fragment: &'a K);

    /// Returns a clone of the current key defining the node the
    /// iterator is currently at.
    fn get_curr_key(&mut self) -> Vec<&'a K>;

    /// Processes the current node and returns the next value
    /// at that node. Also responsible for updating any node-specific
    /// state, like an index into the values.
    fn get_next_value(&mut self) -> &'a V;
}

impl<K, V> TrieIterableNode<K, V> for TrieNode<K, V>
where
    K: Hash + Eq,
{
    fn get_children(&self) -> HashMap<&K, &Self> {
        self.children.iter().collect::<HashMap<&K, &TrieNode<K, V>>>()
    }

    fn get_values(&self) -> &[V] {
        return &self.values;
    }
}

/// An iterator for a `Trie`.
pub struct TrieIterator<'a, K, V>
where
    K: Hash + Eq,
{
    trie: &'a Trie<K, V>,
    iterator_initialized: bool,
    work_stack: Vec<TrieIterableWorkEvent<'a, K, TrieNode<K, V>>>,
    curr_key: Vec<&'a K>,
    curr_node: Option<&'a TrieNode<K, V>>,
    curr_val_index: usize,
}

impl<'a, K, V> TrieIterable<'a, K, V> for TrieIterator<'a, K, V>
where
    K: Hash + Eq,
{
    type Node = TrieNode<K, V>;
    fn is_initialized(&self) -> bool {
        self.iterator_initialized
    }

    fn initialize(&mut self) {
        self.iterator_initialized = true;
        self.add_work_event(TrieIterableWorkEvent {
            key_state: TrieIterableKeyState::PopKeyFragment,
            potential_child: None,
        });

        self.curr_node = Some(self.trie.get_root());
        for (key_fragment, child_node) in self.curr_node.unwrap().get_children().iter() {
            self.add_work_event(TrieIterableWorkEvent {
                key_state: TrieIterableKeyState::AddKeyFragment(key_fragment),
                potential_child: Some(child_node),
            });
        }
    }

    fn add_work_event(&mut self, work_event: TrieIterableWorkEvent<'a, K, Self::Node>) {
        self.work_stack.push(work_event);
    }

    fn expect_work_event(&mut self) -> TrieIterableWorkEvent<'a, K, Self::Node> {
        self.work_stack
            .pop()
            .expect("Should never be attempting to retrieve work event from empty stack.")
    }
    fn expect_curr_node(&self) -> &'a Self::Node {
        self.curr_node.expect("Should never be fetching working node when working node is none.")
    }
    fn set_curr_node(&mut self, new_node: &'a Self::Node) {
        self.curr_val_index = 0;
        self.curr_node = Some(new_node);
    }
    fn is_curr_node_fully_processed(&self) -> bool {
        debug_assert!(
            self.curr_node.is_some(),
            "We should never be fetching an uninitialized curr_node."
        );
        let curr_node = self
            .curr_node
            .expect("Should never ask if curr_node is processed, when curr_node is none.");
        curr_node.get_values().is_empty() || curr_node.get_values().len() <= self.curr_val_index
    }
    fn is_work_stack_empty(&self) -> bool {
        self.work_stack.is_empty()
    }
    fn pop_curr_key_fragment(&mut self) {
        self.curr_key.pop();
    }
    fn extend_curr_key(&mut self, new_fragment: &'a K) {
        self.curr_key.push(new_fragment);
    }

    fn get_curr_key(&mut self) -> Vec<&'a K> {
        self.curr_key.clone()
    }

    fn get_next_value(&mut self) -> &'a V {
        self.curr_val_index = self.curr_val_index + 1;
        &self
            .curr_node
            .expect("Should never be retrieving a value when a working node is unset.")
            .get_values()[self.curr_val_index - 1]
    }
}

/// The key state of a `TrieIterableWorkEvent`
pub enum TrieIterableKeyState<'a, K> {
    AddKeyFragment(&'a K),
    PopKeyFragment,
}

/// A type representing wrapping an trie iterator.
pub struct TrieIterableType<'a, K, V, T: TrieIterable<'a, K, V>>
where
    K: Hash + Eq,
{
    pub iterator: T,
    pub _marker: PhantomData<fn() -> (&'a K, &'a V)>,
}

impl<'a, K, V, T: TrieIterable<'a, K, V>> Iterator for TrieIterableType<'a, K, V, T>
where
    K: Hash + Eq,
{
    type Item = (Vec<&'a K>, Option<&'a V>);

    fn next(&mut self) -> Option<(Vec<&'a K>, Option<&'a V>)> {
        if !self.iterator.is_initialized() {
            self.iterator.initialize();

            // We must special-case "iteration" for an empty root
            // here since we rely on is_curr_node_fully_processed
            // being true to signal empty nodes when a new node is just processed,
            // and we've already pre-processed root.
            if self.iterator.is_curr_node_fully_processed() {
                return Some((self.iterator.get_curr_key(), None));
            }
        }

        while self.iterator.is_curr_node_fully_processed() && !self.iterator.is_work_stack_empty() {
            let next_work_event = self.iterator.expect_work_event();
            match next_work_event.key_state {
                TrieIterableKeyState::PopKeyFragment => {
                    self.iterator.pop_curr_key_fragment();
                }
                TrieIterableKeyState::AddKeyFragment(key_fragment) => {
                    match next_work_event.potential_child {
                        Some(child) => {
                            self.iterator.extend_curr_key(key_fragment);
                            self.iterator.set_curr_node(child);
                        }
                        None => unreachable!(
                        "Work events that extend key fragments must have an associated child node."
                    ),
                    }

                    self.iterator.add_work_event(TrieIterableWorkEvent {
                        key_state: TrieIterableKeyState::PopKeyFragment,
                        potential_child: None,
                    });

                    for (key_fragment, child_node) in
                        self.iterator.expect_curr_node().get_children().iter()
                    {
                        self.iterator.add_work_event(TrieIterableWorkEvent {
                            key_state: TrieIterableKeyState::AddKeyFragment(key_fragment),
                            potential_child: Some(child_node),
                        });
                    }

                    // If a node is fully processed as soon as being added, then
                    // it is empty. We need to signal empty nodes in the iterator.
                    if self.iterator.is_curr_node_fully_processed() {
                        return Some((self.iterator.get_curr_key(), None));
                    }
                }
            }
        }

        if self.iterator.is_curr_node_fully_processed() {
            None
        } else {
            Some((self.iterator.get_curr_key(), Some(self.iterator.get_next_value())))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    fn test_sequence_trie() {
        type TestTrie = Trie<char, String>;
        let mut test_trie: TestTrie = TestTrie::new();
        test_trie.insert("test".to_string().chars().collect(), "a".to_string());
        test_trie.insert("test1".to_string().chars().collect(), "b".to_string());

        let mut curr_node = test_trie.get_root();
        assert!(curr_node.get_values().is_empty());
        curr_node = curr_node.get_children().get(&'t').unwrap();
        assert!(curr_node.get_values().is_empty());
        curr_node = curr_node.get_children().get(&'e').unwrap();
        assert!(curr_node.get_values().is_empty());
        curr_node = curr_node.get_children().get(&'s').unwrap();
        assert!(curr_node.get_values().is_empty());
        curr_node = curr_node.get_children().get(&'t').unwrap();
        assert_eq!(curr_node.get_values().len(), 1);
        curr_node = curr_node.get_children().get(&'1').unwrap();
        assert_eq!(curr_node.get_values().len(), 1);

        let key = "test".to_string().chars().collect();
        let test_node = test_trie.get(&key);
        assert!(test_node.is_some());
        assert_eq!(test_node.unwrap().get_values().len(), 1);
        assert_eq!(test_node.unwrap().get_values()[0 as usize], "a".to_string());

        let key = "test1".to_string().chars().collect();
        let test1_node = test_trie.get(&key);
        assert!(test1_node.is_some());
        assert_eq!(test1_node.unwrap().get_values().len(), 1);
        assert_eq!(test1_node.unwrap().get_values()[0 as usize], "b".to_string());
    }

    #[fuchsia::test]
    fn test_sequence_trie_removal() {
        type TestTrie = Trie<char, String>;
        let mut test_trie: TestTrie = TestTrie::new();
        test_trie.insert("test".to_string().chars().collect(), "a".to_string());
        test_trie.insert("test1".to_string().chars().collect(), "b".to_string());
        test_trie.insert("text".to_string().chars().collect(), "c".to_string());
        test_trie.insert("text12".to_string().chars().collect(), "d".to_string());

        test_trie.remove(&("test".to_string().chars().collect()));
        test_trie.remove(&vec![]);
        test_trie.remove(&("text12".to_string().chars().collect()));

        let key = "test".to_string().chars().collect();
        let test_node = test_trie.get(&key);
        assert!(test_node.is_some());
        assert!(test_node.unwrap().get_values().is_empty());

        let key = "test1".to_string().chars().collect();
        let test1_node = test_trie.get(&key);
        assert!(test1_node.is_some());
        assert_eq!(test1_node.unwrap().get_values().len(), 1);
        assert_eq!(test1_node.unwrap().get_values()[0], "b".to_string());

        let key = "text".to_string().chars().collect();
        let text_node = test_trie.get(&key);
        assert!(text_node.is_some());
        assert_eq!(text_node.unwrap().get_values().len(), 1);
        assert_eq!(text_node.unwrap().get_values()[0], "c".to_string());

        let key = "text1".to_string().chars().collect();
        let text1_node = test_trie.get(&key);
        assert!(text1_node.is_none());
    }

    #[fuchsia::test]
    fn test_sequence_trie_iter() {
        type TestTrie = Trie<char, String>;
        let mut test_trie: TestTrie = TestTrie::new();
        test_trie.insert("test".to_string().chars().collect(), "a".to_string());
        test_trie.insert("test".to_string().chars().collect(), "a2".to_string());
        test_trie.insert("test1".to_string().chars().collect(), "b".to_string());
        test_trie.insert("test1".to_string().chars().collect(), "b2".to_string());
        let mut results_vec = vec![
            (vec!['t', 'e', 's', 't', '1'], Some("b2".to_string())),
            (vec!['t', 'e', 's', 't', '1'], Some("b".to_string())),
            (vec!['t', 'e', 's', 't'], Some("a2".to_string())),
            (vec!['t', 'e', 's', 't'], Some("a".to_string())),
            (vec!['t', 'e', 's'], None),
            (vec!['t', 'e'], None),
            (vec!['t'], None),
            (vec![], None),
        ];
        let mut num_iterations = 0;
        for (key, val) in test_trie.iter() {
            num_iterations = num_iterations + 1;
            let (expected_key, expected_val) = results_vec.pop().unwrap();
            assert_eq!(val, expected_val.as_ref());
            assert_eq!(key.into_iter().collect::<String>(), expected_key.iter().collect::<String>())
        }

        assert_eq!(num_iterations, 8);
    }

    #[fuchsia::test]
    fn test_empty_trie_iters() {
        type TestTrie = Trie<char, String>;
        let empty_trie: TestTrie = TestTrie::new();
        let mut num_iterations = 0;
        let mut results_vec: Vec<(Vec<char>, Option<String>)> = vec![(vec![], None)];
        for (key, val) in empty_trie.iter() {
            num_iterations = num_iterations + 1;
            let (expected_key, expected_val) = results_vec.pop().unwrap();
            assert_eq!(val, expected_val.as_ref());
            assert_eq!(key.into_iter().collect::<String>(), expected_key.iter().collect::<String>())
        }
        assert_eq!(num_iterations, 1);

        let mut one_entry_trie: TestTrie = TestTrie::new();
        one_entry_trie.insert(vec!['t'], "a".to_string());
        let mut results_vec = vec![(vec!['t'], Some("a".to_string())), (vec![], None)];
        num_iterations = 0;
        for (key, val) in one_entry_trie.iter() {
            num_iterations = num_iterations + 1;
            let (expected_key, expected_val) = results_vec.pop().unwrap();
            assert_eq!(val, expected_val.as_ref());
            assert_eq!(key.into_iter().collect::<String>(), expected_key.iter().collect::<String>())
        }

        assert_eq!(num_iterations, 2);
    }
}
