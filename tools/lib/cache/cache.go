// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Package cache provides methods for creating and using a cache.
package cache

import "container/list"

// A Key may be any value that is comparable.
type Key interface{}

// Cache is the interface for cache.
type Cache interface {
	// Adds a value to the cache.
	Add(key Key, value interface{}) interface{}

	// Returns key's value from the cache.
	Get(key Key) (interface{}, bool)

	// Checks if a key exists in cache.
	Contains(key Key) bool

	// Removes a key from the cache.
	Remove(key Key) interface{}

	// Returns the number of items in the cache.
	Len() int

	// Clears all cache entries.
	Clear()
}

// LRUCache is a simple LRU cache.
type LRUCache struct {
	// Size is the maximum number of entries before an item is evicted.
	// Zero means no limit on the number of entries.
	Size uint

	ll    *list.List
	cache map[interface{}]*list.Element
}

type entry struct {
	key   Key
	value interface{}
}

// Add adds a value to the cache and updates the "recently used"-ness of the key.
func (c *LRUCache) Add(key Key, value interface{}) interface{} {
	if c.cache == nil {
		c.cache = make(map[interface{}]*list.Element)
		c.ll = list.New()
	}
	if e, ok := c.cache[key]; ok {
		c.ll.MoveToFront(e)
		value, e.Value.(*entry).value = e.Value.(*entry).value, value
		return value
	}
	e := c.ll.PushFront(&entry{key, value})
	c.cache[key] = e
	if c.Size != 0 && uint(c.ll.Len()) > c.Size {
		v := c.ll.Remove(c.ll.Back())
		delete(c.cache, v.(*entry).key)
	}
	return nil
}

// Get returns key's value from the cache and updates the "recently used"-ness.
func (c *LRUCache) Get(key Key) (interface{}, bool) {
	if c.cache == nil {
		return nil, false
	}
	if e, ok := c.cache[key]; ok {
		c.ll.MoveToFront(e)
		return e.Value.(*entry).value, true
	}
	return nil, false
}

// Contains checks if a key exists in cache without updating the recent-ness.
func (c *LRUCache) Contains(key Key) bool {
	if c.cache == nil {
		return false
	}
	_, ok := c.cache[key]
	return ok
}

// Remove removes a key from the cache.
func (c *LRUCache) Remove(key Key) interface{} {
	if c.cache == nil {
		return nil
	}
	if e, ok := c.cache[key]; ok {
		c.ll.Remove(e)
		delete(c.cache, key)
		return e.Value.(*entry).value
	}
	return nil
}

// Len returns the number of items in the cache.
func (c *LRUCache) Len() int {
	if c.cache == nil {
		return 0
	}
	return c.ll.Len()
}

// Clear clears all cache entries.
func (c *LRUCache) Clear() {
	c.ll = nil
	c.cache = nil
}
