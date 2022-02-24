// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_VM_INCLUDE_VM_VM_ASPACE_H_
#define ZIRCON_KERNEL_VM_INCLUDE_VM_VM_ASPACE_H_

#include <assert.h>
#include <lib/crypto/prng.h>
#include <zircon/types.h>

#include <arch/aspace.h>
#include <fbl/canary.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/macros.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <kernel/lockdep.h>
#include <kernel/mutex.h>
#include <vm/arch_vm_aspace.h>
#include <vm/vm.h>

class VmAddressRegion;
class VmEnumerator;
class VmMapping;
class VmAddressRegionOrMapping;

namespace hypervisor {
class GuestPhysicalAddressSpace;
}  // namespace hypervisor

class VmObject;

class VmAspace : public fbl::DoublyLinkedListable<VmAspace*>, public fbl::RefCounted<VmAspace> {
 public:
  enum class Type {
    User = 0,
    Kernel,
    // You probably do not want to use LOW_KERNEL. It is primarily used for SMP bootstrap or mexec
    // to allow mappings of very low memory using the standard VMM subsystem.
    LowKernel,
    // Used to construct an address space representing hypervisor guest memory.
    GuestPhys,
  };

  // Create an address space of the type specified in |type| with name |name|.
  //
  // Although reference counted, the returned VmAspace must be explicitly destroyed via Destroy.
  //
  // Returns null on failure (e.g. due to resource starvation).
  static fbl::RefPtr<VmAspace> Create(Type type, const char* name);

  // Destroy this address space.
  //
  // Destroy does not free this object, but rather allows it to be freed when the last retaining
  // RefPtr is destroyed.
  zx_status_t Destroy();

  void Rename(const char* name);

  // simple accessors
  vaddr_t base() const { return base_; }
  size_t size() const { return size_; }
  const char* name() const { return name_; }
  ArchVmAspace& arch_aspace() { return arch_aspace_; }
  bool is_user() const { return type_ == Type::User; }
  bool is_aslr_enabled() const { return aslr_enabled_; }

  // Get the root VMAR (briefly acquires the aspace lock)
  // May return nullptr if the aspace has been destroyed or is not yet initialized.
  fbl::RefPtr<VmAddressRegion> RootVmar();

  // Returns true if the address space has been destroyed.
  bool is_destroyed() const;

  // accessor for singleton kernel address space
  static VmAspace* kernel_aspace() { return kernel_aspace_; }

  // given an address, return either the kernel aspace or the current user one
  static VmAspace* vaddr_to_aspace(uintptr_t address);

  // set the per thread aspace pointer to this
  void AttachToThread(Thread* t);

  void Dump(bool verbose) const;
  void DumpLocked(bool verbose) const;

  static void DropAllUserPageTables();
  void DropUserPageTables();

  static void DumpAllAspaces(bool verbose);

  // Harvests all accessed information across all user mappings and updates any page age information
  // for terminal mappings, and potentially harvests page tables depending on the passed in action.
  // This requires holding the aspaces_list_lock_ over the entire duration and whilst not a commonly
  // used lock this function should still only be called infrequently to avoid monopolizing the
  // lock.
  using NonTerminalAction = ArchVmAspace::NonTerminalAction;
  using TerminalAction = ArchVmAspace::TerminalAction;
  static void HarvestAllUserAccessedBits(NonTerminalAction non_terminal_action,
                                         TerminalAction terminal_action);

  // Traverses the VM tree rooted at this node, in depth-first pre-order. If
  // any methods of |ve| return false, the traversal stops and this method
  // returns false. Returns true otherwise.
  bool EnumerateChildren(VmEnumerator* ve);

  // A collection of memory usage counts.
  struct vm_usage_t {
    // A count of pages covered by VmMapping ranges.
    size_t mapped_pages;

    // For the fields below, a page is considered committed if a VmMapping
    // covers a range of a VmObject that contains that page, and that page
    // has physical memory allocated to it.

    // A count of committed pages that are only mapped into this address
    // space.
    size_t private_pages;

    // A count of committed pages that are mapped into this and at least
    // one other address spaces.
    size_t shared_pages;

    // A number that estimates the fraction of shared_pages that this
    // address space is responsible for keeping alive.
    //
    // An estimate of:
    //   For each shared, committed page:
    //   scaled_shared_bytes +=
    //       PAGE_SIZE / (number of address spaces mapping this page)
    //
    // This number is strictly smaller than shared_pages * PAGE_SIZE.
    size_t scaled_shared_bytes;
  };

  // Counts memory usage under the VmAspace.
  zx_status_t GetMemoryUsage(vm_usage_t* usage);

  size_t AllocatedPages() const;

  // Generates a soft fault against this aspace. This is similar to a PageFault except:
  //  * This aspace may not currently be active and this does not have to be called from the
  //    hardware exception handler.
  //  * May be invoked spuriously in situations where the hardware mappings would have prevented a
  //    real PageFault from occurring.
  zx_status_t SoftFault(vaddr_t va, uint flags);

  // Generates an accessed flag fault against this aspace. This is a specialized version of
  // SoftFault that will only resolve a potential missing access flag and nothing else.
  zx_status_t AccessedFault(vaddr_t va);

  // Convenience method for traversing the tree of VMARs to find the deepest
  // VMAR in the tree that includes *va*.
  // Returns nullptr if the aspace has been destroyed or is not yet initialized.
  fbl::RefPtr<VmAddressRegionOrMapping> FindRegion(vaddr_t va);

  // For region creation routines
  static const uint VMM_FLAG_VALLOC_SPECIFIC = (1u << 0);  // allocate at specific address
  static const uint VMM_FLAG_COMMIT = (1u << 1);  // commit memory up front (no demand paging)

  // legacy functions to assist in the transition to VMARs
  // These all assume a flat VMAR structure in which all VMOs are mapped
  // as children of the root.  They will all assert if used on user aspaces
  // TODO(teisenbe): remove uses of these in favor of new VMAR interfaces
  zx_status_t ReserveSpace(const char* name, size_t size, vaddr_t vaddr);
  zx_status_t AllocPhysical(const char* name, size_t size, void** ptr, uint8_t align_pow2,
                            paddr_t paddr, uint vmm_flags, uint arch_mmu_flags);
  zx_status_t AllocContiguous(const char* name, size_t size, void** ptr, uint8_t align_pow2,
                              uint vmm_flags, uint arch_mmu_flags);
  zx_status_t Alloc(const char* name, size_t size, void** ptr, uint8_t align_pow2, uint vmm_flags,
                    uint arch_mmu_flags);
  zx_status_t FreeRegion(vaddr_t va);

  // Internal use function for mapping VMOs.  Do not use.  This is exposed in
  // the public API purely for tests.
  zx_status_t MapObjectInternal(fbl::RefPtr<VmObject> vmo, const char* name, uint64_t offset,
                                size_t size, void** ptr, uint8_t align_pow2, uint vmm_flags,
                                uint arch_mmu_flags);

  uintptr_t vdso_base_address() const;
  uintptr_t vdso_code_address() const;

  // Helper function to test for collision with vdso_code_mapping_.
  bool IntersectsVdsoCode(vaddr_t base, size_t size) const;

 protected:
  // Share the aspace lock with VmAddressRegion/VmMapping so they can serialize
  // changes to the aspace.
  friend class VmAddressRegionOrMapping;
  friend class VmAddressRegion;
  friend class VmMapping;
  Lock<Mutex>* lock() const TA_RET_CAP(lock_) { return &lock_; }
  Lock<Mutex>& lock_ref() const TA_RET_CAP(lock_) { return lock_; }

  // Expose the PRNG for ASLR to VmAddressRegion
  crypto::Prng& AslrPrng() {
    DEBUG_ASSERT(aslr_enabled_);
    return aslr_prng_;
  }

  uint8_t AslrEntropyBits(bool compact) const {
    return compact ? aslr_compact_entropy_bits_ : aslr_entropy_bits_;
  }

 private:
  friend lazy_init::Access;

  // can only be constructed via factory or LazyInit
  VmAspace(vaddr_t base, size_t size, Type type, const char* name);

  DISALLOW_COPY_ASSIGN_AND_MOVE(VmAspace);

  // private destructor that can only be used from the ref ptr
  ~VmAspace();
  friend fbl::RefPtr<VmAspace>;

  // complete initialization, may fail in OOM cases
  zx_status_t Init();

  void InitializeAslr();

  // Returns whether this aspace is marked as latency sensitive.
  bool IsLatencySensitive() const;

  // Sets this aspace as being latency sensitive. This cannot be undone.
  void MarkAsLatencySensitive();

  // Encodes the idea that we can always unmap from user aspaces.
  ArchVmAspace::EnlargeOperation EnlargeArchUnmap() const {
    return is_user() ? ArchVmAspace::EnlargeOperation::Yes : ArchVmAspace::EnlargeOperation::No;
  }

  // internal page fault routine, friended to be only called by vmm_page_fault_handler
  zx_status_t PageFault(vaddr_t va, uint flags);
  friend zx_status_t vmm_page_fault_handler(vaddr_t va, uint flags);
  friend class hypervisor::GuestPhysicalAddressSpace;

  // magic
  fbl::Canary<fbl::magic("VMAS")> canary_;

  // members
  const vaddr_t base_;
  const size_t size_;
  const Type type_;
  char name_[32];
  bool aspace_destroyed_ = false;
  bool aslr_enabled_ = false;
  uint8_t aslr_entropy_bits_ = 0;
  uint8_t aslr_compact_entropy_bits_ = 0;

  // TODO(fxb/85056): This is a temporary solution and needs to be replaced with something that is
  // formalized.
  // Indicates whether or not this aspace is considered a latency sensitive object. For an aspace,
  // being latency sensitive means it will not perform page table reclamation, and will also pass on
  // this tag to any VMOs that get mapped into it. This is an atomic so that it can be safely read
  // outside the lock, however writes should occur inside the lock.
  ktl::atomic<bool> is_latency_sensitive_ = false;

  mutable DECLARE_MUTEX(VmAspace) lock_;

  // Keep a cache of the VmMapping of the last PageFault that occurred. On a page fault this can be
  // checked to see if it matches more quickly than walking the full vmar tree. Mappings that are
  // stored here must be in the ALIVE state, implying that they are in the VMAR tree. It is then the
  // responsibility of the VmMapping to remove itself from here should it transition out of ALIVE,
  // and remove itself from the VMAR tree.
  // A raw pointer is stored here since the VmMapping must be alive and in tree anyway and if it
  // were a RefPtr we would not be able to handle being the one to drop the last ref and perform
  // destruction.
  VmMapping* last_fault_ TA_GUARDED(lock_) = nullptr;

  // root of virtual address space
  // Access to this reference is guarded by lock_.
  fbl::RefPtr<VmAddressRegion> root_vmar_;

  // PRNG used by VMARs for address choices.  We record the seed to enable
  // reproducible debugging.
  crypto::Prng aslr_prng_;
  uint8_t aslr_seed_[crypto::Prng::kMinEntropy];

  // architecturally specific part of the aspace
  ArchVmAspace arch_aspace_;

  fbl::RefPtr<VmMapping> vdso_code_mapping_;

  // The number of page table reclamations attempted since last active. This is used since we need
  // to perform pt reclamation twice in a row (once to clear accessed bits, another time to reclaim
  // page tables) before the aspace is at a fixed point and we can actually stop performing the
  // harvests.
  uint32_t pt_harvest_since_active_ TA_GUARDED(AspaceListLock::Get()) = 0;

  // TODO(fxbug.dev/76417): Remove this once bug has been resolved.
  // To help track down a flake we record a backtrace at the point the aspace is destroyed. The
  // backtrace gets printed out if the aspace is attempted to be used. This allows correlating why
  // the aspace was destroyed at the point we know the flake has happened.
  // Due to the extra memory required this is restricted to debug builds only.
#ifdef DEBUG_ASSERT_IMPLEMENTED
  Backtrace destroyed_bt_;
#endif

  DECLARE_SINGLETON_MUTEX(AspaceListLock);
  static fbl::DoublyLinkedList<VmAspace*> aspaces_list_ TA_GUARDED(AspaceListLock::Get());

  // initialization routines need to construct the singleton kernel address space
  // at a particular points in the bootup process
  static void KernelAspaceInitPreHeap();
  static VmAspace* kernel_aspace_;
  friend void vm_init_preheap();
};

#endif  // ZIRCON_KERNEL_VM_INCLUDE_VM_VM_ASPACE_H_
