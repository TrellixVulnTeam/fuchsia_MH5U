// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>

#include <gtest/gtest.h>
#include <wlan/common/element_splitter.h>
#include <wlan/common/write_element.h>
#include <wlan/mlme/ap/tim.h>
#include <wlan/mlme/client/client_mlme.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/validate_frame.h>

#include "mlme_msg.h"
#include "mock_device.h"
#include "test_bss.h"
#include "test_utils.h"

namespace wlan {

namespace {

namespace wlan_ieee80211 = ::fuchsia::wlan::ieee80211;
namespace wlan_mlme = ::fuchsia::wlan::mlme;

constexpr uint8_t kTestPayload[] = "Hello Fuchsia";
// We check the auto deauthentication every time the signal report timeout fires.
// This matches Rust MLME's pub const ASSOCIATION_STATUS_TIMEOUT_BEACON_COUNT: u32 = 10;
constexpr size_t kAssociationStatusBeaconCount = 10;

wlan_client_mlme_config_t ClientTestConfig() {
  return wlan_client_mlme_config_t{
      // Set to 0 to more easily control the timing for going on- and off-channel so that
      // auto-deauth tests are simpler
      .ensure_on_channel_time = 0,
  };
}

struct ClientTest : public ::testing::Test {
  ClientTest() : device(), client(&device, ClientTestConfig(), true) {}

  void SetUp() override {
    client.Init();
    client.RunUntilStalled();
  }

  void SendWlanPacket(std::unique_ptr<Packet> packet) {
    device.SendWlanPacket(std::move(packet));
    client.RunUntilStalled();
  }

  template <typename T>
  MlmeMsg<T> AssertNextMsgFromSmeChannel() {
    client.RunUntilStalled();
    return device.AssertNextMsgFromSmeChannel<T>();
  }

  template <typename T>
  std::optional<MlmeMsg<T>> GetNextMsgFromSmeChannel() {
    client.RunUntilStalled();
    return device.GetNextMsgFromSmeChannel<T>();
  }

  zx_status_t QueueEthPacket(std::unique_ptr<Packet> pkt) {
    auto status = client.QueueEthFrameTx(std::move(pkt));
    client.RunUntilStalled();
    return status;
  }

  zx_status_t SendNullDataFrame() {
    auto frame = CreateNullDataFrame();
    if (frame.IsEmpty()) {
      return ZX_ERR_NO_RESOURCES;
    }
    SendWlanPacket(frame.Take());
    return ZX_OK;
  }

  void SendBeaconFrame(const common::MacAddr& bssid = common::MacAddr(kBssid1)) {
    SendWlanPacket(CreateBeaconFrame(bssid));
  }

  void Join(bool rsne = true) {
    device.sme_->JoinReq(CreateJoinRequest(rsne));
    client.RunUntilStalled();
    AssertNextMsgFromSmeChannel<wlan_mlme::JoinConfirm>();
  }

  void Authenticate() {
    device.sme_->AuthenticateReq(CreateAuthRequest());
    client.RunUntilStalled();
    SendWlanPacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem));
    AssertNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>();
    device.wlan_queue.clear();
  }

  void Associate(bool rsne = true) {
    device.sme_->AssociateReq(CreateAssocRequest(rsne));
    client.RunUntilStalled();
    SendWlanPacket(CreateAssocRespFrame());
    AssertNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>();
    device.wlan_queue.clear();
  }

  void SetKey() {
    auto key_data = std::vector(std::cbegin(kKeyData), std::cend(kKeyData));
    device.sme_->SetKeysReq(
        CreateSetKeysRequest(common::MacAddr(kBssid1), key_data, wlan_mlme::KeyType::PAIRWISE));
    client.RunUntilStalled();
  }

  void EstablishRsna() {
    device.sme_->SetControlledPort(
        CreateSetCtrlPortRequest(common::MacAddr(kBssid1), wlan_mlme::ControlledPortState::OPEN));
    client.RunUntilStalled();
  }

  void Connect(bool rsne = true) {
    Join(rsne);
    Authenticate();
    Associate(rsne);
    if (rsne) {
      EstablishRsna();
    }
  }

  zx::duration BeaconPeriodsToDuration(size_t periods) {
    return zx::usec(1024) * (periods * kBeaconPeriodTu);
  }

  void IncreaseTimeByBeaconPeriods(size_t periods) {
    client.AdvanceFakeTime(BeaconPeriodsToDuration(periods).to_nsecs());
  }

  // Auto deauthentication is checked when association status check timeout fires so this is to
  // mirror the behavior in MLME. The same timeout also triggers SignalRepport.
  void AdvanceAutoDeauthenticationTimerByBeaconPeriods(size_t periods) {
    for (size_t i = 0; i < periods / kAssociationStatusBeaconCount; i++) {
      IncreaseTimeByBeaconPeriods(kAssociationStatusBeaconCount);
      // MLME may go off channel if deauthentication occurs. In this case, our next SME message
      // should be a SignalReportIndication.
      GetNextMsgFromSmeChannel<::fuchsia::wlan::internal::SignalReportIndication>();
    }
  }

  // Go off channel. This assumes that any existing ensure-on-channel flag is already cleared
  void GoOffChannel(uint16_t beacon_periods) {
    device.sme_->StartScan(CreatePassiveScanRequest(beacon_periods * kBeaconPeriodTu));
    client.RunUntilStalled();
    device.wlan_queue.erase(device.wlan_queue.begin());  // dequeue power-saving frame
  }

  void AssertGoingOnChannel() {
    ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
    // We always use scanning in these tests to force off-channel, and scanning is invoked
    // once at a time, so wlan_mlme::ScanEnd is an indicator of returning to an on-channel state.
    AssertNextMsgFromSmeChannel<wlan_mlme::ScanEnd>();   // Find scan end message.
    device.wlan_queue.erase(device.wlan_queue.begin());  // dequeue power-saving frame
  }

  void AssertAuthConfirm(MlmeMsg<wlan_mlme::AuthenticateConfirm> msg,
                         wlan_ieee80211::StatusCode result_code) {
    EXPECT_EQ(msg.body()->result_code, result_code);
  }

  void AssertAssocConfirm(MlmeMsg<wlan_mlme::AssociateConfirm> msg, uint16_t aid,
                          wlan_ieee80211::StatusCode result_code) {
    EXPECT_EQ(msg.body()->association_id, aid);
    EXPECT_EQ(msg.body()->result_code, result_code);
  }

  void AssertAuthFrame(WlanPacket pkt) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<Authentication>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.body()->auth_algorithm_number, AuthAlgorithm::kOpenSystem);
    EXPECT_EQ(frame.body()->auth_txn_seq_number, 1);
    EXPECT_EQ(frame.body()->status_code, 0);
  }

  void AssertDeauthFrame(WlanPacket pkt, wlan_ieee80211::ReasonCode reason_code) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<Deauthentication>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.body()->reason_code, static_cast<uint16_t>(reason_code));
  }

  void AssertAssocReqFrame(WlanPacket pkt, bool rsne) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<AssociationRequest>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    auto assoc_req_frame = frame.NextFrame();
    cpp20::span<const uint8_t> ie_chain{assoc_req_frame.body_data()};
    ASSERT_TRUE(ValidateFrame("invalid assoc request", *pkt.pkt));

    bool has_ssid = false;
    bool has_rsne = false;
    for (auto [id, body] : common::ElementSplitter(ie_chain)) {
      if (id == element_id::kSsid) {
        has_ssid = true;
      } else if (id == element_id::kRsn) {
        has_rsne = true;
        if (rsne) {
          // kRsne contains two bytes for element ID and length; the rest are
          // RSNE bytes
          EXPECT_EQ(std::memcmp(body.data(), kRsne + 2, body.size()), 0);
          EXPECT_EQ(body.size(), sizeof(kRsne) - 2);
        }
      }
    }
    EXPECT_TRUE(has_ssid);
    EXPECT_EQ(has_rsne, rsne);
  }

  void AssertKeepAliveFrame(WlanPacket pkt) {
    auto data_frame = TypeCheckWlanFrame<DataFrameView<>>(pkt.pkt.get());
    EXPECT_EQ(data_frame.hdr()->fc.to_ds(), 1);
    EXPECT_EQ(data_frame.hdr()->fc.from_ds(), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(data_frame.body_len(), static_cast<size_t>(0));
  }

  struct DataFrameAssert {
    unsigned char protected_frame = 0;
    unsigned char more_data = 0;
  };

  void AssertDataFrameSentToAp(WlanPacket pkt, cpp20::span<const uint8_t> expected_payload,
                               DataFrameAssert asserts = {.protected_frame = 0, .more_data = 0}) {
    auto frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame.hdr()->fc.more_data(), asserts.more_data);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.hdr()->fc.protected_frame(), asserts.protected_frame);

    auto llc_frame = frame.NextFrame();
    EXPECT_RANGES_EQ(llc_frame.body_data(), expected_payload);
  }

  MockDevice device;
  ClientMlme client;
};

TEST_F(ClientTest, Join) {
  // (sme->mlme) Send JOIN.request. Verify a JOIN.confirm message was then sent
  // to SME.
  device.sme_->JoinReq(CreateJoinRequest(true));
  auto join_confirm = AssertNextMsgFromSmeChannel<wlan_mlme::JoinConfirm>();
  ASSERT_EQ(join_confirm.body()->result_code, wlan_ieee80211::StatusCode::SUCCESS);
}

TEST_F(ClientTest, Authenticate) {
  Join();
  // (sme->mlme) Send AUTHENTICATION.request. Verify that no confirmation was
  // sent yet.
  device.sme_->AuthenticateReq(CreateAuthRequest());
  client.RunUntilStalled();

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAuthFrame(std::move(*device.wlan_queue.begin()));
  // (ap->mlme) Respond with a Authentication frame. Verify a
  // AUTHENTICATION.confirm message was
  //            then sent to SME
  SendWlanPacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem));
  auto auth_confirm = AssertNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>();
  AssertAuthConfirm(std::move(auth_confirm), wlan_ieee80211::StatusCode::SUCCESS);

  // Verify a delayed timeout won't cause another confirmation.
  IncreaseTimeByBeaconPeriods(100);
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, Associate_Protected) {
  Join();
  Authenticate();

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  device.sme_->AssociateReq(CreateAssocRequest(true));
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>().has_value());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAssocReqFrame(std::move(*device.wlan_queue.begin()), true);

  // (ap->mlme) Respond with a Association Response frame. Verify a
  // ASSOCIATE.confirm message was
  //            then sent to SME.
  SendWlanPacket(CreateAssocRespFrame());
  auto assoc_confirm = AssertNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>();
  AssertAssocConfirm(std::move(assoc_confirm), kAid, wlan_ieee80211::StatusCode::SUCCESS);

  // Verify a delayed timeout won't cause another confirmation.
  IncreaseTimeByBeaconPeriods(100);
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>().has_value());
}

TEST_F(ClientTest, Associate_Unprotected) {
  // (sme->mlme) Send JOIN.request. Verify a JOIN.confirm message was then sent
  // to SME.
  Join(false);

  // (sme->mlme) Send AUTHENTICATION.request. Verify that no confirmation was
  // sent yet.
  device.sme_->AuthenticateReq(CreateAuthRequest());
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>().has_value());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAuthFrame(std::move(*device.wlan_queue.begin()));
  device.wlan_queue.clear();

  // (ap->mlme) Respond with a Authentication frame. Verify a
  // AUTHENTICATION.confirm message was
  //            then sent to SME
  SendWlanPacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem));

  auto auth_conf = AssertNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>();
  AssertAuthConfirm(std::move(auth_conf), wlan_ieee80211::StatusCode::SUCCESS);

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  device.sme_->AssociateReq(CreateAssocRequest(false));
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>().has_value());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAssocReqFrame(std::move(*device.wlan_queue.begin()), false);

  // (ap->mlme) Respond with a Association Response frame and verify a
  // ASSOCIATE.confirm message
  //            was then sent SME.
  SendWlanPacket(CreateAssocRespFrame());
  auto assoc_conf = AssertNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>();
  AssertAssocConfirm(std::move(assoc_conf), kAid, wlan_ieee80211::StatusCode::SUCCESS);
}

TEST_F(ClientTest, ExchangeEapolFrames) {
  Join();
  Authenticate();
  Associate();

  // (sme->mlme) Send EAPOL.request
  device.sme_->EapolReq(
      CreateEapolRequest(common::MacAddr(kClientAddress), common::MacAddr(kBssid1)));
  client.RunUntilStalled();

  // Verify EAPOL frame was sent to AP
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  auto pkt = std::move(*device.wlan_queue.begin());
  auto frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
  EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
  EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
  EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
  EXPECT_EQ(frame.hdr()->fc.protected_frame(), 0);
  EXPECT_EQ(frame.body()->protocol_id_be, htobe16(kEapolProtocolId));
  auto type_checked_frame = frame.SkipHeader().CheckBodyType<EapolHdr>();
  ASSERT_TRUE(type_checked_frame);
  auto llc_eapol_frame = type_checked_frame.CheckLength();
  ASSERT_TRUE(llc_eapol_frame);
  EXPECT_EQ(llc_eapol_frame.body_len(), static_cast<size_t>(5));
  EXPECT_RANGES_EQ(llc_eapol_frame.body_data(), kEapolPdu);
  EXPECT_EQ(pkt.tx_info.tx_flags, WLAN_TX_INFO_FLAGS_FAVOR_RELIABILITY);
  device.wlan_queue.clear();

  // Verify EAPOL.confirm message was sent to SME
  auto msg_data = device.NextTxMlmeMsg();
  ASSERT_TRUE(msg_data.has_value());
  auto eapol_confirm = MlmeMsg<wlan_mlme::EapolConfirm>::Decode(
      msg_data->data(), fuchsia::wlan::mlme::internal::kMLME_EapolConf_Ordinal);
  ASSERT_TRUE(eapol_confirm.has_value());
  EXPECT_EQ(eapol_confirm.value().body()->result_code, wlan_mlme::EapolResultCode::SUCCESS);

  // After controlled port opens, EAPOL frame has protected flag enabled
  EstablishRsna();
  device.sme_->EapolReq(
      CreateEapolRequest(common::MacAddr(kClientAddress), common::MacAddr(kBssid1)));
  client.RunUntilStalled();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  pkt = std::move(*device.wlan_queue.begin());
  frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
  EXPECT_EQ(frame.hdr()->fc.protected_frame(), 1);
}

TEST_F(ClientTest, SetKeys) {
  Join();
  Authenticate();
  Associate();

  // (sme->mlme) Send SETKEYS.request
  auto key_data = std::vector(std::cbegin(kKeyData), std::cend(kKeyData));
  common::MacAddr bssid(kBssid1);
  device.sme_->SetKeysReq(CreateSetKeysRequest(bssid, key_data, wlan_mlme::KeyType::PAIRWISE));
  client.RunUntilStalled();

  ASSERT_EQ(device.GetKeys().size(), static_cast<size_t>(1));
  auto key_config = device.GetKeys()[0];
  EXPECT_EQ(std::memcmp(key_config.key, kKeyData, sizeof(kKeyData)), 0);
  EXPECT_EQ(key_config.key_idx, 1);
  EXPECT_EQ(key_config.key_type, WLAN_KEY_TYPE_PAIRWISE);
  EXPECT_EQ(std::memcmp(key_config.peer_addr, bssid.byte, sizeof(bssid)), 0);
  EXPECT_EQ(std::memcmp(key_config.cipher_oui, kCipherOui, sizeof(kCipherOui)), 0);
  EXPECT_EQ(key_config.cipher_type, kCipherSuiteType);
}

TEST_F(ClientTest, ConstructAssociateContext) {
  Join();
  Authenticate();

  // Send ASSOCIATE.request. Verify that no confirmation was sent yet.
  device.sme_->AssociateReq(CreateAssocRequest(false));
  client.RunUntilStalled();
  // Respond with a Association Response frame and verify a ASSOCIATE.confirm
  // message was sent.
  auto ap_assoc_ctx = wlan::test_utils::FakeDdkAssocCtx();
  ap_assoc_ctx.has_vht_cap = false;
  ap_assoc_ctx.has_vht_op = false;
  SendWlanPacket(CreateAssocRespFrame(ap_assoc_ctx));
  device.sme_->FinalizeAssociationReq(CreateFinalizeAssociationRequest(ap_assoc_ctx, kBssChannel));
  client.RunUntilStalled();
  auto sta_assoc_ctx = device.GetStationAssocContext();

  ASSERT_TRUE(sta_assoc_ctx != nullptr);
  EXPECT_EQ(sta_assoc_ctx->aid, kAid);
  EXPECT_EQ(sta_assoc_ctx->listen_interval, 0);
  EXPECT_EQ(sta_assoc_ctx->channel.primary, 36);
  EXPECT_EQ(sta_assoc_ctx->channel.cbw, CHANNEL_BANDWIDTH_CBW40);
  EXPECT_TRUE(sta_assoc_ctx->has_ht_cap);
  EXPECT_TRUE(sta_assoc_ctx->has_ht_op);
  EXPECT_FALSE(sta_assoc_ctx->has_vht_cap);
  EXPECT_FALSE(sta_assoc_ctx->has_vht_op);
}

TEST_F(ClientTest, AuthTimeout) {
  Join();

  // (sme->mlme) Send AUTHENTICATE.request. Verify that no confirmation was sent
  // yet.
  device.sme_->AuthenticateReq(CreateAuthRequest());
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>().has_value());

  // Timeout not yet hit.
  IncreaseTimeByBeaconPeriods(kAuthTimeout - 1);
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout hit, verify a AUTHENTICATION.confirm message was sent to SME.
  IncreaseTimeByBeaconPeriods(kAuthTimeout);
  auto auth_conf = AssertNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>();
  AssertAuthConfirm(std::move(auth_conf), wlan_ieee80211::StatusCode::REJECTED_SEQUENCE_TIMEOUT);
}

TEST_F(ClientTest, AssocTimeout) {
  Join();
  Authenticate();

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  device.sme_->AssociateReq(CreateAssocRequest(false));
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>().has_value());

  // Timeout not yet hit.
  IncreaseTimeByBeaconPeriods(10);
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout hit, verify a ASSOCIATE.confirm message was sent to SME.
  IncreaseTimeByBeaconPeriods(40);
  auto assoc_conf = AssertNextMsgFromSmeChannel<wlan_mlme::AssociateConfirm>();
  AssertAssocConfirm(std::move(assoc_conf), 0, wlan_ieee80211::StatusCode::REFUSED_TEMPORARILY);
}

TEST_F(ClientTest, ReceiveDataAfterAssociation_Protected) {
  // Verify no data frame can be received before RSNA is established.
  Join();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Associate();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Setting key does not open controlled port
  SetKey();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Establish RSNA and verify data frame can be received
  EstablishRsna();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  auto eth_frames = device.GetEthPackets();
  ASSERT_EQ(eth_frames.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, SendDataAfterAssociation_Protected) {
  // Verify no data frame can be sent before association
  Join();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association but before RSNA is established, data frame is dropped.
  Associate();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(0));

  // Setting key does not open controlled port, so data frame is dropped.
  SetKey();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(0));

  // After RSNA is established, outbound data frames have `protected_frame` flag
  // enabled
  EstablishRsna();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload,
                          {.protected_frame = 1});
}

TEST_F(ClientTest, SendKeepAliveFrameAfterAssociation_Protected) {
  // Verify client doesn't respond to null data frame before association.
  Join();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association, when client receives null data frame, "Keep Alive"
  // response is sent out
  Associate();
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
  device.wlan_queue.clear();

  EstablishRsna();
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
}

TEST_F(ClientTest, ReceiveDataAfterAssociation_Unprotected) {
  // Verify no data frame can be received before association.
  Join(false);
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Associate and verify data frame can be received.
  Associate(false);
  SendWlanPacket(CreateDataFrame(kTestPayload));
  auto eth_frames = device.GetEthPackets();
  ASSERT_EQ(eth_frames.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, SendDataAfterAssociation_Unprotected) {
  // Verify no data frame can be sent before association.
  Join(false);
  QueueEthPacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  QueueEthPacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Associate and verify that data frame can be sent out.
  Associate(false);
  QueueEthPacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload);
}

TEST_F(ClientTest, SendKeepAliveFrameAfterAssociation_Unprotected) {
  // Verify client doesn't respond to null data frame before association.
  Join(false);
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association, when client receives null data frame, "Keep Alive"
  // response is sent out
  Associate(false);
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
}

TEST_F(ClientTest, ProcessEmptyDataFrames) {
  Connect();

  // Send a data frame which carries an LLC frame with no payload.
  // Verify no ethernet frame was queued.
  SendWlanPacket(CreateDataFrame({}));
  ASSERT_TRUE(device.eth_queue.empty());
}

TEST_F(ClientTest, ProcessAmsduDataFrame) {
  const uint8_t payload_data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  cpp20::span<const uint8_t> payload(payload_data);
  std::vector<cpp20::span<const uint8_t>> payloads;
  for (size_t payload_len = 1; payload_len <= 10; ++payload_len) {
    payloads.push_back(payload.subspan(0, payload_len));
  }

  Connect();
  SendWlanPacket(CreateAmsduDataFramePacket(payloads));
  ASSERT_EQ(device.eth_queue.size(), payloads.size());
  for (size_t i = 0; i < payloads.size(); ++i) {
    auto eth_payload = cpp20::span<const uint8_t>(device.eth_queue[i]).subspan(sizeof(EthernetII));
    EXPECT_RANGES_EQ(eth_payload, payloads[i]);
  }
}

TEST_F(ClientTest, DropManagementFrames) {
  Connect();

  // Construct and send deauthentication frame from another BSS.
  constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Deauthentication::max_len();
  auto packet = GetWlanPacket(max_frame_len);
  ASSERT_NE(packet, nullptr);

  BufferWriter w(*packet);
  auto mgmt_hdr = w.Write<MgmtFrameHeader>();
  mgmt_hdr->fc.set_type(FrameType::kManagement);
  mgmt_hdr->fc.set_subtype(ManagementSubtype::kDeauthentication);
  mgmt_hdr->addr1 = common::MacAddr(kBssid2);
  mgmt_hdr->addr2 = common::MacAddr(kClientAddress);
  mgmt_hdr->addr3 = common::MacAddr(kBssid2);
  w.Write<Deauthentication>()->reason_code = 42;
  SendWlanPacket(std::move(packet));

  // Verify neither a management frame nor service message were sent.
  ASSERT_TRUE(device.svc_queue.empty());
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.eth_queue.empty());

  // Verify data frames can still be send and the clientis presumably
  // associated.
  SendWlanPacket(CreateDataFrame(kTestPayload));
  ASSERT_EQ(device.eth_queue.size(), static_cast<size_t>(1));
}

TEST_F(ClientTest, AutoDeauth_NoBeaconReceived) {
  Connect();

  // Timeout not yet hit.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout);
  ASSERT_TRUE(device.wlan_queue.empty());

  // Auto-deauth timeout, client should be deauthenticated.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

TEST_F(ClientTest, AutoDeauth_NoBeaconsShortlyAfterConnecting) {
  Connect();

  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  SendBeaconFrame();

  // Not enough time has passed yet since beacon frame was sent, so no deauth.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_TRUE(device.wlan_queue.empty());

  // One timeout away from auto-deauth
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout -
                                                  kAssociationStatusBeaconCount);
  ASSERT_TRUE(device.wlan_queue.empty());

  // Auto-deauth triggers now.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

// Generally comment of auto-deauth tests below that combine with switching channel:
// - These tests make sure that the combined on-channel times without receiving beacon
//   is a full auto-deauth interval.
// - How long the off-channel times are do not matter, but we set them long enough to
//   be confident that off-channel times do not contribute to auto-deauth.
TEST_F(ClientTest, AutoDeauth_DoNotDeauthWhileSwitchingChannel) {
  Connect();

  // Very close to getting auto deauthenticated.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout);
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `AssertGoingOnChannel` call.
  GoOffChannel(2 * kAutoDeauthTimeout + kAssociationStatusBeaconCount);

  // For next two timeouts, still off channel, so should not deauth.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_TRUE(device.wlan_queue.empty());

  // Any timeout fired when off-channel does not count against auto-deauth
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout -
                                                  kAssociationStatusBeaconCount);
  ASSERT_TRUE(device.wlan_queue.empty());

  // Ensure enough time has passed so that we can go back to main channel
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout + kAssociationStatusBeaconCount);
  AssertGoingOnChannel();

  // Before going off channel, we did not receive beacon for `kAutoDeauthTimeout` periods. Now
  // one more association status check interval has passed after going back on channel, so should
  // auto deauth.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

TEST_F(ClientTest, AutoDeauth_InterleavingBeaconsAndChannelSwitches) {
  Connect();

  // Before going off channel, advance to the point of almost auto deauthenticating
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout -
                                                  5 * kAssociationStatusBeaconCount);
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `AssertGoingOnChannel` call.
  GoOffChannel(6 * kAssociationStatusBeaconCount);

  // No deauth since off channel.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(5 * kAssociationStatusBeaconCount);
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(kAssociationStatusBeaconCount);
  AssertGoingOnChannel();

  // Got beacon frame, which should reset the timeout.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(
      3 * kAssociationStatusBeaconCount);  // -- On-channel time without beacon  -- //
  SendBeaconFrame();                       // -- Beacon timeout refresh -- ///

  // No deauth since beacon was received not too long ago.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(
      2 * kAssociationStatusBeaconCount);  // -- On-channel time without beacon  -- //
  ASSERT_TRUE(device.wlan_queue.empty());

  // Going off channel and back on channel
  // Total on-channel time without beacons so far: 2 signal report intervals
  GoOffChannel(kAutoDeauthTimeout);
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  AssertGoingOnChannel();

  AdvanceAutoDeauthenticationTimerByBeaconPeriods(
      kAutoDeauthTimeout -
      2 * kAssociationStatusBeaconCount);  // -- On-channel time without beacon -- //
  ASSERT_TRUE(device.wlan_queue.empty());

  // Going off channel and back on channel again
  // Total on-channel time without beacons so far:
  // 2 * kAssociationStatusBeaconCount + kAutoDeauthTimeout - 2 *
  // kAssociationStatusBeaconCount
  GoOffChannel(kAutoDeauthTimeout);
  // Not using AdvanceAutoDeauthenticationTimerByBeaconPeriods because TiggerTimeout() will switch
  // the client back on to main channel.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  AssertGoingOnChannel();
  ASSERT_TRUE(device.wlan_queue.empty());

  // One more signal report beacon period and auto-deauth triggers
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(
      kAssociationStatusBeaconCount);  // -- On-channel time without beacon -- //
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

// This test explores what happens if the whole auto-deauth timeout duration is
// exhausted, but the client switches channel before auto-deauth can trigger.
// For the current implementation where we cancel timer when going off channel
// and reschedule when going back on channel, this test is intended to be a
// safeguard against making the mistake of scheduling or exactly in the present
// when going back on channel.
TEST_F(ClientTest, AutoDeauth_SwitchingChannelBeforeDeauthTimeoutCouldTrigger) {
  Connect();

  // No deauth since off channel.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout);
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `AssertGoingOnChannel` call.
  GoOffChannel(1);
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(1);
  AssertGoingOnChannel();

  // Auto-deauth timeout shouldn't trigger yet. This is because after going back
  // on channel, the client should always schedule timeout sufficiently far
  // enough in the future (at least one beacon interval)
  IncreaseTimeByBeaconPeriods(1);
  ASSERT_TRUE(device.wlan_queue.empty());

  // Auto-deauth now
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

TEST_F(ClientTest, AutoDeauth_ForeignBeaconShouldNotPreventDeauth) {
  Connect();

  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAutoDeauthTimeout);
  SendBeaconFrame(common::MacAddr(kBssid2));  // beacon frame from another AP

  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_ieee80211::ReasonCode::LEAVING_NETWORK_DEAUTH);
  AssertNextMsgFromSmeChannel<wlan_mlme::DeauthenticateIndication>();
}

TEST_F(ClientTest, DropFramesWhileOffChannel) {
  Connect();

  // Advance time to ensure we're on-channel.
  AdvanceAutoDeauthenticationTimerByBeaconPeriods(kAssociationStatusBeaconCount);
  SendBeaconFrame();

  GoOffChannel(1);
  QueueEthPacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(1);
  AssertGoingOnChannel();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(0));
}

TEST_F(ClientTest, InvalidAuthenticationResponse) {
  Join();

  // Send AUTHENTICATION.request. Verify that no confirmation was sent yet.
  device.sme_->AuthenticateReq(CreateAuthRequest());
  // Potential false negative if the message arrives after 10ms. Good enough for sanity check.
  ASSERT_FALSE(GetNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>().has_value());

  // Send authentication frame with wrong algorithm.
  SendWlanPacket(CreateAuthRespFrame(AuthAlgorithm::kSae));

  // Verify that AUTHENTICATION.confirm was received.
  auto auth_conf = AssertNextMsgFromSmeChannel<wlan_mlme::AuthenticateConfirm>();
  AssertAuthConfirm(std::move(auth_conf), wlan_ieee80211::StatusCode::REFUSED_REASON_UNSPECIFIED);

  // Fast forward in time would have caused a timeout.
  // The timeout however should have been canceled and we should not receive
  // and additional confirmation.
  IncreaseTimeByBeaconPeriods(kAuthTimeout);
  ASSERT_TRUE(device.svc_queue.empty());

  // Send a second, now valid authentication frame.
  // This frame should be ignored as the client reset.
  SendWlanPacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem));

  // Fast forward in time far beyond an authentication timeout.
  // There should not be any AUTHENTICATION.confirm sent as the client
  // is expected to have been reset into |idle| state after failing
  // to authenticate.
  IncreaseTimeByBeaconPeriods(1000);
  ASSERT_TRUE(device.svc_queue.empty());
}

// TODO(fxbug.dev/43456): Enable this test again once MLME stats is implemented in Rust.
TEST_F(ClientTest, DISABLED_ProcessZeroRssiFrame) {
  auto no_rssi_pkt = CreateDataFrame(kTestPayload);
  auto rx_info = const_cast<wlan_rx_info_t*>(no_rssi_pkt->ctrl_data<wlan_rx_info_t>());
  rx_info->valid_fields &= ~WLAN_RX_INFO_VALID_DATA_RATE;  // no rssi
  rx_info->rssi_dbm = 0;

  auto rssi_pkt = CreateDataFrame(kTestPayload);
  rx_info = const_cast<wlan_rx_info_t*>(rssi_pkt->ctrl_data<wlan_rx_info_t>());
  rx_info->valid_fields |= WLAN_RX_INFO_VALID_DATA_RATE;
  rx_info->rssi_dbm = 0;

  Connect();

  ASSERT_GT(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist.size(), 0u);
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 0u);

  // Send a data frame with no rssi and verify that we don't increment stats.
  SendWlanPacket(std::move(no_rssi_pkt));
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 0u);

  // Send a data frame with 0 rssi and verify that we *do* increment stats.
  SendWlanPacket(std::move(rssi_pkt));
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 1u);
}

TEST_F(ClientTest, PsPollWithMoreData) {
  Connect();

  auto more_data_pkt = CreateDataFrame(kTestPayload);
  more_data_pkt->mut_field<DataFrameHeader>(0)->fc.set_more_data(true);
  more_data_pkt->mut_field<DataFrameHeader>(0)->addr1 = common::MacAddr(kClientAddress);

  SendWlanPacket(std::move(more_data_pkt));

  ASSERT_EQ(device.wlan_queue.size(), 1ULL);
  auto frame = TypeCheckWlanFrame<CtrlFrameView<PsPollFrame>>(device.wlan_queue[0].pkt.get());
  ASSERT_TRUE(frame);

  EXPECT_EQ(ControlSubtype::kPsPoll, frame.hdr()->fc.subtype());
  EXPECT_EQ(kAid | 0xc000, frame.body()->aid);
  EXPECT_EQ(common::MacAddr(kBssid1), frame.body()->bssid);
  EXPECT_EQ(common::MacAddr(kClientAddress), frame.body()->ta);
}

TEST_F(ClientTest, PsPollWithBeacon) {
  Connect();

  constexpr size_t reserved_ie_len = 256;
  constexpr size_t max_frame_len =
      MgmtFrameHeader::max_len() + ProbeRequest::max_len() + reserved_ie_len;
  auto beacon_pkt = GetWlanPacket(max_frame_len);

  BufferWriter w(*beacon_pkt);
  w.Write(*CreateBeaconFrame(common::MacAddr(kBssid1)));

  TrafficIndicationMap tim;
  tim.SetTrafficIndication(kAid, true);

  size_t bitmap_len;
  uint8_t bitmap_offset;
  uint8_t pvb[kMaxTimBitmapLen];
  EXPECT_EQ(ZX_OK, tim.WritePartialVirtualBitmap(pvb, sizeof(pvb), &bitmap_len, &bitmap_offset));

  TimHeader header;
  header.dtim_count = 0;
  header.dtim_period = 0;
  header.bmp_ctrl.set_offset(bitmap_offset);

  common::WriteTim(&w, header, {pvb, bitmap_len});

  beacon_pkt->set_len(w.WrittenBytes());

  // rx_info is out of band, so we have to copy it back into beacon_pkt here.
  wlan_rx_info_t rx_info{.rx_flags = 0};
  beacon_pkt->CopyCtrlFrom(rx_info);

  SendWlanPacket(std::move(beacon_pkt));

  ASSERT_EQ(device.wlan_queue.size(), 1ULL);
  auto frame = TypeCheckWlanFrame<CtrlFrameView<PsPollFrame>>(device.wlan_queue[0].pkt.get());
  ASSERT_TRUE(frame);

  EXPECT_EQ(ControlSubtype::kPsPoll, frame.hdr()->fc.subtype());
  EXPECT_EQ(kAid | 0xc000, frame.body()->aid);
  EXPECT_EQ(common::MacAddr(kBssid1), frame.body()->bssid);
  EXPECT_EQ(common::MacAddr(kClientAddress), frame.body()->ta);
}

// Add additional tests for (tracked in fxbug.dev/29020):
// AP refuses Authentication/Association
// Regression tests for:
// - fxbug.dev/28988: PS-POLL after TIM indication.
// Deauthenticate in any state issued by AP/SME.
// Disassociation in any state issued by AP/SME.
// Handle Action frames and setup Block-Ack session.
// Drop data frames from unknown BSS.
// Connect to a:
// - HT/VHT capable network
// - 5GHz network
// - different network than currently associated to
// Notify driver about association
// Ensure Deauthentication Indicaiton and notification is sent whenever
// deauthenticating. Enter/Leave power management when going off/on channel.
// Verify timeouts don't hit after resetting the station.

}  // namespace
}  // namespace wlan
