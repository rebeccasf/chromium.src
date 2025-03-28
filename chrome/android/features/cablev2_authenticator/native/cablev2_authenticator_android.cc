// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/timer/elapsed_timer.h"
#include "components/cbor/reader.h"
#include "components/cbor/values.h"
#include "components/cbor/writer.h"
#include "components/device_event_log/device_event_log.h"
#include "crypto/random.h"
#include "device/fido/cable/v2_authenticator.h"
#include "device/fido/cable/v2_handshake.h"
#include "device/fido/cable/v2_registration.h"
#include "device/fido/features.h"
#include "device/fido/fido_parsing_utils.h"
#include "third_party/blink/public/mojom/webauthn/authenticator.mojom.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/obj.h"

// These "headers" actually contain several function definitions and thus can
// only be included once across Chromium.
#include "base/time/time.h"
#include "chrome/android/features/cablev2_authenticator/jni_headers/BLEAdvert_jni.h"
#include "chrome/android/features/cablev2_authenticator/jni_headers/CableAuthenticator_jni.h"
#include "chrome/android/features/cablev2_authenticator/jni_headers/USBHandler_jni.h"

using base::android::ConvertJavaStringToUTF8;
using base::android::ConvertUTF8ToJavaString;
using base::android::JavaByteArrayToByteVector;
using base::android::JavaParamRef;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;
using base::android::ToJavaArrayOfByteArray;
using base::android::ToJavaByteArray;
using base::android::ToJavaIntArray;

namespace {

namespace protobuf {

// WireType enumerates the protobuf wire types. See
// https://developers.google.com/protocol-buffers/docs/encoding#structure
enum class WireType {
  kVarint = 0,
  kLengthPrefixed = 2,
};

// EncodeTag encodes a protobuf tag and type into the identifier value used on
// the wire. See
// https://developers.google.com/protocol-buffers/docs/encoding#structure
constexpr uint64_t EncodeTag(uint64_t tag_number, WireType type) {
  return tag_number << 3 | static_cast<uint8_t>(type);
}

// Some tag numbers used in a protobuf that cannot be included here but which
// we wish to generate messages for.
constexpr uint64_t kTagRequestId = 1;
constexpr uint64_t kTagEventType = 2;
constexpr uint64_t kTagTunnelId = 11;
constexpr uint64_t kTagChromeLog = 501;
constexpr uint64_t kTagEvent = 1;
constexpr uint64_t kTagResult = 2;

// cbb_add_varint encodes |v| as a base128 varint as described at
// https://developers.google.com/protocol-buffers/docs/encoding#varints.
bool cbb_add_varint(CBB* cbb, uint64_t v) {
  for (;;) {
    const uint64_t next_v = v >> 7;
    const bool is_last_byte = (next_v == 0);
    const uint8_t b = (v & 0x7f) | (is_last_byte ? 0 : 0x80);

    if (!CBB_add_u8(cbb, b)) {
      return false;
    }

    if (is_last_byte) {
      return true;
    }
    v = next_v;
  }
}

enum class Type {
  kEvent,
  kResult,
};

// LogEvent logs an event on a server-linked transaction with the given
// |tunnel_id|. The semantics of |value| are specific to the |type| of the
// logged event.
void LogEvent(
    JNIEnv* env,
    base::span<const uint8_t, device::cablev2::kTunnelIdSize> tunnel_id,
    Type type,
    unsigned value) {
  uint64_t tag;
  switch (type) {
    case Type::kEvent:
      tag = kTagEvent;
      break;
    case Type::kResult:
      tag = kTagResult;
      break;
  }

  // An inner protobuf is serialised first in order to get its length for the
  // outer message.
  bssl::ScopedCBB inner_cbb;
  uint8_t* inner_bytes;
  size_t inner_length;
  if (!CBB_init(inner_cbb.get(), 16) ||
      !cbb_add_varint(inner_cbb.get(), EncodeTag(tag, WireType::kVarint)) ||
      !cbb_add_varint(inner_cbb.get(), value) ||
      !CBB_finish(inner_cbb.get(), &inner_bytes, &inner_length)) {
    return;
  }
  bssl::UniquePtr<uint8_t> inner_bytes_storage(inner_bytes);

  bssl::ScopedCBB cbb;
  uint8_t* bytes;
  size_t length;
  if (!CBB_init(cbb.get(), 32) ||
      // Protobuf entries are (tag, value) pairs.
      !cbb_add_varint(cbb.get(), EncodeTag(kTagRequestId, WireType::kVarint)) ||
      !cbb_add_varint(cbb.get(), 0) ||

      !cbb_add_varint(cbb.get(), EncodeTag(kTagEventType, WireType::kVarint)) ||
      !cbb_add_varint(cbb.get(), kTagChromeLog) ||

      !cbb_add_varint(cbb.get(),
                      EncodeTag(kTagTunnelId, WireType::kLengthPrefixed)) ||
      !cbb_add_varint(cbb.get(), tunnel_id.size()) ||
      !CBB_add_bytes(cbb.get(), tunnel_id.data(), tunnel_id.size()) ||

      !cbb_add_varint(cbb.get(),
                      EncodeTag(kTagChromeLog, WireType::kLengthPrefixed)) ||
      !cbb_add_varint(cbb.get(), inner_length) ||
      !CBB_add_bytes(cbb.get(), inner_bytes, inner_length) ||

      !CBB_finish(cbb.get(), &bytes, &length)) {
    return;
  }
  bssl::UniquePtr<uint8_t> bytes_storage(bytes);

  Java_CableAuthenticator_logEvent(env, ToJavaByteArray(env, bytes, length));
}

}  // namespace protobuf

// CableV2MobileEvent enumerates several steps that occur during a caBLEv2
// transaction. Do not change the assigned value since they are used in
// histograms, only append new values. Keep synced with enums.xml.
enum class CableV2MobileEvent {
  kQRRead = 0,
  kServerLink = 1,
  kCloudMessage = 2,
  kUSB = 3,
  kSetup = 4,
  kTunnelServerConnected = 5,
  kHandshakeCompleted = 6,
  kRequestReceived = 7,
  kCTAPError = 8,
  kUnlink = 9,
  kNeedInteractive = 10,
  kInteractionReady = 11,
  kLinkingNotRequested = 12,
  kUSBSuccess = 13,
  kStoppedWhileAwaitingTunnelServerConnection = 14,
  kStoppedWhileAwaitingHandshake = 15,
  kStoppedWhileAwaitingRequest = 16,
  kStoppedWhileAuthenticating = 17,
  kStrayGetAssertionResponse = 18,
  kGetAssertionStarted = 19,
  kGetAssertionComplete = 20,
  kFirstTransactionDone = 21,
  kContactIDNotReady = 22,
  kBluetoothAdvertisePermissionRequested = 23,
  kBluetoothAdvertisePermissionGranted = 24,
  kBluetoothAdvertisePermissionRejected = 25,

  kMaxValue = 25,
};

// CableV2MobileResult enumerates the outcome of a caBLEv2 transction. Do not
// change the assigned value since they are used in histograms, only append new
// values. Keep synced with enums.xml.
enum class CableV2MobileResult {
  kSuccess = 0,
  kUnexpectedEOF = 1,
  kTunnelServerConnectFailed = 2,
  kHandshakeFailed = 3,
  kDecryptFailure = 4,
  kInvalidCBOR = 5,
  kInvalidCTAP = 6,
  kUnknownCommand = 7,
  kInternalError = 8,
  kInvalidQR = 9,
  kInvalidServerLink = 10,
  kEOFWhileProcessing = 11,
  kDiscoverableCredentialsRejected = 12,

  kMaxValue = 12,
};

// JavaByteArrayToSpan returns a span that aliases |data|. Be aware that the
// reference for |data| must outlive the span.
base::span<const uint8_t> JavaByteArrayToSpan(
    JNIEnv* env,
    const JavaParamRef<jbyteArray>& data) {
  if (data.is_null()) {
    return base::span<const uint8_t>();
  }

  const size_t data_len = env->GetArrayLength(data);
  const jbyte* data_bytes = env->GetByteArrayElements(data, /*iscopy=*/nullptr);
  return base::as_bytes(base::make_span(data_bytes, data_len));
}

// JavaByteArrayToFixedSpan returns a span that aliases |data|, or |nullopt| if
// the span is not of the correct length. Be aware that the reference for |data|
// must outlive the span.
template <size_t N>
absl::optional<base::span<const uint8_t, N>> JavaByteArrayToFixedSpan(
    JNIEnv* env,
    const JavaParamRef<jbyteArray>& data) {
  static_assert(N != 0,
                "Zero case is different from JavaByteArrayToSpan as null "
                "inputs will always be rejected here.");

  if (data.is_null()) {
    return absl::nullopt;
  }

  const size_t data_len = env->GetArrayLength(data);
  if (data_len != N) {
    return absl::nullopt;
  }
  const jbyte* data_bytes = env->GetByteArrayElements(data, /*iscopy=*/nullptr);
  return base::as_bytes(base::make_span<N>(data_bytes, data_len));
}

// GlobalData holds all the state for ongoing security key operations. Since
// there are ultimately only one human user, concurrent requests are not
// supported.
struct GlobalData {
  JNIEnv* env = nullptr;
  // instance_num is incremented for each new |Transaction| created and returned
  // to Java to serve as a "handle". This prevents commands intended for a
  // previous transaction getting applied to a replacement. The zero value is
  // reserved so that functions can still return that to indicate an error.
  jlong instance_num = 1;

  // metrics_enabled records whether the user opted into metrics and crash
  // reporting. If so then logging of events related to server-link
  // transactions is permitted.
  bool metrics_enabled = false;

  absl::optional<std::array<uint8_t, device::cablev2::kRootSecretSize>>
      root_secret;
  network::mojom::NetworkContext* network_context = nullptr;

  // event_to_record_if_stopped contains an event to record with UMA if the
  // activity is stopped. This is updated as a transaction progresses.
  absl::optional<CableV2MobileEvent> event_to_record_if_stopped;

  // registration is a non-owning pointer to the global |Registration|.
  device::cablev2::authenticator::Registration* registration = nullptr;

  // current_transaction holds the |Transaction| that is currently active.
  std::unique_ptr<device::cablev2::authenticator::Transaction>
      current_transaction;

  // pending_make_credential_callback holds the callback that the
  // |Authenticator| expects to be run once a makeCredential operation has
  // completed.
  absl::optional<
      device::cablev2::authenticator::Platform::MakeCredentialCallback>
      pending_make_credential_callback;
  // pending_get_assertion_callback holds the callback that the
  // |Authenticator| expects to be run once a getAssertion operation has
  // completed.
  absl::optional<device::cablev2::authenticator::Platform::GetAssertionCallback>
      pending_get_assertion_callback;
  absl::optional<base::TimeTicks> get_assertion_start_time;

  // usb_callback holds the callback that receives data from a USB connection.
  absl::optional<
      base::RepeatingCallback<void(absl::optional<base::span<const uint8_t>>)>>
      usb_callback;

  // server_link_tunnel_id contains the derived tunnel ID for server–link
  // transactions. May be |nullopt| if the current transaction is not
  // server-linked. This is used as an event ID when logging.
  absl::optional<std::array<uint8_t, device::cablev2::kTunnelIdSize>>
      server_link_tunnel_id;
};

// GetGlobalData returns a pointer to the unique |GlobalData| for the address
// space.
GlobalData& GetGlobalData() {
  static base::NoDestructor<GlobalData> global_data;
  return *global_data;
}

void ResetGlobalData() {
  GlobalData& global_data = GetGlobalData();
  global_data.metrics_enabled = false;
  global_data.current_transaction.reset();
  global_data.pending_make_credential_callback.reset();
  global_data.pending_get_assertion_callback.reset();
  global_data.get_assertion_start_time.reset();
  global_data.usb_callback.reset();
  global_data.server_link_tunnel_id.reset();
}

void RecordEvent(const GlobalData* global_data, CableV2MobileEvent event) {
  base::UmaHistogramEnumeration("WebAuthentication.CableV2.MobileEvent", event);

  if (global_data && global_data->metrics_enabled &&
      global_data->server_link_tunnel_id.has_value()) {
    protobuf::LogEvent(global_data->env, *global_data->server_link_tunnel_id,
                       protobuf::Type::kEvent, static_cast<unsigned>(event));
  }
}

void RecordResult(const GlobalData* global_data, CableV2MobileResult result) {
  base::UmaHistogramEnumeration("WebAuthentication.CableV2.MobileResult",
                                result);

  if (global_data && global_data->metrics_enabled &&
      global_data->server_link_tunnel_id.has_value()) {
    protobuf::LogEvent(global_data->env, *global_data->server_link_tunnel_id,
                       protobuf::Type::kResult, static_cast<unsigned>(result));
  }
}

// AndroidBLEAdvert wraps a Java |BLEAdvert| object so that
// |authenticator::Platform| can hold it.
class AndroidBLEAdvert
    : public device::cablev2::authenticator::Platform::BLEAdvert {
 public:
  AndroidBLEAdvert(JNIEnv* env, ScopedJavaGlobalRef<jobject> advert)
      : env_(env), advert_(std::move(advert)) {
    DCHECK(env_->IsInstanceOf(
        advert_.obj(),
        org_chromium_chrome_browser_webauth_authenticator_BLEAdvert_clazz(
            env)));
  }

  ~AndroidBLEAdvert() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    Java_BLEAdvert_close(env_, advert_);
  }

 private:
  const raw_ptr<JNIEnv> env_;
  const ScopedJavaGlobalRef<jobject> advert_;
  SEQUENCE_CHECKER(sequence_checker_);
};

// AndroidPlatform implements |authenticator::Platform| using the GMSCore
// implementation of FIDO operations.
class AndroidPlatform : public device::cablev2::authenticator::Platform {
 public:
  typedef base::OnceCallback<void(ScopedJavaGlobalRef<jobject>)>
      InteractionReadyCallback;
  typedef base::OnceCallback<void(InteractionReadyCallback)>
      InteractionNeededCallback;

  AndroidPlatform(JNIEnv* env,
                  const JavaRef<jobject>& cable_authenticator,
                  bool is_usb)
      : env_(env), cable_authenticator_(cable_authenticator), is_usb_(is_usb) {
    DCHECK(env_->IsInstanceOf(
        cable_authenticator_.obj(),
        org_chromium_chrome_browser_webauth_authenticator_CableAuthenticator_clazz(
            env)));
  }

  ~AndroidPlatform() override = default;

  // Platform:
  void MakeCredential(
      blink::mojom::PublicKeyCredentialCreationOptionsPtr params,
      MakeCredentialCallback callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    GlobalData& global_data = GetGlobalData();
    DCHECK(!global_data.pending_make_credential_callback);
    global_data.pending_make_credential_callback = std::move(callback);

    std::vector<uint8_t> params_bytes =
        blink::mojom::PublicKeyCredentialCreationOptions::Serialize(&params);

    Java_CableAuthenticator_makeCredential(env_, cable_authenticator_,
                                           ToJavaByteArray(env_, params_bytes));
  }

  void GetAssertion(blink::mojom::PublicKeyCredentialRequestOptionsPtr params,
                    GetAssertionCallback callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    GlobalData& global_data = GetGlobalData();
    DCHECK(!global_data.pending_get_assertion_callback);
    global_data.pending_get_assertion_callback = std::move(callback);
    global_data.get_assertion_start_time = base::TimeTicks::Now();

    std::vector<uint8_t> params_bytes =
        blink::mojom::PublicKeyCredentialRequestOptions::Serialize(&params);

    RecordEvent(&global_data, CableV2MobileEvent::kGetAssertionStarted);
    Java_CableAuthenticator_getAssertion(env_, cable_authenticator_,
                                         ToJavaByteArray(env_, params_bytes));
  }

  void OnStatus(Status status) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    LOG(ERROR) << __func__ << " " << static_cast<int>(status);

    GlobalData& global_data = GetGlobalData();
    CableV2MobileEvent event;
    switch (status) {
      case Status::TUNNEL_SERVER_CONNECT:
        event = CableV2MobileEvent::kTunnelServerConnected;
        tunnel_server_connect_time_.emplace();
        global_data.event_to_record_if_stopped =
            CableV2MobileEvent::kStoppedWhileAwaitingHandshake;
        break;
      case Status::HANDSHAKE_COMPLETE:
        if (tunnel_server_connect_time_) {
          base::UmaHistogramMediumTimes(
              "WebAuthentication.CableV2.RendezvousTime",
              tunnel_server_connect_time_->Elapsed());
          tunnel_server_connect_time_.reset();
        }
        event = CableV2MobileEvent::kHandshakeCompleted;
        global_data.event_to_record_if_stopped =
            CableV2MobileEvent::kStoppedWhileAwaitingRequest;
        break;
      case Status::REQUEST_RECEIVED:
        event = CableV2MobileEvent::kRequestReceived;
        global_data.event_to_record_if_stopped =
            CableV2MobileEvent::kStoppedWhileAuthenticating;
        break;
      case Status::CTAP_ERROR:
        event = CableV2MobileEvent::kCTAPError;
        break;
      case Status::FIRST_TRANSACTION_DONE:
        global_data.event_to_record_if_stopped.reset();
        event = CableV2MobileEvent::kFirstTransactionDone;
        break;
    }
    RecordEvent(&global_data, event);

    if (!cable_authenticator_) {
      return;
    }

    Java_CableAuthenticator_onStatus(env_, cable_authenticator_,
                                     static_cast<int>(status));
  }

  void OnCompleted(absl::optional<Error> maybe_error) override {
    LOG(ERROR) << __func__ << " "
               << (maybe_error ? static_cast<int>(*maybe_error) : -1);
    GlobalData& global_data = GetGlobalData();
    global_data.event_to_record_if_stopped.reset();

    CableV2MobileResult result = CableV2MobileResult::kSuccess;
    if (maybe_error) {
      switch (*maybe_error) {
        case Error::UNEXPECTED_EOF:
          result = CableV2MobileResult::kUnexpectedEOF;
          break;
        case Error::EOF_WHILE_PROCESSING:
          result = CableV2MobileResult::kEOFWhileProcessing;
          break;
        case Error::TUNNEL_SERVER_CONNECT_FAILED:
          result = CableV2MobileResult::kTunnelServerConnectFailed;
          break;
        case Error::HANDSHAKE_FAILED:
          result = CableV2MobileResult::kHandshakeFailed;
          break;
        case Error::DECRYPT_FAILURE:
          result = CableV2MobileResult::kDecryptFailure;
          break;
        case Error::INVALID_CBOR:
          result = CableV2MobileResult::kInvalidCBOR;
          break;
        case Error::INVALID_CTAP:
          result = CableV2MobileResult::kInvalidCTAP;
          break;
        case Error::UNKNOWN_COMMAND:
          result = CableV2MobileResult::kUnknownCommand;
          break;
        case Error::AUTHENTICATOR_SELECTION_RECEIVED:
        case Error::DISCOVERABLE_CREDENTIALS_REQUEST:
          result = CableV2MobileResult::kDiscoverableCredentialsRejected;
          break;
        case Error::INTERNAL_ERROR:
        case Error::SERVER_LINK_WRONG_LENGTH:
        case Error::SERVER_LINK_NOT_ON_CURVE:
        case Error::NO_SCREENLOCK:
        case Error::NO_BLUETOOTH_PERMISSION:
        case Error::QR_URI_ERROR:
          result = CableV2MobileResult::kInternalError;
          break;
      }
    }
    RecordResult(&global_data, result);

    if (is_usb_ && result == CableV2MobileResult::kSuccess) {
      RecordEvent(nullptr, CableV2MobileEvent::kUSBSuccess);
    }

    // The transaction might fail before interactive mode, thus
    // |cable_authenticator_| may be empty.
    if (cable_authenticator_) {
      const bool ok = !maybe_error.has_value();
      Java_CableAuthenticator_onComplete(
          env_, cable_authenticator_, ok,
          ok ? 0 : static_cast<int>(*maybe_error));
    }
    // ResetGlobalData will delete the |Transaction|, which will delete this
    // object. Thus nothing else can be done after this call.
    ResetGlobalData();
  }

  std::unique_ptr<BLEAdvert> SendBLEAdvert(
      base::span<const uint8_t, device::cablev2::kAdvertSize> payload)
      override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return std::make_unique<AndroidBLEAdvert>(
        env_, ScopedJavaGlobalRef<jobject>(Java_CableAuthenticator_newBLEAdvert(
                  env_, ToJavaByteArray(env_, payload))));
  }

 private:
  const raw_ptr<JNIEnv> env_;
  ScopedJavaGlobalRef<jobject> cable_authenticator_;
  absl::optional<base::ElapsedTimer> tunnel_server_connect_time_;

  // is_usb_ is true if this object was created in order to respond to a client
  // connected over USB.
  const bool is_usb_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<AndroidPlatform> weak_factory_{this};
};

// USBTransport wraps the Java |USBHandler| object so that
// |authenticator::Platform| can use it as a transport.
class USBTransport : public device::cablev2::authenticator::Transport {
 public:
  USBTransport(JNIEnv* env, ScopedJavaGlobalRef<jobject> usb_device)
      : env_(env), usb_device_(std::move(usb_device)) {
    DCHECK(env_->IsInstanceOf(
        usb_device_.obj(),
        org_chromium_chrome_browser_webauth_authenticator_USBHandler_clazz(
            env)));
  }

  ~USBTransport() override { Java_USBHandler_close(env_, usb_device_); }

  // GetCallback returns callback which will be called repeatedly with data from
  // the USB connection, forwarded via the Java code.
  base::RepeatingCallback<void(absl::optional<base::span<const uint8_t>>)>
  GetCallback() {
    return base::BindRepeating(&USBTransport::OnData,
                               weak_factory_.GetWeakPtr());
  }

  // Transport:
  void StartReading(
      base::RepeatingCallback<void(Update)> update_callback) override {
    callback_ = update_callback;
    Java_USBHandler_startReading(env_, usb_device_);
  }

  void Write(std::vector<uint8_t> data) override {
    Java_USBHandler_write(env_, usb_device_, ToJavaByteArray(env_, data));
  }

 private:
  void OnData(absl::optional<base::span<const uint8_t>> data) {
    if (!data) {
      callback_.Run(Disconnected::kDisconnected);
    } else {
      callback_.Run(device::fido_parsing_utils::Materialize(*data));
    }
  }

  const raw_ptr<JNIEnv> env_;
  const ScopedJavaGlobalRef<jobject> usb_device_;
  base::RepeatingCallback<void(Update)> callback_;
  base::WeakPtrFactory<USBTransport> weak_factory_{this};
};

}  // anonymous namespace

// These functions are the entry points for CableAuthenticator.java and
// BLEHandler.java calling into C++.

static void JNI_CableAuthenticator_Setup(JNIEnv* env,
                                         jlong registration_long,
                                         jlong network_context_long,
                                         const JavaParamRef<jbyteArray>& secret,
                                         jboolean metrics_enabled) {
  GlobalData& global_data = GetGlobalData();
  global_data.metrics_enabled = metrics_enabled;

  // The root_secret may not be provided when triggered for server-link. It
  // won't be used in that case either, but we need to be able to grab it if
  // setup() is called called for a different type of exchange.
  base::span<const uint8_t> root_secret = JavaByteArrayToSpan(env, secret);
  if (!root_secret.empty() && !global_data.root_secret) {
    global_data.root_secret.emplace();
    CHECK_EQ(global_data.root_secret->size(), root_secret.size());
    memcpy(global_data.root_secret->data(), root_secret.data(),
           global_data.root_secret->size());
  }

  // If starting a new transaction, don't record anything if stopped.
  global_data.event_to_record_if_stopped.reset();

  // This function can be called multiple times and must be idempotent. The
  // |env| member of |global_data| is used to flag whether setup has
  // already occurred.
  if (global_data.env) {
    return;
  }

  RecordEvent(&global_data, CableV2MobileEvent::kSetup);
  global_data.env = env;

  static_assert(sizeof(jlong) >= sizeof(void*), "");
  global_data.registration =
      reinterpret_cast<device::cablev2::authenticator::Registration*>(
          registration_long);
  global_data.registration->PrepareContactID();

  global_data.network_context =
      reinterpret_cast<network::mojom::NetworkContext*>(network_context_long);
}

static jlong JNI_CableAuthenticator_StartUSB(
    JNIEnv* env,
    const JavaParamRef<jobject>& cable_authenticator,
    const JavaParamRef<jobject>& usb_device) {
  GlobalData& global_data = GetGlobalData();
  RecordEvent(&global_data, CableV2MobileEvent::kUSB);

  auto transport = std::make_unique<USBTransport>(
      env, ScopedJavaGlobalRef<jobject>(usb_device));
  DCHECK(!global_data.usb_callback);
  global_data.usb_callback = transport->GetCallback();

  global_data.current_transaction =
      device::cablev2::authenticator::TransactWithPlaintextTransport(
          std::make_unique<AndroidPlatform>(env, cable_authenticator,
                                            /*is_usb=*/true),
          std::unique_ptr<device::cablev2::authenticator::Transport>(
              transport.release()));

  return ++global_data.instance_num;
}

static jlong JNI_CableAuthenticator_StartQR(
    JNIEnv* env,
    const JavaParamRef<jobject>& cable_authenticator,
    const JavaParamRef<jstring>& authenticator_name,
    const JavaParamRef<jstring>& qr_uri,
    jboolean link) {
  GlobalData& global_data = GetGlobalData();
  RecordEvent(&global_data, CableV2MobileEvent::kQRRead);

  const std::string& qr_string = ConvertJavaStringToUTF8(qr_uri);
  absl::optional<device::cablev2::qr::Components> decoded_qr(
      device::cablev2::qr::Parse(qr_string));
  if (!decoded_qr) {
    FIDO_LOG(ERROR) << "Failed to decode QR: " << qr_string;
    RecordResult(&global_data, CableV2MobileResult::kInvalidQR);
    return 0;
  }

  if (!link) {
    RecordEvent(&global_data, CableV2MobileEvent::kLinkingNotRequested);
  } else if (!global_data.registration->contact_id()) {
    LOG(ERROR) << "Contact ID was not ready for QR transaction";
    RecordEvent(&global_data, CableV2MobileEvent::kContactIDNotReady);
  }

  global_data.event_to_record_if_stopped =
      CableV2MobileEvent::kStoppedWhileAwaitingTunnelServerConnection;
  global_data
      .current_transaction = device::cablev2::authenticator::TransactFromQRCode(
      // Just because the client supports storing linking information doesn't
      // imply that it supports revision one, but we happened to introduce
      // these features at the same time.
      /*protocol_revision=*/decoded_qr->supports_linking.has_value() ? 1 : 0,
      std::make_unique<AndroidPlatform>(env, cable_authenticator,
                                        /*is_usb=*/false),
      global_data.network_context, *global_data.root_secret,
      ConvertJavaStringToUTF8(authenticator_name), decoded_qr->secret,
      decoded_qr->peer_identity,
      link ? global_data.registration->contact_id() : absl::nullopt,
      // If the QR code knows about at least two registered tunnel server
      // domains then we consider it recent enough to use the new Crypter mode.
      /*use_new_crypter_construction=*/decoded_qr->num_known_domains >= 2);

  return ++global_data.instance_num;
}

std::tuple<base::span<const uint8_t, device::kP256X962Length>,
           base::span<const uint8_t, device::cablev2::kQRSecretSize>,
           std::array<uint8_t, device::cablev2::kTunnelIdSize>>
ParseServerLinkData(JNIEnv* env,
                    const JavaParamRef<jbyteArray>& server_link_data_java) {
  constexpr size_t kDataSize =
      device::kP256X962Length + device::cablev2::kQRSecretSize;
  const absl::optional<base::span<const uint8_t, kDataSize>> server_link_data =
      JavaByteArrayToFixedSpan<kDataSize>(env, server_link_data_java);
  // validateServerLinkData should have been called to check this already.
  CHECK(server_link_data);

  const base::span<const uint8_t, device::kP256X962Length> peer_identity =
      server_link_data->subspan<0, device::kP256X962Length>();
  const base::span<const uint8_t, device::cablev2::kQRSecretSize> qr_secret =
      server_link_data
          ->subspan<device::kP256X962Length, device::cablev2::kQRSecretSize>();
  const std::array<uint8_t, device::cablev2::kTunnelIdSize> tunnel_id =
      device::cablev2::Derive<device::cablev2::kTunnelIdSize>(
          qr_secret, base::span<uint8_t>(),
          device::cablev2::DerivedValueType::kTunnelID);

  return std::make_tuple(peer_identity, qr_secret, tunnel_id);
}

static jlong JNI_CableAuthenticator_StartServerLink(
    JNIEnv* env,
    const JavaParamRef<jobject>& cable_authenticator,
    const JavaParamRef<jbyteArray>& server_link_data_java) {
  GlobalData& global_data = GetGlobalData();

  auto server_link_values = ParseServerLinkData(env, server_link_data_java);
  auto peer_identity = std::get<0>(server_link_values);
  auto qr_secret = std::get<1>(server_link_values);
  global_data.server_link_tunnel_id = std::get<2>(server_link_values);

  // Sending pairing information is disabled when doing a server-linked
  // connection, thus the root secret and authenticator name will not be used.
  std::array<uint8_t, device::cablev2::kRootSecretSize> dummy_root_secret = {0};
  std::string dummy_authenticator_name = "";
  global_data.event_to_record_if_stopped =
      CableV2MobileEvent::kStoppedWhileAwaitingTunnelServerConnection;
  RecordEvent(&global_data, CableV2MobileEvent::kServerLink);

  global_data.current_transaction =
      device::cablev2::authenticator::TransactFromQRCode(
          /*protocol_revision=*/0,
          std::make_unique<AndroidPlatform>(env, cable_authenticator,
                                            /*is_usb=*/false),
          global_data.network_context, dummy_root_secret,
          dummy_authenticator_name, qr_secret, peer_identity, absl::nullopt,
          /*use_new_crypter_construction=*/false);

  return ++global_data.instance_num;
}

static jlong JNI_CableAuthenticator_StartCloudMessage(
    JNIEnv* env,
    const JavaParamRef<jobject>& cable_authenticator,
    const JavaParamRef<jbyteArray>& serialized_event) {
  GlobalData& global_data = GetGlobalData();
  RecordEvent(&global_data, CableV2MobileEvent::kCloudMessage);

  auto event =
      device::cablev2::authenticator::Registration::Event::FromSerialized(
          JavaByteArrayToSpan(env, serialized_event));
  if (!event) {
    LOG(ERROR) << "Failed to parse event";
    return 0;
  }

  DCHECK((event->source ==
          device::cablev2::authenticator::Registration::Type::LINKING) ==
         event->contact_id.has_value());

  // There is deliberately no check for |!global_data.current_transaction|
  // because multiple Cloud messages may come in from different paired devices.
  // Only the most recent is processed.
  global_data.event_to_record_if_stopped =
      CableV2MobileEvent::kStoppedWhileAwaitingTunnelServerConnection;
  global_data.current_transaction =
      device::cablev2::authenticator::TransactFromFCM(
          event->protocol_revision,
          std::make_unique<AndroidPlatform>(env, cable_authenticator,
                                            /*is_usb=*/false),
          global_data.network_context, *global_data.root_secret,
          event->routing_id, event->tunnel_id, event->pairing_id,
          event->client_nonce, event->contact_id);

  return ++global_data.instance_num;
}

static void JNI_CableAuthenticator_Stop(JNIEnv* env, jlong instance_num) {
  GlobalData& global_data = GetGlobalData();
  if (global_data.instance_num == instance_num) {
    ResetGlobalData();
  }
}

static int JNI_CableAuthenticator_ValidateServerLinkData(
    JNIEnv* env,
    const JavaParamRef<jbyteArray>& jdata) {
  base::span<const uint8_t> data = JavaByteArrayToSpan(env, jdata);
  if (data.size() != device::kP256X962Length + device::cablev2::kQRSecretSize) {
    RecordResult(nullptr, CableV2MobileResult::kInvalidServerLink);
    return static_cast<int>(device::cablev2::authenticator::Platform::Error::
                                SERVER_LINK_WRONG_LENGTH);
  }

  base::span<const uint8_t> x962 = data.subspan(0, device::kP256X962Length);
  bssl::UniquePtr<EC_GROUP> p256(
      EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  bssl::UniquePtr<EC_POINT> point(EC_POINT_new(p256.get()));
  if (!EC_POINT_oct2point(p256.get(), point.get(), x962.data(), x962.size(),
                          /*ctx=*/nullptr)) {
    RecordResult(nullptr, CableV2MobileResult::kInvalidServerLink);
    return static_cast<int>(device::cablev2::authenticator::Platform::Error::
                                SERVER_LINK_NOT_ON_CURVE);
  }

  return 0;
}

static int JNI_CableAuthenticator_ValidateQRURI(
    JNIEnv* env,
    const JavaParamRef<jstring>& qr_uri) {
  const std::string& qr_string = ConvertJavaStringToUTF8(qr_uri);
  if (!device::cablev2::qr::Parse(qr_string)) {
    RecordResult(nullptr, CableV2MobileResult::kInvalidQR);
    return static_cast<int>(
        device::cablev2::authenticator::Platform::Error::QR_URI_ERROR);
  }

  return 0;
}

static void JNI_CableAuthenticator_OnActivityStop(JNIEnv* env,
                                                  jlong instance_num) {
  GlobalData& global_data = GetGlobalData();
  if (global_data.event_to_record_if_stopped &&
      global_data.instance_num == instance_num) {
    RecordEvent(&global_data, *global_data.event_to_record_if_stopped);
    global_data.event_to_record_if_stopped.reset();
  }
}

static void JNI_CableAuthenticator_OnAuthenticatorAttestationResponse(
    JNIEnv* env,
    jint ctap_status,
    const JavaParamRef<jbyteArray>& jattestation_object) {
  GlobalData& global_data = GetGlobalData();

  if (!global_data.pending_make_credential_callback) {
    return;
  }
  auto callback = std::move(*global_data.pending_make_credential_callback);
  global_data.pending_make_credential_callback.reset();

  std::move(callback).Run(ctap_status,
                          JavaByteArrayToSpan(env, jattestation_object));
}

static void JNI_CableAuthenticator_OnAuthenticatorAssertionResponse(
    JNIEnv* env,
    jint ctap_status,
    const JavaParamRef<jbyteArray>& jresponse_bytes) {
  GlobalData& global_data = GetGlobalData();
  RecordEvent(&global_data, CableV2MobileEvent::kGetAssertionComplete);

  // TODO: |get_assertion_start_time| should always be present in this case.
  // But, at the time of writing, we are seeing some odd numbers in UMA metrics
  // and aren't sure what's going on. Thus there's an if to avoid introducing
  // a crash. If the number of records for this histogram are comparible to
  // the number of recorded starts, then this can be removed.
  DCHECK(global_data.get_assertion_start_time);
  if (global_data.get_assertion_start_time) {
    const base::TimeDelta duration =
        base::TimeTicks::Now() - global_data.get_assertion_start_time.value();
    base::UmaHistogramMediumTimes("WebAuthentication.CableV2.GetAssertionTime",
                                  duration);
    global_data.get_assertion_start_time.reset();
  }

  if (!global_data.pending_get_assertion_callback) {
    RecordEvent(&global_data, CableV2MobileEvent::kStrayGetAssertionResponse);
    return;
  }
  auto callback = std::move(*global_data.pending_get_assertion_callback);
  global_data.pending_get_assertion_callback.reset();

  if (ctap_status ==
      static_cast<jint>(device::CtapDeviceResponseCode::kSuccess)) {
    base::span<const uint8_t> response_bytes =
        JavaByteArrayToSpan(env, jresponse_bytes);
    auto response = blink::mojom::GetAssertionAuthenticatorResponse::New();
    if (blink::mojom::GetAssertionAuthenticatorResponse::Deserialize(
            response_bytes.data(), response_bytes.size(), &response)) {
      std::move(callback).Run(ctap_status, std::move(response));
      return;
    }

    ctap_status =
        static_cast<jint>(device::CtapDeviceResponseCode::kCtap2ErrOther);
  }

  std::move(callback).Run(ctap_status, nullptr);
}

static void JNI_CableAuthenticator_RecordEvent(
    JNIEnv* env,
    jint event,
    const JavaParamRef<jbyteArray>& server_link_data_java) {
  auto server_link_values = ParseServerLinkData(env, server_link_data_java);
  base::UmaHistogramEnumeration("WebAuthentication.CableV2.MobileEvent",
                                static_cast<CableV2MobileEvent>(event));
  protobuf::LogEvent(env, /*tunnel_id=*/std::get<2>(server_link_values),
                     protobuf::Type::kEvent, event);
}

static void JNI_USBHandler_OnUSBData(JNIEnv* env,
                                     const JavaParamRef<jbyteArray>& usb_data) {
  GlobalData& global_data = GetGlobalData();
  if (!global_data.usb_callback) {
    return;
  }

  if (!usb_data) {
    global_data.usb_callback->Run(absl::nullopt);
  } else {
    global_data.usb_callback->Run(JavaByteArrayToSpan(env, usb_data));
  }
}
