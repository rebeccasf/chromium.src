// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/network/network_state.h"

#include <stdint.h>

#include <memory>
#include <utility>

#include "base/i18n/streaming_utf8_validator.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "chromeos/network/network_state_handler.h"
#include "chromeos/network/network_state_test_helper.h"
#include "chromeos/network/tether_constants.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/cros_system_api/dbus/service_constants.h"
#include "third_party/cros_system_api/dbus/shill/dbus-constants.h"

namespace chromeos {

namespace {

const char kTestCellularDevicePath[] = "cellular_path";
const char kTestCellularDeviceName[] = "cellular_name";

class NetworkStateTest : public testing::Test {
 public:
  NetworkStateTest() : network_state_("test_path") {}

  NetworkStateTest(const NetworkStateTest&) = delete;
  NetworkStateTest& operator=(const NetworkStateTest&) = delete;

  // testing::Test:
  void SetUp() override { AddCellularDevice(); }

 protected:
  const DeviceState* GetCellularDevice() {
    return helper_.network_state_handler()->GetDeviceState(
        kTestCellularDevicePath);
  }

  bool SetProperty(const std::string& key, base::Value value) {
    const bool result = network_state_.PropertyChanged(key, value);
    properties_.SetKey(key, std::move(value));
    return result;
  }

  bool SetStringProperty(const std::string& key, const std::string& value) {
    return SetProperty(key, base::Value(value));
  }

  bool SignalInitialPropertiesReceived() {
    return network_state_.InitialPropertiesReceived(properties_);
  }

  NetworkState network_state_;

 private:
  void AddCellularDevice() {
    helper_.device_test()->AddDevice(
        kTestCellularDevicePath, shill::kTypeCellular, kTestCellularDeviceName);
    base::RunLoop().RunUntilIdle();
  }

  base::test::SingleThreadTaskEnvironment task_environment_;
  NetworkStateTestHelper helper_{/*use_default_devices_and_services=*/false};

  base::Value properties_{base::Value::Type::DICTIONARY};
};

}  // namespace

// Setting kNameProperty should set network name after call to
// InitialPropertiesReceived().
TEST_F(NetworkStateTest, NameAscii) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeVPN));

  std::string network_setname = "Name TEST";
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, network_setname));
  EXPECT_FALSE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.name(), network_setname);
}

TEST_F(NetworkStateTest, NameAsciiWithNull) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeVPN));

  std::string network_setname = "Name TEST\x00xxx";
  std::string network_setname_result = "Name TEST";
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, network_setname));
  EXPECT_FALSE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.name(), network_setname_result);
}

// Truncates invalid UTF-8. base::Value has a DCHECK against invalid UTF-8
// strings, which is why this is a release mode only test.
#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
TEST_F(NetworkStateTest, NameTruncateInvalid) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeVPN));

  std::string network_setname = "SSID TEST \x01\xff!";
  std::string network_setname_result = "SSID TEST \xEF\xBF\xBD\xEF\xBF\xBD!";
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, network_setname));
  EXPECT_TRUE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.name(), network_setname_result);
}
#endif

// If HexSSID doesn't exist, fallback to NameProperty.
TEST_F(NetworkStateTest, SsidFromName) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));

  std::string wifi_utf8 = "UTF-8 \u3042\u3044\u3046";
  std::string wifi_utf8_result = "UTF-8 \xE3\x81\x82\xE3\x81\x84\xE3\x81\x86";
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, wifi_utf8));
  EXPECT_FALSE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.name(), wifi_utf8_result);
}

// latin1 SSID -> UTF8 SSID (Hex)
TEST_F(NetworkStateTest, SsidLatin) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));

  std::string wifi_latin1 = "latin-1 \x54\xe9\x6c\xe9\x63\x6f\x6d";  // Télécom
  std::string wifi_latin1_hex =
      base::HexEncode(wifi_latin1.c_str(), wifi_latin1.length());
  std::string wifi_latin1_result = "latin-1 T\xc3\xa9\x6c\xc3\xa9\x63om";
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, wifi_latin1_hex));
  EXPECT_TRUE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.name(), wifi_latin1_result);
}

// Hex SSID
TEST_F(NetworkStateTest, SsidHex) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));

  std::string wifi_hex_result = "This is HEX SSID!";
  std::string wifi_hex =
      base::HexEncode(wifi_hex_result.c_str(), wifi_hex_result.length());
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, wifi_hex));
  EXPECT_TRUE(SignalInitialPropertiesReceived());
  EXPECT_EQ(wifi_hex_result, network_state_.name());

  // Check HexSSID via network state dictionary.
  base::Value dictionary(base::Value::Type::DICTIONARY);
  network_state_.GetStateProperties(&dictionary);
  std::string* value = dictionary.FindStringKey(shill::kWifiHexSsid);
  EXPECT_NE(nullptr, value);
  EXPECT_EQ(wifi_hex, *value);
}

// Non-UTF-8 SSID should be preserved in |raw_ssid_| field.
TEST_F(NetworkStateTest, SsidNonUtf8) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));

  std::string non_utf8_ssid = "\xc0";
  ASSERT_FALSE(base::StreamingUtf8Validator::Validate(non_utf8_ssid));

  std::vector<uint8_t> non_utf8_ssid_bytes;
  non_utf8_ssid_bytes.push_back(static_cast<uint8_t>(non_utf8_ssid.data()[0]));

  std::string wifi_hex =
      base::HexEncode(non_utf8_ssid.data(), non_utf8_ssid.size());
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, wifi_hex));
  EXPECT_TRUE(SignalInitialPropertiesReceived());
  EXPECT_EQ(network_state_.raw_ssid(), non_utf8_ssid_bytes);
}

// Multiple updates for Hex SSID should work fine.
TEST_F(NetworkStateTest, SsidHexMultipleUpdates) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));

  std::string wifi_hex_result = "This is HEX SSID!";
  std::string wifi_hex =
      base::HexEncode(wifi_hex_result.c_str(), wifi_hex_result.length());
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, wifi_hex));
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, wifi_hex));
}

TEST_F(NetworkStateTest, CaptivePortalState) {
  std::string network_name = "test";
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeWifi));
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, network_name));
  std::string hex_ssid =
      base::HexEncode(network_name.c_str(), network_name.length());
  EXPECT_TRUE(SetStringProperty(shill::kWifiHexSsid, hex_ssid));

  // State != portal -> portal_state() == kOnline
  EXPECT_TRUE(SetStringProperty(shill::kStateProperty, shill::kStateReady));
  SignalInitialPropertiesReceived();
  EXPECT_EQ(network_state_.portal_state(), NetworkState::PortalState::kOnline);

  // State == redirect-found -> portal_state() == kPortal
  EXPECT_TRUE(
      SetStringProperty(shill::kStateProperty, shill::kStateRedirectFound));
  SignalInitialPropertiesReceived();
  EXPECT_EQ(network_state_.portal_state(), NetworkState::PortalState::kPortal);

  // State == portal-suspected -> portal_state() == kPortalSuspected
  EXPECT_TRUE(
      SetStringProperty(shill::kStateProperty, shill::kStatePortalSuspected));
  SignalInitialPropertiesReceived();
  EXPECT_EQ(network_state_.portal_state(),
            NetworkState::PortalState::kPortalSuspected);

  // State == no-connectivity -> portal_state() == kOffline
  EXPECT_TRUE(
      SetStringProperty(shill::kStateProperty, shill::kStateNoConnectivity));
  SignalInitialPropertiesReceived();
  EXPECT_EQ(network_state_.portal_state(),
            NetworkState::PortalState::kNoInternet);
}

// Third-party VPN provider.
TEST_F(NetworkStateTest, VPNThirdPartyProvider) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeVPN));
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, "VPN"));

  base::Value provider(base::Value::Type::DICTIONARY);
  provider.SetKey(shill::kTypeProperty,
                  base::Value(shill::kProviderThirdPartyVpn));
  provider.SetKey(shill::kHostProperty,
                  base::Value("third-party-vpn-provider-extension-id"));
  EXPECT_TRUE(SetProperty(shill::kProviderProperty, std::move(provider)));
  SignalInitialPropertiesReceived();
  ASSERT_TRUE(network_state_.vpn_provider());
  EXPECT_EQ(network_state_.vpn_provider()->type, shill::kProviderThirdPartyVpn);
  EXPECT_EQ(network_state_.vpn_provider()->id,
            "third-party-vpn-provider-extension-id");
}

// Arc VPN provider.
TEST_F(NetworkStateTest, VPNArcProvider) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeVPN));
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, "VPN"));

  base::Value provider(base::Value::Type::DICTIONARY);
  provider.SetKey(shill::kTypeProperty, base::Value(shill::kProviderArcVpn));
  provider.SetKey(shill::kHostProperty, base::Value("package.name.foo"));
  EXPECT_TRUE(SetProperty(shill::kProviderProperty, std::move(provider)));
  SignalInitialPropertiesReceived();
  ASSERT_TRUE(network_state_.vpn_provider());
  EXPECT_EQ(network_state_.vpn_provider()->type, shill::kProviderArcVpn);
  EXPECT_EQ(network_state_.vpn_provider()->id, "package.name.foo");
}

TEST_F(NetworkStateTest, AllowRoaming) {
  EXPECT_FALSE(network_state_.allow_roaming());
  EXPECT_TRUE(
      SetProperty(shill::kCellularAllowRoamingProperty, base::Value(true)));
  EXPECT_TRUE(network_state_.allow_roaming());
}

TEST_F(NetworkStateTest, Visible) {
  EXPECT_FALSE(network_state_.visible());

  network_state_.set_visible(true);
  EXPECT_TRUE(network_state_.visible());

  network_state_.set_visible(false);
  EXPECT_FALSE(network_state_.visible());
}

TEST_F(NetworkStateTest, ConnectionState) {
  network_state_.set_visible(true);

  network_state_.SetConnectionState(shill::kStateConfiguration);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateConfiguration);
  EXPECT_TRUE(network_state_.IsConnectingState());
  EXPECT_TRUE(network_state_.IsConnectingOrConnected());
  EXPECT_TRUE(network_state_.IsActive());
  // State change to configuration from idle should not set connect_requested
  // unless explicitly set by the UI.
  EXPECT_FALSE(network_state_.connect_requested());

  network_state_.SetConnectionState(shill::kStateOnline);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateOnline);
  EXPECT_TRUE(network_state_.IsConnectedState());
  EXPECT_TRUE(network_state_.IsConnectingOrConnected());
  EXPECT_TRUE(network_state_.IsActive());

  network_state_.SetConnectionState(shill::kStateConfiguration);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateConfiguration);
  EXPECT_TRUE(network_state_.IsConnectingState());
  // State change to configuration from a connected state should set
  // connect_requested.
  EXPECT_TRUE(network_state_.connect_requested());

  network_state_.SetConnectionState(shill::kStateOnline);
  EXPECT_TRUE(network_state_.IsConnectedState());
  // State change to connected should clear connect_requested.
  EXPECT_FALSE(network_state_.connect_requested());

  network_state_.SetConnectionState(shill::kStateIdle);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateIdle);
  EXPECT_FALSE(network_state_.IsConnectedState());
  EXPECT_FALSE(network_state_.IsConnectingState());
  EXPECT_FALSE(network_state_.IsConnectingOrConnected());
  EXPECT_FALSE(network_state_.IsActive());

  EXPECT_TRUE(SetStringProperty(shill::kActivationStateProperty,
                                shill::kActivationStateActivating));
  EXPECT_FALSE(network_state_.IsConnectedState());
  EXPECT_FALSE(network_state_.IsConnectingState());
  EXPECT_FALSE(network_state_.IsConnectingOrConnected());
  EXPECT_TRUE(network_state_.IsActive());
}

TEST_F(NetworkStateTest, ConnectRequested) {
  network_state_.set_visible(true);

  network_state_.SetConnectionState(shill::kStateIdle);

  network_state_.set_connect_requested_for_testing(true);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateIdle);
  EXPECT_FALSE(network_state_.IsConnectedState());
  EXPECT_TRUE(network_state_.IsConnectingState());
  EXPECT_TRUE(network_state_.IsConnectingOrConnected());

  network_state_.SetConnectionState(shill::kStateOnline);
  EXPECT_TRUE(network_state_.IsConnectedState());
  EXPECT_FALSE(network_state_.IsConnectingState());
}

TEST_F(NetworkStateTest, ConnectionStateNotVisible) {
  network_state_.set_visible(false);

  network_state_.SetConnectionState(shill::kStateConfiguration);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateIdle);
  EXPECT_FALSE(network_state_.IsConnectingState());

  network_state_.SetConnectionState(shill::kStateOnline);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateIdle);
  EXPECT_FALSE(network_state_.IsConnectedState());

  network_state_.SetConnectionState(shill::kStateConfiguration);
  EXPECT_EQ(network_state_.connection_state(), shill::kStateIdle);
  EXPECT_FALSE(network_state_.IsConnectingState());
}

TEST_F(NetworkStateTest, TetherProperties) {
  network_state_.set_type_for_testing(kTypeTether);
  network_state_.set_tether_carrier("Project Fi");
  network_state_.set_battery_percentage(85);
  network_state_.set_tether_has_connected_to_host(true);
  network_state_.set_signal_strength(75);

  base::Value dictionary(base::Value::Type::DICTIONARY);
  network_state_.GetStateProperties(&dictionary);

  absl::optional<int> signal_strength =
      dictionary.FindIntKey(kTetherSignalStrength);
  EXPECT_TRUE(signal_strength.has_value());
  EXPECT_EQ(75, signal_strength.value());

  absl::optional<int> battery_percentage =
      dictionary.FindIntKey(kTetherBatteryPercentage);
  EXPECT_TRUE(battery_percentage.has_value());
  EXPECT_EQ(85, battery_percentage.value());

  absl::optional<bool> tether_has_connected_to_host =
      dictionary.FindBoolKey(kTetherHasConnectedToHost);
  EXPECT_TRUE(tether_has_connected_to_host.has_value());
  EXPECT_TRUE(tether_has_connected_to_host.value());

  std::string* carrier = dictionary.FindStringKey(kTetherCarrier);
  EXPECT_NE(nullptr, carrier);
  EXPECT_EQ("Project Fi", *carrier);
}

TEST_F(NetworkStateTest, CelularPaymentPortalPost) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeCellular));
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, "Test Cellular"));
  EXPECT_TRUE(SetStringProperty(shill::kNetworkTechnologyProperty,
                                shill::kNetworkTechnologyLteAdvanced));
  EXPECT_TRUE(SetStringProperty(shill::kActivationTypeProperty,
                                shill::kActivationTypeOTA));
  EXPECT_TRUE(SetStringProperty(shill::kActivationStateProperty,
                                shill::kActivationStateActivated));

  base::Value payment_portal(base::Value::Type::DICTIONARY);
  payment_portal.SetKey(shill::kPaymentPortalURL,
                        base::Value("http://test-portal.com"));
  payment_portal.SetKey(shill::kPaymentPortalMethod, base::Value("POST"));
  payment_portal.SetKey(shill::kPaymentPortalPostData,
                        base::Value("fake_data"));

  EXPECT_TRUE(
      SetProperty(shill::kPaymentPortalProperty, std::move(payment_portal)));

  SignalInitialPropertiesReceived();
  EXPECT_EQ("Test Cellular", network_state_.name());
  EXPECT_EQ(shill::kNetworkTechnologyLteAdvanced,
            network_state_.network_technology());
  EXPECT_EQ(shill::kActivationTypeOTA, network_state_.activation_type());
  EXPECT_EQ(shill::kActivationStateActivated,
            network_state_.activation_state());
  EXPECT_EQ("http://test-portal.com", network_state_.payment_url());
  EXPECT_EQ("fake_data", network_state_.payment_post_data());
}

TEST_F(NetworkStateTest, CelularPaymentPortalGet) {
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeCellular));
  EXPECT_TRUE(SetStringProperty(shill::kNameProperty, "Test Cellular"));
  EXPECT_TRUE(SetStringProperty(shill::kNetworkTechnologyProperty,
                                shill::kNetworkTechnologyLteAdvanced));
  EXPECT_TRUE(SetStringProperty(shill::kActivationTypeProperty,
                                shill::kActivationTypeOTA));
  EXPECT_TRUE(SetStringProperty(shill::kActivationStateProperty,
                                shill::kActivationStateActivated));

  base::Value payment_portal(base::Value::Type::DICTIONARY);
  payment_portal.SetKey(shill::kPaymentPortalURL,
                        base::Value("http://test-portal.com"));
  payment_portal.SetKey(shill::kPaymentPortalMethod, base::Value("GET"));
  payment_portal.SetKey(shill::kPaymentPortalPostData, base::Value("ignored"));

  EXPECT_TRUE(
      SetProperty(shill::kPaymentPortalProperty, std::move(payment_portal)));

  SignalInitialPropertiesReceived();

  EXPECT_EQ("Test Cellular", network_state_.name());
  EXPECT_EQ(shill::kNetworkTechnologyLteAdvanced,
            network_state_.network_technology());
  EXPECT_EQ(shill::kActivationTypeOTA, network_state_.activation_type());
  EXPECT_EQ(shill::kActivationStateActivated,
            network_state_.activation_state());
  EXPECT_EQ("http://test-portal.com", network_state_.payment_url());
  EXPECT_EQ("", network_state_.payment_post_data());
}

TEST_F(NetworkStateTest, CellularSpecifier) {
  const char kTestCellularNetworkName[] = "cellular1";
  const char kTestIccid1[] = "1234567890";
  const char kTestIccid2[] = "0987654321";
  EXPECT_TRUE(SetStringProperty(shill::kTypeProperty, shill::kTypeCellular));
  EXPECT_TRUE(
      SetStringProperty(shill::kNameProperty, kTestCellularNetworkName));
  network_state_.set_update_received();

  // Verify that cellular network state with same name but different iccid
  // produce different specifier values.
  EXPECT_TRUE(SetStringProperty(shill::kIccidProperty, kTestIccid1));
  std::string specifier1 = network_state_.GetSpecifier();
  EXPECT_TRUE(SetStringProperty(shill::kIccidProperty, kTestIccid2));
  std::string specifier2 = network_state_.GetSpecifier();
  EXPECT_NE(specifier1, specifier2);
}

TEST_F(NetworkStateTest, NonShillCellular) {
  const char kTestIccid[] = "test_iccid";
  const char kTestEid[] = "test_eid";
  const char kTestGuid[] = "test_guid";

  std::unique_ptr<NetworkState> non_shill_cellular =
      NetworkState::CreateNonShillCellularNetwork(
          kTestIccid, kTestEid, kTestGuid, /*is_managed=*/false,
          GetCellularDevice());
  EXPECT_EQ(kTestIccid, non_shill_cellular->iccid());
  EXPECT_EQ(kTestEid, non_shill_cellular->eid());
  EXPECT_EQ(kTestGuid, non_shill_cellular->guid());
  EXPECT_FALSE(non_shill_cellular->IsManagedByPolicy());

  base::Value dictionary(base::Value::Type::DICTIONARY);
  non_shill_cellular->GetStateProperties(&dictionary);
  EXPECT_EQ(kTestIccid, *dictionary.FindStringKey(shill::kIccidProperty));
  EXPECT_EQ(kTestEid, *dictionary.FindStringKey(shill::kEidProperty));
  EXPECT_EQ(kTestGuid, *dictionary.FindStringKey(shill::kGuidProperty));

  non_shill_cellular = NetworkState::CreateNonShillCellularNetwork(
      kTestIccid, kTestEid, kTestGuid, /*is_managed=*/true,
      GetCellularDevice());
  EXPECT_EQ(kTestIccid, non_shill_cellular->iccid());
  EXPECT_EQ(kTestEid, non_shill_cellular->eid());
  EXPECT_EQ(kTestGuid, non_shill_cellular->guid());
  EXPECT_TRUE(non_shill_cellular->IsManagedByPolicy());

  non_shill_cellular->GetStateProperties(&dictionary);
  EXPECT_EQ(kTestIccid, *dictionary.FindStringKey(shill::kIccidProperty));
  EXPECT_EQ(kTestEid, *dictionary.FindStringKey(shill::kEidProperty));
  EXPECT_EQ(kTestGuid, *dictionary.FindStringKey(shill::kGuidProperty));
}

}  // namespace chromeos
