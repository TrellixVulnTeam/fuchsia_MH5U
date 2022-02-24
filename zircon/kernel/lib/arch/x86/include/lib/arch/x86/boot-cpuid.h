// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_ARCH_X86_INCLUDE_LIB_ARCH_X86_BOOT_CPUID_H_
#define ZIRCON_KERNEL_LIB_ARCH_X86_INCLUDE_LIB_ARCH_X86_BOOT_CPUID_H_

#include <lib/arch/x86/cpuid.h>
#include <lib/special-sections/special-sections.h>

// Easy access to CPUID results collected for the boot CPU at boot time.
//
// NOTE: This header is available in contexts other than x86 Fuchsia code,
// but the functions declared here are only defined for x86 Fuchsia code.
// For example, parameterized templates might use BootCpuidIo as a default
// template argument but also be usable in non-Fuchsia unit test code when
// a different template argument is supplied.
//
// See <lib/arch/x86/cpuid.h> for the definitions of various types representing
// particular CPUID data.  For any type T among those types, using BootCpuid<T>
// in a program automatically causes the InitializeBootCpuid() function called
// from assembly at early startup to pre-populate the data that BootCpuid<T>
// will return.  These are "free" inline calls that just extract the bits using
// `hwreg`-style accessors from pre-populated hidden global variables.  e.g.
// ```
// bool have_avx = arch::BootCpuid<arch::CpuidFeatureFlagsC>().avx();
// ```
// This will boil down into `... = (_hidden_global_[2] & constant_mask) != 0;`.
//
// The InitializeBootCpuid() call can be made early on from assembly code to
// populate the data.  That function requires only the basic machine stack for
// its call and return, and doesn't need the full C++ ABI to be available yet.

// TODO(fxbug.dev/64109): <cpuid.h> lacks a proper multiple inclusion guard!
#ifndef __cpuid
#include <cpuid.h>
#endif

#include <type_traits>

namespace arch {

namespace internal {

// These are referenced from assembly code and so they need unmangled names
// that are tolerable to use from assembly.
extern "C" CpuidIo gBootCpuid0, gBootCpuidHyp0, gBootCpuidExt0, gBootCpuidFeature, gBootCpuidExtf;

}  // namespace internal

// A "CPUID I/O provider", BootCpuidIo's methods are expected to be
// instantiated by "CPUID value types", defined in <lib/arch/x86/cpuid.h>.
//
// `BootCpuidIo<T>{}.Get()` returns a `const arch::CpuidIo*` that can be used
// with the `hwreg` objects from `T::Get()`. InitializeBootCpuid() fills in
// the data for all the instantiations linked in.
//
// This template can be used as a parameter for template functions, e.g.
// `arch::GetVendor(BootCpuidIo{})`.
class BootCpuidIo {
 public:
  // Most often just Get<Type> is used instead to reach a particular (sub)leaf.
  // Multiple different CpuidValue types reach the same (sub)leaf, usually one
  // type for each of the four registers.
  template <typename CpuidValue>
  const CpuidIo* Get() const {
    return GetLeaf<CpuidValue::kLeaf, CpuidValue::kSubleaf>();
  }

  // Convenience accessor for the common case.
  template <typename CpuidValue>
  auto Read() const {
    return CpuidValue::Get().ReadFrom(Get<CpuidValue>());
  }

 private:
  // The underlying instantiation is indexed by leaf and subleaf.
  template <uint32_t Leaf, uint32_t Subleaf = 0>
  const CpuidIo* GetLeaf() const {
#ifdef __clang__
    // The CpuidIo object for each instantiation goes into a special section
    // that the InitializeBootCpuid() function processes when called at
    // startup.  Each entry starts at compile time with the leaf and subleaf
    // in the slots for the registers where the cpuid instruction takes them
    // as operands, and gets filled by InitializeBootCpuid() with the register
    // results, or cleared to all zero if the Leaf is not supported by the
    // hardware.  The assembly code implementing InitializeBootCpuid() knows
    // the exact layout of this data, so make sure the C++ types conform.
    static_assert(alignof(CpuidIo) == alignof(uint32_t));
    static_assert(sizeof(CpuidIo) == sizeof(uint32_t[4]));
    static_assert(std::is_same_v<decltype(CpuidIo{}.values_), uint32_t[4]>);

    // "used" for kCpuidLeaf to prevent compiler-GC; "used" for kCpuidIo to be
    // doubly sure that every BootCpuidLeaf entry has an associated one in
    // BootCpuidData.
    SPECIAL_SECTION("BootCpuidData", uint32_t) static CpuidIo gCpuidIo;
    SPECIAL_SECTION("BootCpuidLeaf", uint32_t)
    static const uint32_t kCpuidLeaf[2] = {Leaf, Subleaf};
#else
    // TODO(fxbug.dev/27083): GCC doesn't honor the section attribute in a
    // COMDAT context.  Instead, just do on-demand initialization right here.
    // This bloats the code at each use and is less efficient than simply
    // preloading all the data at startup via the special section hack.
    static CpuidIo gCpuidIo = []() {
      using MaxLeaf = std::conditional_t<
          (Leaf < CpuidMaximumHypervisorLeaf::kLeaf), CpuidMaximumLeaf,
          std::conditional_t<(Leaf < CpuidMaximumExtendedLeaf::kLeaf), CpuidMaximumHypervisorLeaf,
                             CpuidMaximumExtendedLeaf>>;
      if (Leaf > BootCpuidIo{}.Read<MaxLeaf>().reg_value()) {
        return CpuidIo{};
      }
      CpuidIo data;
      __cpuid_count(Leaf, Subleaf,  //
                    data.values_[CpuidIo::kEax], data.values_[CpuidIo::kEbx],
                    data.values_[CpuidIo::kEcx], data.values_[CpuidIo::kEdx]);
      return data;
    }();
#endif  // __clang__
    return &gCpuidIo;
  }
};

// Call this once early in startup, before any uses of arch::BootCpuIdIo.  It
// initializes all the CpuidIo values in link map by using the CPUID
// instruction.  See below for implementation details.
extern "C" void InitializeBootCpuid();

// Convenient accessor for BootCpuidIo data, e.g.
// ```
// bool have_avx = arch::BootCpuid<arch::CpuidFeatureFlagsC>().avx();
// ```
template <typename CpuidValue>
inline auto BootCpuid() {
  return CpuidValue::Get().ReadFrom(BootCpuidIo{}.Get<CpuidValue>());
}

// Whether the leaf assosiated with a CPUID value type is supported, according
// to BootCpuidIo.
template <typename CpuidValue>
inline bool BootCpuidSupports() {
  return CpuidSupports<CpuidValue>(BootCpuidIo{});
}

// Explicit specializations for types used from assembly make it possible
// to give them unmangled (extern "C") names that are usable from assembly.

template <>
inline const CpuidIo* BootCpuidIo::GetLeaf<CpuidMaximumLeaf::kLeaf>() const {
  return &internal::gBootCpuid0;
}

template <>
inline const CpuidIo* BootCpuidIo::GetLeaf<CpuidMaximumHypervisorLeaf::kLeaf>() const {
  return &internal::gBootCpuidHyp0;
}

template <>
inline const CpuidIo* BootCpuidIo::GetLeaf<CpuidMaximumExtendedLeaf::kLeaf>() const {
  return &internal::gBootCpuidExt0;
}

template <>
inline const CpuidIo* BootCpuidIo::GetLeaf<CpuidFeatureFlagsC::kLeaf>() const {
  return &internal::gBootCpuidFeature;
}

template <>
inline const CpuidIo* BootCpuidIo::GetLeaf<CpuidExtendedFeatureFlagsB::kLeaf>() const {
  return &internal::gBootCpuidExtf;
}

}  // namespace arch

#endif  // ZIRCON_KERNEL_LIB_ARCH_X86_INCLUDE_LIB_ARCH_X86_BOOT_CPUID_H_
