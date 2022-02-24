// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/acpi_lite.h>
#include <lib/acpi_lite/debug_port.h>

#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

namespace acpi_lite {
namespace {

void TestOneInput(FuzzedDataProvider& provider) {
  // Get the test data.
  std::vector<uint8_t> data = provider.ConsumeRemainingBytes<uint8_t>();

  // Ensure we have at least enough bytes for a valid header.
  if (data.size() < sizeof(AcpiDbg2Table)) {
    return;
  }

  // Update |length| to match the actual data length.
  auto* table = reinterpret_cast<AcpiDbg2Table*>(data.data());
  table->header.length = static_cast<uint32_t>(data.size());

  // Try and parse the result.
  (void)ParseAcpiDbg2Table(*table);
}

}  // namespace
}  // namespace acpi_lite

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  acpi_lite::TestOneInput(provider);
  return 0;
}
