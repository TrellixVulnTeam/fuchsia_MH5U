# The `#[specialize_ip]` and `#[specialize_ip_addr]` proc macros

This crate defines the `#[specialize_ip]` and `#[specialize_ip_addr]` proc
macros. These allow the creation of functions which are generic over the `Ip` or
`IpAddr` traits, but provide specialized behavior depending on which concrete
type is given. The netstack requires a lot of protocol-specific logic, and these
proc macros make that logic easy.

We will use `#[specialize_ip]` in this explanation, but the same explanation
applies just as well to `#[specialize_ip_addr]`.

The `#[specialize_ip]` attribute can be placed on any function where exactly one
of its type parameters has an `Ip` bound. For example:

```rust
#[specialize_ip]
fn foo<D: EventDispatcher, I: Ip>() { ... }
```

Multiple types with an `Ip` bound are not allowed, and the type with the `Ip`
bound may not also have other bounds. Where clauses are not currently supported.

The result is a function whose signature is identical from the perspective of
those outside the function, but which has different bodies depending on whether
`I` is `Ipv4` or `Ipv6`.

The inside of a specialized function is written as follows:

```rust
#[specialize_ip]
fn foo<D: EventDispatcher, I: Ip>() {
    do_thing_a();

    #[ipv4]
    do_ipv4_thing();

    do_thing_b();

    #[ipv4]
    do_other_ipv4_thing();

    {
        do_thing_c();
        #[ipv6]
        do_ipv6_thing();
    }

    #[ipv6]
    do_other_ipv6_thing();

    match do_thing_d() {
        #[ipv4]
        4 => do_ipv4_thing_e(),

        #[ipv6]
        6 => do_ipv6_thing_e(),

        _ => do_other_thing_e(),
    };
}
```


The `#[ipv4]` (`#[ipv4addr]`) and `#[ipv6]` (`#[ipv6addr]`) attributes are used
to indicate a statement or match arm which should only be included in a
particular version of the function. If multiple statements are required, a block
can be used:

```rust
#[ipv4]
{
    do_first_ipv4_thing();
    do_second_ipv4_thing();
}
```

Any statement or match arm not annotated with `#[ipv4]` or `#[ipv6]` will be
present in both versions of the function, while statements or match arms
annotated with `#[ipv4]` will be removed in the `Ipv6` version of the function,
and vice versa. The above example would compile into:

```rust
/// `Ipv4` version
fn foo<D: EventDispatcher>() {
    do_thing_a();

    do_ipv4_thing();

    do_thing_b();

    do_other_ipv4_thing();

    {
        do_thing_c();
    }

    match do_thing_d() {
        4 => do_ipv4_thing_e(),
        _ => do_other_thing_e(),
    };
}

/// `Ipv6` version
fn foo<D: EventDispatcher>() {
    do_thing_a();

    do_thing_b();

    {
        do_thing_c();
        do_ipv6_thing();
    }

    do_other_ipv6_thing();

    match do_thing_d() {
        6 => do_ipv6_thing_e(),
        _ => do_other_thing_e(),
    };
}
```

## Limitations

### Statements vs Expressions

Due to the way the Rust parser works, only statements and match arms may be
annotated with `#[ipv4]` or `#[ipv6]`; they cannot be used to annotate
expressions. In other words, the following will fail to parse before the proc
macro is ever run:

```rust
#[specialize_ip]
fn address_bits<I: Ip>() -> usize {
    #[ipv4]
    32
    #[ipv6]
    128
}
```

As a workaround, an explicit `return` can be used:

```rust
#[specialize_ip]
fn address_bits<I: Ip>() -> usize {
    #[ipv4]
    return 32;
    #[ipv6]
    return 128;
}
```

Expressions which are not annotated is fine, so expressions inside annotated
blocks will work fine:

```rust
#[specialize_ip]
fn address_bits<I: Ip>() -> usize {
    #[ipv4]
    {
        32
    }

    #[ipv6]
    {
        128
    }
}
```

### Impl Trait

Under the hood, the macros are implemented by generating and implementing
traits. Rust currently doesn't support the impl trait feature for trait
functions and methods, so they are not supported by our macros either.

## Implementation

Under the hood, these proc macros use impl specialization. An extension trait is
defined on the `Ip` or `IpAddr` traits with a function of the appropriate
signature. The extension trait is implemented for each of the two concrete
types, with the method's body containing the generated code. All instances of
the type with the `Ip` or `IpAddr` bound are replaced with `Self`. For example,
this:

```rust
fn foo<D: EventDispatcher, I: Ip>(addr: I::Addr) -> I::Addr {
    do_thing_a();

    #[ipv4]
    let ret = do_ipv4_thing(addr);

    #[ipv6]
    let ret = do_ipv6_thing(addr);

    do_thing_b(&ret);
    ret
}
```

Produces this:

```rust
fn foo<D: EventDispatcher, I: Ip>(addr: I::Addr) -> I::Addr {
    trait Ext: Ip {
        fn f<D: EventDispatcher>(addr: Self::Addr) -> Self::Addr;
    }
    impl<I: Ip> Ext for I {
        default fn f<D: EventDispatcher>(addr: Self::Addr) -> Self::Addr { unimplemented!() }
    }
    impl Ext for Ipv4 {
        fn f<D: EventDispatcher>(addr: Self::Addr) -> Self::Addr {
            do_thing_a();
            let ret = do_ipv4_thing(addr);
            do_thing_b(&ret);
            ret
        }
    }
    impl Ext for Ipv6 {
        fn f<D: EventDispatcher>(addr: Self::Addr) -> Self::Addr {
            do_thing_a();
            let ret = do_ipv6_thing(addr);
            do_thing_b(&ret);
            ret
        }
    }

    I::f::<D>(addr)
}
```

# The `#[ip_test]` and `#[ip_addr_test]` macros

The `#[ip_test]` and `#[ip_addr_test]` macros provide a shorthand to define
tests that are parameterized and need to run on both IP versions.

We will use `#[ip_test]` in this explanation, but the same explanation
applies just as well to `#[ip_addr_test]`.

You can define a test that is parameterized over an IP version as follows:

```rust
#[ip_test]
fn test_foo<I: Ip>() {
   assert!(do_ip_specific_thing::<I>());
   /* ... */
}
```

A function marked with `#[ip_test]` or `#[ip_addr_test]` must *always*:
* Receive zero arguments
* Have *exactly one* type parameter that
   * Has an `Ip` trait bound for `#[ip_test]`
   * Has an `IpAddress` trait bound for `#[ip_addr_test]`

The `#[ip_test]` and `#[ip_addr_test]` macros generate code from that example
that looks like:

```rust
fn test_foo<I: Ip>() {
   assert!(do_ip_specific_thing::<I>());
   /* ... */
}

#[test]
fn test_foo_v4() {
   test_foo::<Ipv4>();
}

#[test]
fn test_foo_v6() {
   test_foo::<Ipv6>();
}
```

You can also can mix the `#[ip_test]` macro with the `#[specialize_ip]` macro:

```rust
#[specialize_ip]
#[ip_test]
fn test_foo<I: Ip>() {
   #[ipv4]
   assert!(do_ipv4_thing());
   #[ipv6]
   assert!(do_ipv6_thing());
   /* ... */
}
```

Finally, for test attributes, you can just add them as you would for a regular
test:

```rust
#[ip_test]
#[should_panic]
fn test_foo_panics<I: Ip>() {
    /* ... */
   do_ip_thing_that_panics::<I>();
}
```