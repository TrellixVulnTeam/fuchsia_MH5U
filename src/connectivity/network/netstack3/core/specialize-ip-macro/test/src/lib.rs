// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Enable the specialize ip macro feature since that is what we are testing.
#![feature(min_specialization)]

/// Module with types that have the same identifiers that `specialize_ip` and
/// `specialize_ip_macro` attributes looks for. Both of these attributes
/// look for the `Ipv4`, `Ipv6`, `Ipv4Addr`, and `Ipv6Addr` identifiers
/// within `net_types::ip` so we create the same hierarchy in this test.
#[cfg(test)]
pub(crate) mod net_types {
    pub(crate) mod ip {
        /// Simple trait with the identifier `Ip` to test that the
        /// `specialize_ip` attribute specializes function for the various types
        /// that implement this trait.
        pub(crate) trait Ip {
            const VERSION: usize;
        }

        /// Simple trait with the identifier `IpAddress` to test that the
        /// `specialize_ip_address` attribute specializes function for the
        /// various types that implement this trait.
        pub(crate) trait IpAddress {
            const VERSION: usize;
        }

        /// Test type with the identifier `Ipv4` that the `specialize_ip`
        /// attribute can specialize a function for.
        pub(crate) struct Ipv4;
        impl Ip for Ipv4 {
            const VERSION: usize = 4;
        }

        /// Test type with the identifier `Ipv6` that the `specialize_ip`
        /// attribute can specialize a function for.
        pub(crate) struct Ipv6;
        impl Ip for Ipv6 {
            const VERSION: usize = 6;
        }

        /// Test type with the identifier `Ipv4Addr` that the
        /// `specialize_ip_address` attribute can specialize a function for.
        pub(crate) struct Ipv4Addr;
        impl IpAddress for Ipv4Addr {
            const VERSION: usize = 4;
        }

        /// Test type with the identifier `Ipv6Addr` that the
        /// `specialize_ip_address` attribute can specialize a function for.
        pub(crate) struct Ipv6Addr;
        impl IpAddress for Ipv6Addr {
            const VERSION: usize = 6;
        }
    }
}

/// Test the specialize_ip_macro macros.
///
/// We have 4 tests for each `specialize_ip` and `specialize_ip_address`:
///  1) Functions with just the specialized type as a type parameter:
///   - test_simple_specialize_for_ip
///   - test_simple_specialize_for_ip_address
///  2) Functions with the specialized type and additional type parameters:
///   - test_additional_type_params_specialize_for_ip
///   - test_additional_type_params_specialize_for_ip_address
///  3) Functions with the specialized type and a lifetime parameter:
///   - test_lifetime_param_specialized_for_ip
///   - test_lifetime_param_specialized_for_ip_address
///  4) Functions with the specialized type and an additional type and lifetime
///     parameters:
///   - test_lifetime_and_additional_type_params_specialized_for_ip
///   - test_lifetime_and_additional_type_params_specialized_for_ip_address
///  5) Functions with the specialized type and multiple type and lifetime
///     parameters:
///   - test_multiple_lifetimes_and_additional_type_params_specialized_for_ip
///   - test_multiple_lifetimes_and_additional_type_params_specialized_for_ip_address
#[cfg(test)]
mod tests {
    // This brings net_types into scope so that the generated code can access it
    // as `net_types` (where it expects it) rather than as `crate::net_types`.
    use crate::net_types;
    use crate::net_types::ip::*;
    use specialize_ip_macro::{ip_addr_test, ip_test, specialize_ip, specialize_ip_address};

    trait TraitA {
        fn ret_vala(&self) -> i32;
    }
    trait TraitB {
        fn ret_valb(&self) -> i32;
    }
    trait GetWrapperItem<A> {
        fn get_first(self) -> A;
        fn get_second(self) -> A;
    }

    #[derive(Copy, Clone)]
    struct TraitAType1(i32);
    impl TraitA for TraitAType1 {
        fn ret_vala(&self) -> i32 {
            self.0
        }
    }

    #[derive(Copy, Clone)]
    struct TraitAType2(i32);
    impl TraitA for TraitAType2 {
        fn ret_vala(&self) -> i32 {
            self.0 * 2
        }
    }

    #[derive(Copy, Clone)]
    struct TraitBType1(i32);
    impl TraitB for TraitBType1 {
        fn ret_valb(&self) -> i32 {
            self.0 * 3
        }
    }

    #[derive(Copy, Clone)]
    struct TraitBType2(i32);
    impl TraitB for TraitBType2 {
        fn ret_valb(&self) -> i32 {
            self.0 * 4
        }
    }

    struct Wrapper<A>(A, A);
    impl<A> GetWrapperItem<A> for Wrapper<A> {
        fn get_first(self) -> A {
            self.0
        }
        fn get_second(self) -> A {
            self.1
        }
    }

    enum EitherLifetime<'a, 'b> {
        A(&'a mut i32),
        B(&'b mut i32),
    }

    impl<'a, 'b> EitherLifetime<'a, 'b> {
        fn add(&mut self, val: i32) {
            match self {
                EitherLifetime::A(x) => **x += val,
                EitherLifetime::B(x) => **x += val,
            }
        }
    }

    #[specialize_ip]
    fn simple_specialized_for_ip<I: Ip>() -> i32 {
        #[ipv4]
        {
            1
        }

        #[ipv6]
        {
            2
        }
    }

    #[specialize_ip_address]
    fn simple_specialized_for_ip_address<A: IpAddress>() -> i32 {
        #[ipv4addr]
        {
            3
        }

        #[ipv6addr]
        {
            4
        }
    }

    #[specialize_ip]
    fn additional_type_params_specialized_for_ip<TA: TraitA, I: Ip, TB: TraitB>(
        ta: TA,
        tb: TB,
    ) -> i32 {
        #[ipv4]
        {
            ta.ret_vala() * tb.ret_valb()
        }

        #[ipv6]
        {
            ta.ret_vala() + tb.ret_valb()
        }
    }

    #[specialize_ip_address]
    fn additional_type_params_specialized_for_ip_address<TA: TraitA, A: IpAddress, TB: TraitB>(
        ta: TA,
        tb: TB,
    ) -> i32 {
        #[ipv4addr]
        {
            ta.ret_vala() - tb.ret_valb()
        }

        #[ipv6addr]
        {
            tb.ret_valb() - ta.ret_vala()
        }
    }

    #[specialize_ip]
    fn lifetime_param_specialized_for_ip<'a, I: Ip>(
        w1: &'a Wrapper<i32>,
        w2: &'a Wrapper<i32>,
    ) -> &'a i32 {
        #[ipv4]
        {
            &w1.0
        }

        #[ipv6]
        {
            &w2.0
        }
    }

    #[specialize_ip_address]
    fn lifetime_param_specialized_for_ip_address<'a, A: IpAddress>(
        w1: &'a Wrapper<i32>,
        w2: &'a Wrapper<i32>,
    ) -> &'a i32 {
        #[ipv4addr]
        {
            &w1.1
        }

        #[ipv6addr]
        {
            &w2.1
        }
    }

    fn lifetime_and_additional_type_params_specialized_for_ip<'a, I: Ip>(
        a: &'a mut i32,
        b: &'a mut i32,
        c: &'a mut i32,
        d: &'a mut i32,
    ) -> &'a mut i32 {
        let w1 = Wrapper(a, b);
        let w2 = Wrapper(c, d);
        lifetime_and_additional_type_params_specialized_for_ip_inner::<'a, Wrapper<&'a mut i32>, I>(
            w1, w2,
        )
    }

    #[specialize_ip]
    fn lifetime_and_additional_type_params_specialized_for_ip_inner<
        'a,
        W: GetWrapperItem<&'a mut i32>,
        I: Ip,
    >(
        w1: W,
        w2: W,
    ) -> &'a mut i32 {
        #[ipv4]
        let ret = w1.get_first();

        #[ipv6]
        let ret = w2.get_first();

        ret
    }

    fn lifetime_and_additional_type_params_specialized_for_ip_address<'a, A: IpAddress>(
        a: &'a mut i32,
        b: &'a mut i32,
        c: &'a mut i32,
        d: &'a mut i32,
    ) -> &'a mut i32 {
        let w1 = Wrapper(a, b);
        let w2 = Wrapper(c, d);
        lifetime_and_additional_type_params_specialized_for_ip_address_inner::<
            'a,
            Wrapper<&'a mut i32>,
            A,
        >(w1, w2)
    }

    #[specialize_ip_address]
    fn lifetime_and_additional_type_params_specialized_for_ip_address_inner<
        'a,
        W: GetWrapperItem<&'a mut i32>,
        A: IpAddress,
    >(
        w1: W,
        w2: W,
    ) -> &'a mut i32 {
        #[ipv4addr]
        let ret = w1.get_second();

        #[ipv6addr]
        let ret = w2.get_second();

        ret
    }

    fn multiple_lifetimes_and_additional_type_params_specialized_for_ip<'a, 'b, I: Ip>(
        a: &'a mut i32,
        b: &'a mut i32,
        c: &'b mut i32,
        d: &'b mut i32,
    ) -> EitherLifetime<'a, 'b> {
        let w1 = Wrapper(a, b);
        let w2 = Wrapper(c, d);
        multiple_lifetimes_and_additional_type_params_specialized_for_ip_inner::<
            'a,
            'b,
            Wrapper<&'a mut i32>,
            Wrapper<&'b mut i32>,
            I,
        >(w1, w2)
    }

    #[specialize_ip]
    fn multiple_lifetimes_and_additional_type_params_specialized_for_ip_inner<
        'a,
        'b,
        W1: GetWrapperItem<&'a mut i32>,
        W2: GetWrapperItem<&'b mut i32>,
        I: Ip,
    >(
        w1: W1,
        w2: W2,
    ) -> EitherLifetime<'a, 'b> {
        #[ipv4]
        let ret = EitherLifetime::A(w1.get_first());

        #[ipv6]
        let ret = EitherLifetime::B(w2.get_first());

        ret
    }

    fn multiple_lifetimes_and_additional_type_params_specialized_for_ip_address<
        'a,
        'b,
        A: IpAddress,
    >(
        a: &'a mut i32,
        b: &'a mut i32,
        c: &'b mut i32,
        d: &'b mut i32,
    ) -> EitherLifetime<'a, 'b> {
        let w1 = Wrapper(a, b);
        let w2 = Wrapper(c, d);
        multiple_lifetimes_and_additional_type_params_specialized_for_ip_address_inner::<
            'a,
            'b,
            Wrapper<&'a mut i32>,
            Wrapper<&'b mut i32>,
            A,
        >(w1, w2)
    }

    #[specialize_ip_address]
    fn multiple_lifetimes_and_additional_type_params_specialized_for_ip_address_inner<
        'a,
        'b,
        W1: GetWrapperItem<&'a mut i32>,
        W2: GetWrapperItem<&'b mut i32>,
        A: IpAddress,
    >(
        w1: W1,
        w2: W2,
    ) -> EitherLifetime<'a, 'b> {
        #[ipv4addr]
        let ret = EitherLifetime::A(w1.get_second());

        #[ipv6addr]
        let ret = EitherLifetime::B(w2.get_second());

        ret
    }

    #[specialize_ip]
    fn match_arms_specialized_for_ip<I: Ip>(a: u32) -> u32 {
        match a {
            1 => 255,
            2 => 127,
            #[ipv4]
            3 => 63,
            #[ipv6]
            4 => 31,
            #[ipv4]
            5 => 15,
            #[ipv6]
            6 => 7,
            _ => 0,
        }
    }

    #[specialize_ip_address]
    fn match_arms_specialized_for_ip_address<A: IpAddress>(a: u32) -> u32 {
        match a {
            1 => 256,
            2 => 128,
            #[ipv4addr]
            3 => 64,
            #[ipv6addr]
            4 => 32,
            #[ipv4addr]
            5 => 16,
            #[ipv6addr]
            6 => 8,
            _ => 0,
        }
    }

    #[test]
    fn test_simple_specialize_for_ip() {
        assert_eq!(simple_specialized_for_ip::<Ipv4>(), 1);
        assert_eq!(simple_specialized_for_ip::<Ipv6>(), 2);
    }

    #[test]
    fn test_simple_specialize_for_ip_address() {
        assert_eq!(simple_specialized_for_ip_address::<Ipv4Addr>(), 3);
        assert_eq!(simple_specialized_for_ip_address::<Ipv6Addr>(), 4);
    }

    #[test]
    fn test_additional_type_params_specialize_for_ip() {
        let a1 = TraitAType1(1);
        let a2 = TraitAType2(2);
        let b1 = TraitBType1(3);
        let b2 = TraitBType2(4);

        assert_eq!(additional_type_params_specialized_for_ip::<_, Ipv4, _>(a1, b1), 9);
        assert_eq!(additional_type_params_specialized_for_ip::<_, Ipv6, _>(a2, b2), 20);
    }

    #[test]
    fn test_additional_type_params_specialize_for_ip_address() {
        let a1 = TraitAType1(1);
        let a2 = TraitAType2(2);
        let b1 = TraitBType1(3);
        let b2 = TraitBType2(4);

        assert_eq!(additional_type_params_specialized_for_ip_address::<_, Ipv4Addr, _>(a1, b1), -8);
        assert_eq!(additional_type_params_specialized_for_ip_address::<_, Ipv6Addr, _>(a2, b2), 12);
    }

    #[test]
    fn test_lifetime_param_specialized_for_ip() {
        let w1 = Wrapper(1, 2);
        let w2 = Wrapper(3, 4);
        assert_eq!(*lifetime_param_specialized_for_ip::<Ipv4>(&w1, &w2), 1);
        assert_eq!(*lifetime_param_specialized_for_ip::<Ipv6>(&w1, &w2), 3);
    }

    #[test]
    fn test_lifetime_param_specialized_for_ip_address() {
        let w1 = Wrapper(1, 2);
        let w2 = Wrapper(3, 4);
        assert_eq!(*lifetime_param_specialized_for_ip_address::<Ipv4Addr>(&w1, &w2), 2);
        assert_eq!(*lifetime_param_specialized_for_ip_address::<Ipv6Addr>(&w1, &w2), 4);
    }

    #[test]
    fn test_lifetime_and_additional_type_params_specialized_for_ip() {
        let mut a = 0;
        let mut b = 10;
        let mut c = 20;
        let mut d = 30;
        *lifetime_and_additional_type_params_specialized_for_ip::<Ipv4>(
            &mut a, &mut b, &mut c, &mut d,
        ) += 3;
        assert_eq!(a, 3);
        assert_eq!(b, 10);
        assert_eq!(c, 20);
        assert_eq!(d, 30);
        *lifetime_and_additional_type_params_specialized_for_ip::<Ipv6>(
            &mut a, &mut b, &mut c, &mut d,
        ) += 4;
        assert_eq!(a, 3);
        assert_eq!(b, 10);
        assert_eq!(c, 24);
        assert_eq!(d, 30);
    }

    #[test]
    fn test_lifetime_and_additional_type_params_specialized_for_ip_address() {
        let mut a = 0;
        let mut b = 10;
        let mut c = 20;
        let mut d = 30;
        *lifetime_and_additional_type_params_specialized_for_ip_address::<Ipv4Addr>(
            &mut a, &mut b, &mut c, &mut d,
        ) += 5;
        assert_eq!(a, 0);
        assert_eq!(b, 15);
        assert_eq!(c, 20);
        assert_eq!(d, 30);
        *lifetime_and_additional_type_params_specialized_for_ip_address::<Ipv6Addr>(
            &mut a, &mut b, &mut c, &mut d,
        ) += 6;
        assert_eq!(a, 0);
        assert_eq!(b, 15);
        assert_eq!(c, 20);
        assert_eq!(d, 36);
    }

    #[test]
    fn test_multiple_lifetimes_and_additional_type_params_specialized_for_ip() {
        let mut a = 0;
        let mut b = 10;
        let mut c = 20;
        let mut d = 30;
        multiple_lifetimes_and_additional_type_params_specialized_for_ip::<Ipv4>(
            &mut a, &mut b, &mut c, &mut d,
        )
        .add(3);
        assert_eq!(a, 3);
        assert_eq!(b, 10);
        assert_eq!(c, 20);
        assert_eq!(d, 30);
        multiple_lifetimes_and_additional_type_params_specialized_for_ip::<Ipv6>(
            &mut a, &mut b, &mut c, &mut d,
        )
        .add(4);
        assert_eq!(a, 3);
        assert_eq!(b, 10);
        assert_eq!(c, 24);
        assert_eq!(d, 30);
    }

    #[test]
    fn test_multiple_lifetimes_and_additional_type_params_specialized_for_ip_address() {
        let mut a = 0;
        let mut b = 10;
        let mut c = 20;
        let mut d = 30;
        multiple_lifetimes_and_additional_type_params_specialized_for_ip_address::<Ipv4Addr>(
            &mut a, &mut b, &mut c, &mut d,
        )
        .add(5);
        assert_eq!(a, 0);
        assert_eq!(b, 15);
        assert_eq!(c, 20);
        assert_eq!(d, 30);
        multiple_lifetimes_and_additional_type_params_specialized_for_ip_address::<Ipv6Addr>(
            &mut a, &mut b, &mut c, &mut d,
        )
        .add(6);
        assert_eq!(a, 0);
        assert_eq!(b, 15);
        assert_eq!(c, 20);
        assert_eq!(d, 36);
    }

    #[test]
    fn test_match_arms_specialized_for_ip() {
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(1), 255);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(2), 127);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(3), 63);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(4), 0);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(5), 15);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(6), 0);
        assert_eq!(match_arms_specialized_for_ip::<Ipv4>(7), 0);

        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(1), 255);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(2), 127);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(3), 0);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(4), 31);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(5), 0);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(6), 7);
        assert_eq!(match_arms_specialized_for_ip::<Ipv6>(7), 0);
    }

    #[test]
    fn test_match_arms_specialized_for_ip_address() {
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(1), 256);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(2), 128);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(3), 64);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(4), 0);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(5), 16);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(6), 0);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv4Addr>(7), 0);

        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(1), 256);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(2), 128);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(3), 0);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(4), 32);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(5), 0);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(6), 8);
        assert_eq!(match_arms_specialized_for_ip_address::<Ipv6Addr>(7), 0);
    }

    #[ip_test]
    fn test_ip_test<I: Ip>() {
        let x = simple_specialized_for_ip::<I>();
        if I::VERSION == 4 {
            assert!(x == 1);
        } else {
            assert!(x == 2);
        }
    }

    // test that we can stack both macros:
    #[ip_test]
    #[specialize_ip]
    fn test_specialize_ip_test<I: Ip>() {
        let x = simple_specialized_for_ip::<I>();
        #[ipv4]
        assert!(x == 1);
        #[ipv6]
        assert!(x == 2);
    }

    // test that we can stack both macros (in reverse order):
    #[specialize_ip]
    #[ip_test]
    fn test_specialize_ip_test_different_order<I: Ip>() {
        let x = simple_specialized_for_ip::<I>();
        #[ipv4]
        assert!(x == 1);
        #[ipv6]
        assert!(x == 2);
    }

    #[ip_addr_test]
    fn test_ip_addr_test<A: IpAddress>() {
        let x = simple_specialized_for_ip_address::<A>();
        if A::VERSION == 4 {
            assert!(x == 3);
        } else {
            assert!(x == 4);
        }
    }

    #[ip_addr_test]
    #[specialize_ip_address]
    fn test_specialize_ip_addr_test<A: IpAddress>() {
        let x = simple_specialized_for_ip_address::<A>();
        #[ipv4addr]
        assert!(x == 3);
        #[ipv6addr]
        assert!(x == 4);
    }

    // test that all the `ip_[addr]_test` functions were generated
    #[test]
    fn test_all_tests_generation() {
        test_ip_test_v4();
        test_ip_test_v6();
        test_specialize_ip_test_v4();
        test_specialize_ip_test_v6();
        test_specialize_ip_test_different_order_v4();
        test_specialize_ip_test_different_order_v6();
        test_ip_addr_test_v4();
        test_ip_addr_test_v6();
        test_specialize_ip_addr_test_v4();
        test_specialize_ip_addr_test_v6();
    }

    #[ip_test]
    #[should_panic]
    fn test_should_panic<I: Ip>() {
        assert_eq!(0, simple_specialized_for_ip::<I>());
    }
}
