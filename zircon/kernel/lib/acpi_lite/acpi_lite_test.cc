// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/acpi_lite.h>
#include <lib/acpi_lite/testing/test_data.h>
#include <lib/acpi_lite/testing/test_util.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/status.h>
#include <string.h>

#include <initializer_list>
#include <memory>

#include <gtest/gtest.h>

namespace acpi_lite::testing {
namespace {

TEST(AcpiParser, NoRsdp) {
  NullPhysMemReader reader;
  zx::status<AcpiParser> result = AcpiParser::Init(reader, 0);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), ZX_ERR_NOT_FOUND);
}

TEST(AcpiParser, EmptyTables) {
  EmptyPhysMemReader reader;
  zx::status<AcpiParser> result = AcpiParser::Init(reader, 0);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), ZX_ERR_NOT_FOUND);
}

// Ensure that the named table exists, and passed some basic checks.
void VerifyTableExists(const AcpiParser& parser, const char* signature) {
  // Fetch the table.
  const AcpiSdtHeader* table = GetTableBySignature(parser, AcpiSignature(signature));
  ASSERT_TRUE(table != nullptr) << "Table does not exist.";

  // Ensure signature matches.
  EXPECT_EQ(memcmp(table, signature, 4), 0) << "Table has invalid signature.";

  // Ensure length is sensible.
  ASSERT_GE(table->length, sizeof(AcpiSdtHeader));
}

TEST(AcpiParser, ParseQemuTables) {
  FakePhysMemReader reader = QemuPhysMemReader();
  AcpiParser result = AcpiParser::Init(reader, reader.rsdp()).value();
  ASSERT_EQ(4u, result.num_tables());

  // Ensure we can read the HPET table.
  VerifyTableExists(result, "HPET");
}

TEST(AcpiParser, ParseIntelNucTables) {
  // Parse the QEMU tables.
  FakePhysMemReader reader = IntelNuc7i5dnPhysMemReader();
  AcpiParser result = AcpiParser::Init(reader, reader.rsdp()).value();
  EXPECT_EQ(28u, result.num_tables());
  VerifyTableExists(result, "HPET");
  VerifyTableExists(result, "DBG2");
}

TEST(AcpiParser, ParseFuchsiaHypervisor) {
  FakePhysMemReader reader = FuchsiaHypervisorPhysMemReader();
  AcpiParser result = AcpiParser::Init(reader, reader.rsdp()).value();
  EXPECT_EQ(result.num_tables(), 3u);
}

TEST(AcpiParser, ReadMissingTable) {
  // Parse the QEMU tables.
  FakePhysMemReader reader = QemuPhysMemReader();
  AcpiParser result = AcpiParser::Init(reader, reader.rsdp()).value();

  // Read a missing table.
  EXPECT_EQ(GetTableBySignature(result, AcpiSignature("AAAA")), nullptr);

  // Read a bad index.
  EXPECT_EQ(result.GetTableAtIndex(result.num_tables()), nullptr);
  EXPECT_EQ(result.GetTableAtIndex(~0), nullptr);
}

TEST(AcpiParser, AcpiChecksum) {
  // Empty checksum.
  EXPECT_TRUE(AcpiChecksumValid(nullptr, 0));

  // Valid checksum.
  {
    uint8_t buffer[1] = {0};
    EXPECT_TRUE(AcpiChecksumValid(&buffer, 1));
  }

  // Invalid checksum.
  {
    uint8_t buffer[1] = {52};
    EXPECT_FALSE(AcpiChecksumValid(&buffer, 1));
  }

  // Calculate a checksum.
  {
    uint8_t buffer[2] = {32, 0};
    EXPECT_FALSE(AcpiChecksumValid(&buffer, 2));
    buffer[1] = AcpiChecksum(&buffer, 2);
    EXPECT_TRUE(AcpiChecksumValid(&buffer, 2));
  }
}

TEST(AcpiParser, RsdtInvalidLengths) {
  // Create a RSDT with an invalid (too short) length.
  AcpiRsdt bad_rsdt = {
      .header =
          {
              .sig = AcpiRsdt::kSignature,
              .length = 10,  // covers checksum, but nothing else.
              .revision = 1,
              .checksum = 0,
          },
  };
  bad_rsdt.header.checksum = AcpiChecksum(&bad_rsdt, bad_rsdt.header.length);

  // Add the bad RSDT to a table set.
  FakePhysMemReader::Region region[] = {{
      .phys_addr = 0x1000,
      .data =
          cpp20::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&bad_rsdt), sizeof(AcpiRsdt)),
  }};

  // Attempt to parse the bad RSDT. Ensure we get an error.
  FakePhysMemReader reader(/*rsdp=*/0, region);
  size_t num_tables;
  EXPECT_TRUE(ValidateRsdt(reader, 0x1000, &num_tables).is_error());
}

TEST(AcpiParser, DumpTables) {
  // Parse the QEMU tables.
  FakePhysMemReader reader = QemuPhysMemReader();
  zx::status<AcpiParser> result = AcpiParser::Init(reader, reader.rsdp());
  ASSERT_FALSE(result.is_error());

  // Dump the (relatively short) QEMU tables.
  result->DumpTables();
}

// A PhysMemReader that emulates the BIOS read-only area between 0xe'0000 and 0xf'ffff.
class BiosAreaPhysMemReader : public PhysMemReader {
 public:
  explicit BiosAreaPhysMemReader(cpp20::span<const FakePhysMemReader::Region> regions)
      : fallback_(0, regions) {
    // Create a fake BIOS area.
    bios_area_ = std::make_unique<uint8_t[]>(kBiosReadOnlyAreaLength);

    // Copy any tables into the fake BIOS area.
    for (const auto& region : regions) {
      if (region.phys_addr >= kBiosReadOnlyAreaStart && region.phys_addr < kBiosReadOnlyAreaEnd) {
        memcpy(bios_area_.get() + region.phys_addr - kBiosReadOnlyAreaStart, region.data.data(),
               std::min(region.data.size_bytes(), kBiosReadOnlyAreaEnd - region.phys_addr));
      }
    }
  }

  zx::status<const void*> PhysToPtr(uintptr_t phys, size_t length) override {
    if (phys >= kBiosReadOnlyAreaStart && phys < kBiosReadOnlyAreaEnd &&
        phys + length <= kBiosReadOnlyAreaEnd) {
      return zx::success(&bios_area_[phys - kBiosReadOnlyAreaStart]);
    }

    return fallback_.PhysToPtr(phys, length);
  }

 private:
  static constexpr zx_paddr_t kBiosReadOnlyAreaEnd =
      kBiosReadOnlyAreaStart + kBiosReadOnlyAreaLength;
  std::unique_ptr<uint8_t[]> bios_area_;
  FakePhysMemReader fallback_;
};

TEST(AcpiParser, AcpiSignatureConstruct) {
  AcpiSignature sig("ABCD");

  // Ensure the in-memory representation is correct.
  EXPECT_TRUE(memcmp(&sig, "ABCD", 4) == 0);
}

TEST(AcpiParser, AcpiSignatureWriteToBuffer) {
  // Write out the signature.
  AcpiSignature sig("ABCD");
  char buff[5];
  sig.WriteToBuffer(buff);
  EXPECT_TRUE(strcmp("ABCD", buff) == 0);
}

// Test auto-detection of the location of the RSD PTR by searching the read-only BIOS
// aread.
#if defined(__x86_64__)
TEST(AcpiParser, RsdPtrAutodetect) {
  BiosAreaPhysMemReader reader(
      {QemuPhysMemReader().regions().data(), QemuPhysMemReader().regions().size()});
  zx::status<AcpiParser> result = AcpiParser::Init(reader, /*rsdp_pa=*/0);
  ASSERT_TRUE(!result.is_error());
  EXPECT_EQ(4u, result->num_tables());
}
#endif

TEST(GetTableByType, NothingFound) {
  FakeAcpiParser parser;
  EXPECT_EQ(nullptr, GetTableByType<AcpiHpetTable>(parser));
}

TEST(GetTableByType, ValidEntryFound) {
  AcpiHpetTable table = {
      .header =
          {
              .sig = AcpiHpetTable::kSignature,
              .length = sizeof(AcpiHpetTable),
          },
      .flags = 42,
  };
  table.header.checksum = AcpiChecksum(&table, sizeof(table));
  FakeAcpiParser parser({&table.header});

  const AcpiHpetTable* result = GetTableByType<AcpiHpetTable>(parser);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->flags, 42);
}

TEST(GetTableByType, ShortEntry) {
  AcpiHpetTable table = {
      .header =
          {
              .sig = AcpiHpetTable::kSignature,
              // Length is too short to hold a |AcpiHpetTable|.
              .length = sizeof(AcpiHpetTable) - 1,
          },
  };
  table.header.checksum = AcpiChecksum(&table, sizeof(table) - 1);
  FakeAcpiParser parser({&table.header});

  EXPECT_EQ(GetTableByType<AcpiHpetTable>(parser), nullptr);
}

}  // namespace
}  // namespace acpi_lite::testing
