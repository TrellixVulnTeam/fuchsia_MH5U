// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "lib/acpi_lite.h"

#include <inttypes.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <pretty/hexdump.h>

#include "debug.h"

#define LOCAL_TRACE 0

namespace acpi_lite {
namespace {

// Map a variable-length structure into memory.
//
// Perform a two-phase PhysToPtr conversion:
//
//   1. We first read a fixed-sized header.
//   2. We next determine the length of the structure by reading the fields.
//   3. We finally map in the full size of the structure.
//
// This allows us to handle the common use-case where the number of bytes that need
// to be accessed at a particular address cannot be determined until we first read
// a header at that address.
template <typename T>
zx::status<const T*> MapStructure(PhysMemReader& reader, zx_paddr_t phys) {
  // Try and read the header.
  zx::status<const void*> result = reader.PhysToPtr(phys, sizeof(T));
  if (result.is_error()) {
    return result.take_error();
  }
  const T* header = static_cast<const T*>(result.value());

  // Ensure that the length looks reasonable.
  if (header->size() < sizeof(T)) {
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }

  // Get the number of bytes the full structure needs, as determined by its header.
  result = reader.PhysToPtr(phys, header->size());
  if (result.is_error()) {
    return result.take_error();
  }

  return zx::success(static_cast<const T*>(result.value()));
}

bool ValidateRsdp(const AcpiRsdp* rsdp) {
  // Verify the RSDP signature.
  if (rsdp->sig1 != AcpiRsdp::kSignature1 || rsdp->sig2 != AcpiRsdp::kSignature2) {
    return false;
  }

  // Validate the checksum on the V1 header.
  if (!AcpiChecksumValid(rsdp, sizeof(AcpiRsdp))) {
    return false;
  }

  return true;
}

struct RootSystemTableDetails {
  uint64_t rsdp_address;
  uint32_t rsdt_address;
  uint64_t xsdt_address;
};

zx::status<RootSystemTableDetails> ParseRsdp(PhysMemReader& reader, zx_paddr_t rsdp_pa) {
  // Read the header.
  zx::status<const void*> maybe_rsdp_v1 = reader.PhysToPtr(rsdp_pa, sizeof(AcpiRsdp));
  if (maybe_rsdp_v1.is_error()) {
    return maybe_rsdp_v1.take_error();
  }
  auto* rsdp_v1 = static_cast<const AcpiRsdp*>(maybe_rsdp_v1.value());

  // Verify the V1 header details.
  if (!ValidateRsdp(rsdp_v1)) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  // If this is just a V1 RSDP, parse it and finish up.
  if (rsdp_v1->revision < 2) {
    return zx::success(RootSystemTableDetails{
        .rsdp_address = rsdp_pa,
        .rsdt_address = rsdp_v1->rsdt_address,
        .xsdt_address = 0,
    });
  }

  // Try and map the larger V2 structure.
  zx::status<const AcpiRsdpV2*> rsdp_v2 = MapStructure<AcpiRsdpV2>(reader, rsdp_pa);
  if (rsdp_v2.is_error()) {
    return rsdp_v2.take_error();
  }

  // Validate the checksum of the larger structure.
  if (!AcpiChecksumValid(rsdp_v2.value(), rsdp_v2.value()->length)) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  return zx::success(RootSystemTableDetails{
      .rsdp_address = rsdp_pa,
      .rsdt_address = rsdp_v2.value()->v1.rsdt_address,
      .xsdt_address = rsdp_v2.value()->xsdt_address,
  });
}

#if defined(__x86_64__) || defined(__i386__)
// Search for a valid RSDP in the BIOS read-only memory space in [0xe0000..0xfffff],
// on 16 byte boundaries.
//
// Return 0 if no RSDP found.
//
// Reference: ACPI v6.3, Section 5.2.5.1
zx::status<zx_paddr_t> FindRsdpPc(PhysMemReader& reader) {
  // Get a virtual address for the read-only BIOS range.
  zx::status<const void*> maybe_bios_section =
      reader.PhysToPtr(kBiosReadOnlyAreaStart, kBiosReadOnlyAreaLength);
  if (maybe_bios_section.is_error()) {
    return maybe_bios_section.take_error();
  }
  auto* bios_section = static_cast<const uint8_t*>(maybe_bios_section.value());

  // Try every 16-byte offset from 0xe0'0000 to 0xff'ffff, until we have no room left for an
  // AcpiRsdp struct.
  for (size_t offset = 0x0; offset <= kBiosReadOnlyAreaLength - sizeof(AcpiRsdp); offset += 16) {
    const auto rsdp = reinterpret_cast<const AcpiRsdp*>(bios_section + offset);
    if (ValidateRsdp(rsdp)) {
      return zx::success(kBiosReadOnlyAreaStart + offset);
    }
  }

  return zx::error(ZX_ERR_NOT_FOUND);
}
#endif

zx::status<RootSystemTableDetails> FindRootTables(PhysMemReader& physmem_reader,
                                                  zx_paddr_t rsdp_pa) {
  // If the user gave us an explicit RSDP, just use that directly.
  if (rsdp_pa != 0) {
    return ParseRsdp(physmem_reader, rsdp_pa);
  }

  // Otherwise, attempt to find it in a platform-specific way.
#if defined(__x86_64__) || defined(__i386__)
  {
    zx::status<zx_paddr_t> result = FindRsdpPc(physmem_reader);
    if (result.is_ok()) {
      LOG_DEBUG("ACPI LITE: Found RSDP at physical address %#" PRIxPTR ".\n", result.value());
      return ParseRsdp(physmem_reader, result.value());
    }
    LOG_INFO("ACPI LITE: Couldn't find ACPI RSDP in BIOS area\n");
  }
#endif

  return zx::error(ZX_ERR_NOT_FOUND);
}

}  // namespace

bool AcpiChecksumValid(const void* buf, size_t len) {
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
  // If we are fuzzing, calculate by don't verify checksums.
  AcpiChecksum(buf, len);
  return true;
#else
  return AcpiChecksum(buf, len) == 0;
#endif
}

uint8_t AcpiChecksum(const void* _buf, size_t len) {
  uint8_t c = 0;

  const uint8_t* buf = static_cast<const uint8_t*>(_buf);
  for (size_t i = 0; i < len; i++) {
    c = static_cast<uint8_t>(c + buf[i]);
  }

  // The checksum is valid if the sum of bytes mod 256 == 0.
  //
  // We return "-c" here. This doesn't change a valid checksum, and allows
  // code calculating checksums to use to code:
  //
  //   foo.checksum = AcpiChecksum(&foo, sizeof(foo));
  //
  return -c;
}

zx::status<const AcpiRsdt*> ValidateRsdt(PhysMemReader& reader, uint32_t rsdt_pa,
                                         size_t* num_tables) {
  // Map in the RSDT.
  zx::status<const AcpiRsdt*> rsdt = MapStructure<AcpiRsdt>(reader, rsdt_pa);
  if (rsdt.is_error()) {
    return rsdt.take_error();
  }

  // Ensure we have an RSDT signature.
  if (rsdt.value()->header.sig != AcpiRsdt::kSignature) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  // Validate checksum.
  if (!AcpiChecksumValid(rsdt.value(), rsdt.value()->header.length)) {
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }

  // Ensure this is a revision we understand.
  if (rsdt.value()->header.revision != 1) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  // Compute the number of tables we have.
  *num_tables = (rsdt.value()->header.length - sizeof(AcpiSdtHeader)) / sizeof(uint32_t);

  return rsdt;
}

zx::status<const AcpiXsdt*> ValidateXsdt(PhysMemReader& reader, uint64_t xsdt_pa,
                                         size_t* num_tables) {
  // Map in the XSDT.
  zx::status<const AcpiXsdt*> xsdt = MapStructure<AcpiXsdt>(reader, xsdt_pa);
  if (xsdt.is_error()) {
    return xsdt.take_error();
  }

  // Ensure we have an XSDT signature.
  if (xsdt.value()->header.sig != AcpiXsdt::kSignature) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  // Validate checksum.
  if (!AcpiChecksumValid(xsdt.value(), xsdt.value()->header.length)) {
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }

  // Ensure this is a revision we understand.
  if (xsdt.value()->header.revision != 1) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  // Compute the number of tables we have.
  *num_tables = (xsdt.value()->header.length - sizeof(AcpiSdtHeader)) / sizeof(uint64_t);

  return xsdt;
}

zx_paddr_t AcpiParser::GetTablePhysAddr(size_t index) const {
  if (index >= num_tables_) {
    return 0;
  }

  // Get the physical address for the index'th table.
  return xsdt_ != nullptr ? xsdt_->addr64[index] : rsdt_->addr32[index];
}

const AcpiSdtHeader* AcpiParser::GetTableAtIndex(size_t index) const {
  zx_paddr_t paddr = GetTablePhysAddr(index);
  if (paddr == 0) {
    return nullptr;
  }

  // Map it in.
  return MapStructure<AcpiSdtHeader>(*reader_, paddr).value_or(nullptr);
}

const AcpiSdtHeader* GetTableBySignature(const AcpiParserInterface& parser, AcpiSignature sig) {
  size_t num_tables = parser.num_tables();
  for (size_t i = 0; i < num_tables; i++) {
    const AcpiSdtHeader* header = parser.GetTableAtIndex(i);
    if (!header) {
      continue;
    }

    // Continue searching if the header doesn't match.
    if (sig != header->sig) {
      continue;
    }

    // If the checksum is invalid, keep searching.
    if (!AcpiChecksumValid(header, header->length)) {
      continue;
    }

    return header;
  }

  return nullptr;
}

zx::status<AcpiParser> AcpiParser::Init(PhysMemReader& physmem_reader, zx_paddr_t rsdp_pa) {
  // Find the root tables.
  zx::status<RootSystemTableDetails> root_tables = FindRootTables(physmem_reader, rsdp_pa);
  if (root_tables.is_error()) {
    LOG_INFO("ACPI LITE: Could not validate RSDP structure: %" PRId32 "\n",
             root_tables.error_value());
    return root_tables.take_error();
  }

  // Validate the tables.
  auto parser = [&]() -> zx::status<AcpiParser> {
    // If an XSDT table exists, try using it first.
    if (root_tables.value().xsdt_address != 0) {
      size_t num_tables = 0;
      zx::status<const AcpiXsdt*> xsdt =
          ValidateXsdt(physmem_reader, root_tables.value().xsdt_address, &num_tables);
      if (xsdt.is_ok()) {
        LOG_DEBUG("ACPI LITE: Found valid XSDT table at physical address %#" PRIx64 "\n",
                  root_tables.value().xsdt_address);
        return zx::success(AcpiParser(physmem_reader, root_tables.value().rsdp_address,
                                      /*rsdt=*/nullptr, xsdt.value(), num_tables,
                                      root_tables.value().xsdt_address));
      }
      LOG_DEBUG("ACPI LITE: Invalid XSDT table at physical address %#" PRIx64 "\n",
                root_tables.value().xsdt_address);
    }

    // Otherwise, try using the RSDT.
    if (root_tables.value().rsdt_address != 0) {
      size_t num_tables = 0;
      zx::status<const AcpiRsdt*> rsdt =
          ValidateRsdt(physmem_reader, root_tables.value().rsdt_address, &num_tables);
      if (rsdt.is_ok()) {
        LOG_DEBUG("ACPI LITE: Found valid RSDT table at physical address %#" PRIx32 "\n",
                  root_tables.value().rsdt_address);
        return zx::success(AcpiParser(physmem_reader, root_tables.value().rsdp_address,
                                      rsdt.value(), /*xsdt=*/nullptr, num_tables,
                                      root_tables.value().rsdt_address));
      }
      LOG_DEBUG("ACPI LITE: Invalid RSDT table at physical address %#" PRIx32 "\n",
                root_tables.value().rsdt_address);
    }

    // Nothing found.
    return zx::error(ZX_ERR_NOT_FOUND);
  }();

  if (LOCAL_TRACE && parser.is_ok()) {
    parser.value().DumpTables();
  }

  return parser;
}

void AcpiParser::DumpTables() const {
  printf("root table at paddr %#" PRIxPTR ":\n", root_table_addr_);
  if (xsdt_ != nullptr) {
    hexdump(xsdt_, xsdt_->header.length);
  } else {
    ZX_DEBUG_ASSERT(rsdt_ != nullptr);
    hexdump(rsdt_, rsdt_->header.length);
  }

  // walk the table list
  for (size_t i = 0; i < num_tables_; i++) {
    const auto header = GetTableAtIndex(i);
    if (!header) {
      continue;
    }

    char name[AcpiSignature::kAsciiLength + 1];
    header->sig.WriteToBuffer(name);
    printf("table %zu: '%s' at paddr %#" PRIxPTR ", len %" PRIu32 "\n", i, name,
           GetTablePhysAddr(i), header->length);
    hexdump(header, header->length);
  }
}

}  // namespace acpi_lite
