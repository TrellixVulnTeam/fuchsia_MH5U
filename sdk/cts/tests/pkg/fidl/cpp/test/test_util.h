// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CTS_TESTS_PKG_FIDL_CPP_TEST_TEST_UTIL_H_
#define CTS_TESTS_PKG_FIDL_CPP_TEST_TEST_UTIL_H_

#include <lib/fidl/internal.h>

#include <cstdint>
#include <ios>
#include <iostream>

#include <zxtest/zxtest.h>

#include "lib/fidl/cpp/clone.h"
#include "sdk/lib/fidl/cpp/message.h"

namespace fidl {
namespace test {
namespace util {

constexpr fidl_message_header_t kV1Header = {
    .magic_number = kFidlWireFormatMagicNumberInitial,
};
constexpr fidl_message_header_t kV2Header = {
    .flags = {FIDL_MESSAGE_HEADER_FLAGS_0_USE_VERSION_V2, 0, 0},
    .magic_number = kFidlWireFormatMagicNumberInitial,
};

inline bool operator==(zx_handle_disposition_t a, zx_handle_disposition_t b) {
  return a.operation == b.operation && a.handle == b.handle && a.type == b.type &&
         a.rights == b.rights && a.result == b.result;
}
inline bool operator!=(zx_handle_disposition_t a, zx_handle_disposition_t b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const zx_handle_disposition_t& hd) {
  return os << "zx_handle_disposition_t{\n"
            << "  .operation = " << hd.operation << "\n"
            << "  .handle = " << hd.handle << "\n"
            << "  .type = " << hd.type << "\n"
            << "  .rights = " << hd.rights << "\n"
            << "  .result = " << hd.result << "\n"
            << "}\n";
}

template <typename T>
bool cmp_payload(const T* actual, size_t actual_size, const T* expected, size_t expected_size) {
  bool pass = true;
  for (size_t i = 0; i < actual_size && i < expected_size; i++) {
    if (actual[i] != expected[i]) {
      pass = false;
      if constexpr (std::is_same_v<T, zx_handle_disposition_t>) {
        std::cout << std::dec << "element[" << i << "]: actual=" << actual[i]
                  << " expected=" << expected[i];
      } else {
        std::cout << std::dec << "element[" << i << "]: " << std::hex << "actual=0x" << +actual[i]
                  << " "
                  << "expected=0x" << +expected[i] << "\n";
      }
    }
  }
  if (actual_size != expected_size) {
    pass = false;
    std::cout << std::dec << "element[...]: "
              << "actual.size=" << +actual_size << " "
              << "expected.size=" << +expected_size << "\n";
  }
  return pass;
}

template <class Output, class Input>
Output RoundTrip(const Input& input) {
  fidl::BodyEncoder encoder(::fidl::internal::WireFormatVersion::kV1);
  auto offset = encoder.Alloc(EncodingInlineSize<Input, fidl::BodyEncoder>(&encoder));
  fidl::Clone(input).Encode(&encoder, offset);
  auto oubgoing_body = encoder.GetBody();
  const char* err_msg = nullptr;
  EXPECT_EQ(
      ZX_OK,
      oubgoing_body.Validate(::fidl::internal::WireFormatVersion::kV1, Output::FidlType, &err_msg),
      "%s", err_msg);

  std::vector<zx_handle_info_t> handle_infos(oubgoing_body.handles().actual());
  EXPECT_EQ(ZX_OK,
            FidlHandleDispositionsToHandleInfos(oubgoing_body.handles().data(), handle_infos.data(),
                                                oubgoing_body.handles().actual()));
  fidl::HLCPPIncomingBody incoming_body(
      BytePart(oubgoing_body.bytes(), 0),
      HandleInfoPart(handle_infos.data(), static_cast<uint32_t>(handle_infos.size())));
  oubgoing_body.ClearHandlesUnsafe();
  EXPECT_EQ(
      ZX_OK,
      incoming_body.Decode(fidl::internal::WireFormatMetadata::FromTransactionalHeader(kV1Header),
                           Output::FidlType, &err_msg),
      "%s", err_msg);

  fidl::Decoder decoder(std::move(incoming_body));
  Output output;
  Output::Decode(&decoder, &output, 0);
  return output;
}

template <class Output>
Output DecodedBytes(std::vector<uint8_t> input) {
  HLCPPIncomingBody body(BytePart(input.data(), static_cast<uint32_t>(input.capacity()),
                                  static_cast<uint32_t>(input.size())),
                         HandleInfoPart());

  const char* error = nullptr;
  EXPECT_EQ(ZX_OK,
            body.Decode(fidl::internal::WireFormatMetadata::FromTransactionalHeader(kV1Header),
                        Output::FidlType, &error),
            "%s", error);

  fidl::Decoder decoder(std::move(body));
  Output output;
  Output::Decode(&decoder, &output, 0);

  return output;
}

template <class Output>
Output DecodedBytes(const fidl_message_header_t& header, std::vector<uint8_t> bytes,
                    std::vector<zx_handle_info_t> handle_infos) {
  HLCPPIncomingBody body(
      BytePart(bytes.data(), static_cast<uint32_t>(bytes.capacity()),
               static_cast<uint32_t>(bytes.size())),
      HandleInfoPart(handle_infos.data(), static_cast<uint32_t>(handle_infos.capacity()),
                     static_cast<uint32_t>(handle_infos.size())));
  bytes.resize(bytes.capacity());  // To avoid container overflow during V2 -> V1 transform

  const char* error = nullptr;
  EXPECT_EQ(ZX_OK,
            body.Decode(fidl::internal::WireFormatMetadata::FromTransactionalHeader(header),
                        Output::FidlType, &error),
            "%s", error);

  fidl::Decoder decoder(std::move(body));
  Output output;
  Output::Decode(&decoder, &output, 0);

  return output;
}

template <class Input>
void ForgetHandles(internal::WireFormatVersion wire_format, Input input) {
  // Encode purely for the side effect of linearizing the handles.
  fidl::BodyEncoder enc(wire_format);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::BodyEncoder>(&enc));
  input.Encode(&enc, offset);
  enc.GetBody().ClearHandlesUnsafe();
}

template <class Input>
bool ValueToBytes(const Input& input, const std::vector<uint8_t>& expected) {
  fidl::BodyEncoder enc(::fidl::internal::WireFormatVersion::kV1);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::BodyEncoder>(&enc));
  fidl::Clone(input).Encode(&enc, offset);
  auto msg = enc.GetBody();
  return cmp_payload(msg.bytes().data(), msg.bytes().actual(), expected.data(), expected.size());
}

template <class Input>
bool ValueToBytes(internal::WireFormatVersion wire_format, Input input,
                  const std::vector<uint8_t>& bytes,
                  const std::vector<zx_handle_disposition_t>& handles, bool check_rights = true) {
  fidl::BodyEncoder enc(wire_format);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::BodyEncoder>(&enc));
  input.Encode(&enc, offset);
  HLCPPOutgoingBody body = enc.GetBody();
  auto bytes_match =
      cmp_payload(body.bytes().data(), body.bytes().actual(), bytes.data(), bytes.size());
  bool handles_match = false;
  if (check_rights) {
    handles_match =
        cmp_payload(body.handles().data(), body.handles().actual(), handles.data(), handles.size());
  } else {
    zx_handle_t body_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    for (uint32_t i = 0; i < body.handles().actual(); i++) {
      body_handles[i] = body.handles().data()[i].handle;
    }
    zx_handle_t expected_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    for (uint32_t i = 0; i < handles.size(); i++) {
      expected_handles[i] = handles.data()[i].handle;
    }
    handles_match =
        cmp_payload(body_handles, body.handles().actual(), expected_handles, handles.size());
  }
  const char* validation_error = nullptr;
  zx_status_t validation_status = body.Validate(wire_format, Input::FidlType, &validation_error);
  if (validation_status != ZX_OK) {
    std::cout << "Validator exited with status " << validation_status << std::endl;
  }
  if (validation_error) {
    std::cout << "Validator error " << validation_error << std::endl;
  }
  return bytes_match && handles_match && (validation_status == ZX_OK);
}

template <class Output>
void CheckDecodeFailure(const fidl_message_header_t& header, std::vector<uint8_t> input,
                        std::vector<zx_handle_info_t> handle_infos,
                        const zx_status_t expected_failure_code) {
  HLCPPIncomingBody body(
      BytePart(input.data(), static_cast<uint32_t>(input.capacity()),
               static_cast<uint32_t>(input.size())),
      HandleInfoPart(handle_infos.data(), static_cast<uint32_t>(handle_infos.capacity()),
                     static_cast<uint32_t>(handle_infos.size())));
  input.resize(input.capacity());  // To avoid container overflow during V2 -> V1 transform

  const char* error = nullptr;
  EXPECT_EQ(expected_failure_code,
            body.Decode(fidl::internal::WireFormatMetadata::FromTransactionalHeader(header),
                        Output::FidlType, &error),
            "%s", error);
}

template <class Input>
void CheckEncodeFailure(internal::WireFormatVersion wire_format, const Input& input,
                        const zx_status_t expected_failure_code) {
  fidl::BodyEncoder enc(wire_format);
  auto offset = enc.Alloc(EncodingInlineSize<Input, fidl::BodyEncoder>(&enc));
  fidl::Clone(input).Encode(&enc, offset);
  auto msg = enc.GetBody();
  const char* error = nullptr;
  EXPECT_EQ(expected_failure_code, msg.Validate(wire_format, Input::FidlType, &error), "%s", error);
}

}  // namespace util
}  // namespace test
}  // namespace fidl

#endif  // CTS_TESTS_PKG_FIDL_CPP_TEST_TEST_UTIL_H_
