// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <tuple>
#include <utility>

#include "base/command_line.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/media/media_browsertest.h"
#include "chrome/browser/media/test_license_server.h"
#include "chrome/browser/media/wv_test_license_server_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/test_launcher_utils.h"
#include "components/prefs/pref_service.h"
#include "components/variations/variations_switches.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/key_system_names.h"
#include "media/base/media_switches.h"
#include "media/base/test_data_util.h"
#include "media/cdm/supported_cdm_versions.h"
#include "media/media_buildflags.h"
#include "testing/gtest/include/gtest/gtest-spi.h"
#include "third_party/widevine/cdm/buildflags.h"
#include "third_party/widevine/cdm/widevine_cdm_common.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/windows_version.h"
#endif

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
#include "chrome/browser/media/library_cdm_test_helper.h"
#include "media/cdm/cdm_paths.h"
#endif

// Available key systems.
const char kClearKeyKeySystem[] = "org.w3.clearkey";
const char kExternalClearKeyKeySystem[] = "org.chromium.externalclearkey";

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
// Variants of External Clear Key key system to test different scenarios.
// To add a new variant, make sure you also update:
// - media/test/data/eme_player_js/globals.js
// - media/test/data/eme_player_js/player_utils.js
// - CreateCdmInstance() in clear_key_cdm.cc
const char kExternalClearKeyMessageTypeTestKeySystem[] =
    "org.chromium.externalclearkey.messagetypetest";
const char kExternalClearKeyFileIOTestKeySystem[] =
    "org.chromium.externalclearkey.fileiotest";
const char kExternalClearKeyInitializeFailKeySystem[] =
    "org.chromium.externalclearkey.initializefail";
const char kExternalClearKeyPlatformVerificationTestKeySystem[] =
    "org.chromium.externalclearkey.platformverificationtest";
const char kExternalClearKeyCrashKeySystem[] =
    "org.chromium.externalclearkey.crash";
#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
const char kExternalClearKeyVerifyCdmHostTestKeySystem[] =
    "org.chromium.externalclearkey.verifycdmhosttest";
#endif  // BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
const char kExternalClearKeyStorageIdTestKeySystem[] =
    "org.chromium.externalclearkey.storageidtest";
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

// Sessions to load.
const char kNoSessionToLoad[] = "";
#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
const char kPersistentLicense[] = "PersistentLicense";
const char kUnknownSession[] = "UnknownSession";
#endif

// EME-specific test results and errors.
const char kUnitTestSuccess[] = "UNIT_TEST_SUCCESS";
const char16_t kEmeUnitTestFailure16[] = u"UNIT_TEST_FAILURE";
const char kEmeNotSupportedError[] = "NOTSUPPORTEDERROR";
const char16_t kEmeNotSupportedError16[] = u"NOTSUPPORTEDERROR";
const char16_t kEmeGenerateRequestFailed[] = u"EME_GENERATEREQUEST_FAILED";
const char16_t kEmeSessionNotFound16[] = u"EME_SESSION_NOT_FOUND";
const char16_t kEmeLoadFailed[] = u"EME_LOAD_FAILED";
const char kEmeUpdateFailed[] = "EME_UPDATE_FAILED";
const char16_t kEmeUpdateFailed16[] = u"EME_UPDATE_FAILED";
const char16_t kEmeErrorEvent[] = u"EME_ERROR_EVENT";
const char16_t kEmeMessageUnexpectedType[] = u"EME_MESSAGE_UNEXPECTED_TYPE";
const char16_t kEmeRenewalMissingHeader[] = u"EME_RENEWAL_MISSING_HEADER";
#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
const char kEmeSessionClosedAndError[] = "EME_SESSION_CLOSED_AND_ERROR";
const char kEmeSessionNotFound[] = "EME_SESSION_NOT_FOUND";
#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
const char kEmeUnitTestFailure[] = "UNIT_TEST_FAILURE";
#endif
#endif

const char kDefaultEmePlayer[] = "eme_player.html";
const char kDefaultMseOnlyEmePlayer[] = "mse_different_containers.html";

// The type of video src used to load media.
enum class SrcType { SRC, MSE };

// Must be in sync with CONFIG_CHANGE_TYPE in eme_player_js/global.js
enum class ConfigChangeType {
  CLEAR_TO_CLEAR = 0,
  CLEAR_TO_ENCRYPTED = 1,
  ENCRYPTED_TO_CLEAR = 2,
  ENCRYPTED_TO_ENCRYPTED = 3,
};

// Whether the video should be played once or twice.
enum class PlayCount { ONCE, TWICE };

// Base class for encrypted media tests.
class EncryptedMediaTestBase : public MediaBrowserTest {
 public:
  bool IsExternalClearKey(const std::string& key_system) {
    if (key_system == kExternalClearKeyKeySystem)
      return true;
    std::string prefix = std::string(kExternalClearKeyKeySystem) + '.';
    return key_system.substr(0, prefix.size()) == prefix;
  }

#if BUILDFLAG(ENABLE_WIDEVINE)
  bool IsWidevine(const std::string& key_system) {
    return key_system == kWidevineKeySystem;
  }
#endif  // BUILDFLAG(ENABLE_WIDEVINE)

  void RunEncryptedMediaTestPage(const std::string& html_page,
                                 const std::string& key_system,
                                 const base::StringPairs& query_params,
                                 const std::string& expected_title) {
    base::StringPairs new_query_params = query_params;
    StartLicenseServerIfNeeded(key_system, &new_query_params);
    RunMediaTestPage(html_page, new_query_params, expected_title, true);
  }

  // Tests |html_page| using |media_file| and |key_system|.
  // When |session_to_load| is not empty, the test will try to load
  // |session_to_load| with stored keys, instead of creating a new session
  // and trying to update it with licenses.
  // When |force_invalid_response| is true, the test will provide invalid
  // responses, which should trigger errors.
  // TODO(xhwang): Find an easier way to pass multiple configuration test
  // options.
  void RunEncryptedMediaTest(const std::string& html_page,
                             const std::string& media_file,
                             const std::string& key_system,
                             SrcType src_type,
                             const std::string& session_to_load,
                             bool force_invalid_response,
                             PlayCount play_count,
                             const std::string& expected_title) {
    base::StringPairs query_params;
    query_params.emplace_back("mediaFile", media_file);
    query_params.emplace_back("mediaType",
                              media::GetMimeTypeForFile(media_file));
    query_params.emplace_back("keySystem", key_system);
    if (src_type == SrcType::MSE)
      query_params.emplace_back("useMSE", "1");
    if (force_invalid_response)
      query_params.emplace_back("forceInvalidResponse", "1");
    if (!session_to_load.empty())
      query_params.emplace_back("sessionToLoad", session_to_load);
    if (play_count == PlayCount::TWICE)
      query_params.emplace_back("playTwice", "1");
    RunEncryptedMediaTestPage(html_page, key_system, query_params,
                              expected_title);
  }

  void RunSimpleEncryptedMediaTest(const std::string& media_file,
                                   const std::string& key_system,
                                   SrcType src_type) {
    std::string expected_title = media::kEndedTitle;
    if (!IsPlayBackPossible(key_system)) {
      expected_title = kEmeUpdateFailed;
    }

    RunEncryptedMediaTest(kDefaultEmePlayer, media_file, key_system, src_type,
                          kNoSessionToLoad, false, PlayCount::ONCE,
                          expected_title);
    // Check KeyMessage received for all key systems.
    bool receivedKeyMessage = false;
    EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
        browser()->tab_strip_model()->GetActiveWebContents(),
        "window.domAutomationController.send("
        "document.querySelector('video').receivedKeyMessage);",
        &receivedKeyMessage));
    EXPECT_TRUE(receivedKeyMessage);
  }

  void RunEncryptedMediaMultipleFileTest(const std::string& key_system,
                                         const std::string& video_file,
                                         const std::string& audio_file,
                                         const std::string& expected_title) {
    if (!IsPlayBackPossible(key_system)) {
      DVLOG(0) << "Skipping test - Test requires video playback.";
      return;
    }

    base::StringPairs query_params;
    query_params.emplace_back("keySystem", key_system);
    query_params.emplace_back("runEncrypted", "1");
    if (!video_file.empty()) {
      query_params.emplace_back("videoFile", video_file);
      query_params.emplace_back("videoFormat",
                                media::GetMimeTypeForFile(video_file));
    }
    if (!audio_file.empty()) {
      query_params.emplace_back("audioFile", audio_file);
      query_params.emplace_back("audioFormat",
                                media::GetMimeTypeForFile(audio_file));
    }

    RunEncryptedMediaTestPage(kDefaultMseOnlyEmePlayer, key_system,
                              query_params, expected_title);
  }

  // Starts a license server if available for the |key_system| and adds a
  // 'licenseServerURL' query parameter to |query_params|.
  void StartLicenseServerIfNeeded(const std::string& key_system,
                                  base::StringPairs* query_params) {
    std::unique_ptr<TestLicenseServerConfig> config =
        GetServerConfig(key_system);
    if (!config)
      return;
    license_server_ = std::make_unique<TestLicenseServer>(std::move(config));
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      EXPECT_TRUE(license_server_->Start());
    }
    query_params->push_back(
        std::make_pair("licenseServerURL", license_server_->GetServerURL()));
  }

  bool IsPlayBackPossible(const std::string& key_system) {
#if BUILDFLAG(ENABLE_WIDEVINE)
    if (IsWidevine(key_system) && !GetServerConfig(key_system))
      return false;
#endif  // BUILDFLAG(ENABLE_WIDEVINE)
    return true;
  }

  std::unique_ptr<TestLicenseServerConfig> GetServerConfig(
      const std::string& key_system) {
#if BUILDFLAG(ENABLE_WIDEVINE)
    if (IsWidevine(key_system)) {
      std::unique_ptr<TestLicenseServerConfig> config(
          new WVTestLicenseServerConfig);
      if (config->IsPlatformSupported())
        return config;
    }
#endif  // BUILDFLAG(ENABLE_WIDEVINE)
    return nullptr;
  }

 protected:
  std::unique_ptr<TestLicenseServer> license_server_;

  // We want to fail quickly when a test fails because an error is encountered.
  void AddWaitForTitles(content::TitleWatcher* title_watcher) override {
    MediaBrowserTest::AddWaitForTitles(title_watcher);
    title_watcher->AlsoWaitForTitle(kEmeUnitTestFailure16);
    title_watcher->AlsoWaitForTitle(kEmeNotSupportedError16);
    title_watcher->AlsoWaitForTitle(kEmeGenerateRequestFailed);
    title_watcher->AlsoWaitForTitle(kEmeSessionNotFound16);
    title_watcher->AlsoWaitForTitle(kEmeLoadFailed);
    title_watcher->AlsoWaitForTitle(kEmeUpdateFailed16);
    title_watcher->AlsoWaitForTitle(kEmeErrorEvent);
    title_watcher->AlsoWaitForTitle(kEmeMessageUnexpectedType);
    title_watcher->AlsoWaitForTitle(kEmeRenewalMissingHeader);
  }

#if BUILDFLAG(IS_CHROMEOS)
  void SetUpCommandLine(base::CommandLine* command_line) override {
    MediaBrowserTest::SetUpCommandLine(command_line);

    // Persistent license is supported on ChromeOS when protected media
    // identifier is allowed which involves a user action. Use this switch to
    // always allow the identifier for testing purpose. Note that the test page
    // is hosted on "127.0.0.1". See net::EmbeddedTestServer for details.
    command_line->AppendSwitchASCII(
        switches::kUnsafelyAllowProtectedMediaIdentifierForDomain, "127.0.0.1");
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
  void SetUpDefaultCommandLine(base::CommandLine* command_line) override {
    base::CommandLine default_command_line(base::CommandLine::NO_PROGRAM);
    InProcessBrowserTest::SetUpDefaultCommandLine(&default_command_line);
    test_launcher_utils::RemoveCommandLineSwitch(
        default_command_line, switches::kDisableComponentUpdate, command_line);
  }
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

  void SetUpCommandLineForKeySystem(const std::string& key_system,
                                    base::CommandLine* command_line) {
    if (GetServerConfig(key_system)) {
      // Since the web and license servers listen on different ports, we need to
      // disable web-security to send license requests to the license server.
      // TODO(shadi): Add port forwarding to the test web server configuration.
      command_line->AppendSwitch(switches::kDisableWebSecurity);
    }

    // TODO(crbug.com/1243903): WhatsNewUI might be causing timeouts.
    std::vector<base::Feature> enabled_features;
    std::vector<base::Feature> disabled_features = {
        features::kChromeWhatsNewUI};

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
    if (IsExternalClearKey(key_system)) {
      RegisterClearKeyCdm(command_line);
      enabled_features.push_back(media::kExternalClearKeyForTesting);
    }
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

    scoped_feature_list_.InitWithFeatures(enabled_features, disabled_features);
  }

  base::test::ScopedFeatureList scoped_feature_list_;
};

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
// Tests encrypted media playback using ExternalClearKey key system with a
// specific library CDM interface version as test parameter:
// - int: CDM interface version to test
class ECKEncryptedMediaTest : public EncryptedMediaTestBase,
                              public testing::WithParamInterface<int> {
 public:
  int GetCdmInterfaceVersion() { return GetParam(); }

  // We use special |key_system| names to do non-playback related tests,
  // e.g. kExternalClearKeyFileIOTestKeySystem is used to test file IO.
  void TestNonPlaybackCases(const std::string& key_system,
                            const std::string& expected_title) {
    // Since we do not test playback, arbitrarily choose a test file and source
    // type.
    RunEncryptedMediaTest(kDefaultEmePlayer, "bear-a_enc-a.webm", key_system,
                          SrcType::SRC, kNoSessionToLoad, false,
                          PlayCount::ONCE, expected_title);
  }

  void TestPlaybackCase(const std::string& key_system,
                        const std::string& session_to_load,
                        const std::string& expected_title) {
    RunEncryptedMediaTest(kDefaultEmePlayer, "bear-320x240-v_enc-v.webm",
                          key_system, SrcType::MSE, session_to_load, false,
                          PlayCount::ONCE, expected_title);
  }

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    EncryptedMediaTestBase::SetUpCommandLine(command_line);
    SetUpCommandLineForKeySystem(kExternalClearKeyKeySystem, command_line);
    // Override enabled CDM interface version for testing.
    command_line->AppendSwitchASCII(
        switches::kOverrideEnabledCdmInterfaceVersion,
        base::NumberToString(GetCdmInterfaceVersion()));
  }
};

// Tests encrypted media playback with output protection using ExternalClearKey
// key system with a specific display surface to be captured specified as the
// test parameter.
class ECKEncryptedMediaOutputProtectionTest
    : public EncryptedMediaTestBase,
      public testing::WithParamInterface<const char*> {
 public:
  void TestOutputProtection(bool create_recorder_before_media_keys) {
#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
    // QueryOutputProtectionStatus() is known to fail on Linux Chrome OS builds.
    std::string expected_title = kEmeUnitTestFailure;
#else
    std::string expected_title = kUnitTestSuccess;
#endif

    base::StringPairs query_params;
    if (create_recorder_before_media_keys)
      query_params.emplace_back("createMediaRecorderBeforeMediaKeys", "1");
    RunMediaTestPage("eme_and_get_display_media.html", query_params,
                     expected_title, true);
  }

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    EncryptedMediaTestBase::SetUpCommandLine(command_line);
    SetUpCommandLineForKeySystem(kExternalClearKeyKeySystem, command_line);
    // The output protection tests create a MediaRecorder on a MediaStream,
    // so this allows for a fake stream to be created.
    command_line->AppendSwitch(switches::kUseFakeUIForMediaStream);
    command_line->AppendSwitchASCII(
        switches::kUseFakeDeviceForMediaStream,
        base::StringPrintf("display-media-type=%s", GetParam()));
  }
};

class ECKIncognitoEncryptedMediaTest : public EncryptedMediaTestBase {
 public:
  // We use special |key_system| names to do non-playback related tests,
  // e.g. kExternalClearKeyFileIOTestKeySystem is used to test file IO.
  void TestNonPlaybackCases(const std::string& key_system,
                            const std::string& expected_title) {
    // Since we do not test playback, arbitrarily choose a test file and source
    // type.
    RunEncryptedMediaTest(kDefaultEmePlayer, "bear-a_enc-a.webm", key_system,
                          SrcType::SRC, kNoSessionToLoad, false,
                          PlayCount::ONCE, expected_title);
  }

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    EncryptedMediaTestBase::SetUpCommandLine(command_line);
    SetUpCommandLineForKeySystem(kExternalClearKeyKeySystem, command_line);
    command_line->AppendSwitch(switches::kIncognito);
  }
};
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

// A base class for parameterized encrypted media tests. Subclasses must
// override `CurrentKeySystem()` and `CurrentSourceType()`.
class ParameterizedEncryptedMediaTestBase : public EncryptedMediaTestBase {
 public:
  virtual std::string CurrentKeySystem() = 0;
  virtual SrcType CurrentSourceType() = 0;

  void TestSimplePlayback(const std::string& encrypted_media) {
    RunSimpleEncryptedMediaTest(encrypted_media, CurrentKeySystem(),
                                CurrentSourceType());
  }

  void TestMultiplePlayback(const std::string& encrypted_media) {
    DCHECK(IsPlayBackPossible(CurrentKeySystem()));
    RunEncryptedMediaTest(kDefaultEmePlayer, encrypted_media,
                          CurrentKeySystem(), CurrentSourceType(),
                          kNoSessionToLoad, false, PlayCount::TWICE,
                          media::kEndedTitle);
  }

  void RunInvalidResponseTest() {
    RunEncryptedMediaTest(kDefaultEmePlayer, "bear-320x240-av_enc-av.webm",
                          CurrentKeySystem(), CurrentSourceType(),
                          kNoSessionToLoad, true, PlayCount::ONCE,
                          kEmeUpdateFailed);
  }

  void TestFrameSizeChange() {
    RunEncryptedMediaTest("encrypted_frame_size_change.html",
                          "frame_size_change-av_enc-v.webm", CurrentKeySystem(),
                          CurrentSourceType(), kNoSessionToLoad, false,
                          PlayCount::ONCE, media::kEndedTitle);
  }

  void TestConfigChange(ConfigChangeType config_change_type) {
    DCHECK_EQ(CurrentSourceType(), SrcType::MSE)
        << "Config change only happens when using MSE.";
    DCHECK(IsPlayBackPossible(CurrentKeySystem()))
        << "ConfigChange test requires video playback.";

    base::StringPairs query_params;
    query_params.emplace_back("keySystem", CurrentKeySystem());
    query_params.emplace_back(
        "configChangeType",
        base::NumberToString(static_cast<int>(config_change_type)));
    RunEncryptedMediaTestPage("mse_config_change.html", CurrentKeySystem(),
                              query_params, media::kEndedTitle);
  }

  void TestPolicyCheck() {
    base::StringPairs query_params;
    // We do not care about playback so choose an arbitrary media file.
    std::string media_file = "bear-a_enc-a.webm";
    query_params.emplace_back("mediaFile", media_file);
    query_params.emplace_back("mediaType",
                              media::GetMimeTypeForFile(media_file));
    if (CurrentSourceType() == SrcType::MSE)
      query_params.emplace_back("useMSE", "1");
    query_params.emplace_back("keySystem", CurrentKeySystem());
    query_params.emplace_back("policyCheck", "1");
    RunEncryptedMediaTestPage(kDefaultEmePlayer, CurrentKeySystem(),
                              query_params, kUnitTestSuccess);
  }

  void TestDifferentContainers(const std::string& video_media_file,
                               const std::string& audio_media_file) {
    DCHECK_EQ(CurrentSourceType(), SrcType::MSE);
    RunEncryptedMediaMultipleFileTest(CurrentKeySystem(), video_media_file,
                                      audio_media_file, media::kEndedTitle);
  }

  void DisableEncryptedMedia() {
    PrefService* pref_service = browser()->profile()->GetPrefs();
    pref_service->SetBoolean(prefs::kEnableEncryptedMedia, false);
  }

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    EncryptedMediaTestBase::SetUpCommandLine(command_line);
    SetUpCommandLineForKeySystem(CurrentKeySystem(), command_line);
  }
};

// Tests encrypted media playback with a combination of parameters:
// - char*: Key system name.
// - SrcType: Use MSE or SRC.
//
// Note:
// 1. Only parameterized (*_P) tests can be used. Non-parameterized (*_F)
// tests will crash at GetParam().
// 2. For key systems backed by library CDMs, the latest CDM interface version
// supported by both the CDM and Chromium will be used.
class EncryptedMediaTest
    : public ParameterizedEncryptedMediaTestBase,
      public testing::WithParamInterface<std::tuple<const char*, SrcType>> {
 public:
  std::string CurrentKeySystem() override { return std::get<0>(GetParam()); }
  SrcType CurrentSourceType() override { return std::get<1>(GetParam()); }
};

// Similar to EncryptedMediaTest, but the source type is always MSE. This is
// needed because many tests can only work with MSE (not with SRC), e.g.
// encrypted MP4, see http://crbug.com/170793. Use this class for those tests so
// we don't have to start the test and then skip it.
class MseEncryptedMediaTest : public ParameterizedEncryptedMediaTestBase,
                              public testing::WithParamInterface<const char*> {
 public:
  std::string CurrentKeySystem() override { return GetParam(); }
  SrcType CurrentSourceType() override { return SrcType::MSE; }
};

using ::testing::Combine;
using ::testing::Values;

INSTANTIATE_TEST_SUITE_P(MSE_ClearKey,
                         EncryptedMediaTest,
                         Combine(Values(kClearKeyKeySystem),
                                 Values(SrcType::MSE)));

INSTANTIATE_TEST_SUITE_P(MSE_ClearKey,
                         MseEncryptedMediaTest,
                         Values(kClearKeyKeySystem));

// External Clear Key is currently only used on platforms that use library CDMs.
#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
INSTANTIATE_TEST_SUITE_P(SRC_ExternalClearKey,
                         EncryptedMediaTest,
                         Combine(Values(kExternalClearKeyKeySystem),
                                 Values(SrcType::SRC)));

INSTANTIATE_TEST_SUITE_P(MSE_ExternalClearKey,
                         EncryptedMediaTest,
                         Combine(Values(kExternalClearKeyKeySystem),
                                 Values(SrcType::MSE)));

INSTANTIATE_TEST_SUITE_P(MSE_ExternalClearKey,
                         MseEncryptedMediaTest,
                         Values(kExternalClearKeyKeySystem));
#else   // BUILDFLAG(ENABLE_LIBRARY_CDMS)
// To reduce test time, only run ClearKey SRC tests when we are not running
// ExternalClearKey SRC tests.
INSTANTIATE_TEST_SUITE_P(SRC_ClearKey,
                         EncryptedMediaTest,
                         Combine(Values(kClearKeyKeySystem),
                                 Values(SrcType::SRC)));
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

#if BUILDFLAG(BUNDLE_WIDEVINE_CDM)
INSTANTIATE_TEST_SUITE_P(MSE_Widevine,
                         EncryptedMediaTest,
                         Combine(Values(kWidevineKeySystem),
                                 Values(SrcType::MSE)));

INSTANTIATE_TEST_SUITE_P(MSE_Widevine,
                         MseEncryptedMediaTest,
                         Values(kWidevineKeySystem));
#endif  // #if BUILDFLAG(BUNDLE_WIDEVINE_CDM)

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_AudioClearVideo_WebM) {
  TestSimplePlayback("bear-320x240-av_enc-a.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoAudio_WebM) {
  TestSimplePlayback("bear-320x240-av_enc-av.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoClearAudio_WebM) {
  TestSimplePlayback("bear-320x240-av_enc-v.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VP9Video_WebM_Fullsample) {
  TestSimplePlayback("bear-320x240-v-vp9_fullsample_enc-v.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VP9Video_WebM_Subsample) {
  TestSimplePlayback("bear-320x240-v-vp9_subsample_enc-v.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoAudio_WebM_Opus) {
  TestSimplePlayback("bear-320x240-opus-av_enc-av.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoClearAudio_WebM_Opus) {
  TestSimplePlayback("bear-320x240-opus-av_enc-v.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_Multiple_VideoAudio_WebM) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "Playback_Multiple test requires playback.";

  TestMultiplePlayback("bear-320x240-av_enc-av.webm");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_AudioOnly_MP4_FLAC) {
  TestSimplePlayback("bear-flac-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_AudioOnly_MP4_OPUS) {
  TestSimplePlayback("bear-opus-cenc.mp4");
}

// TODO(crbug.com/1045393): Flaky on multiple platforms.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       DISABLED_Playback_VideoOnly_MP4_VP9) {
  TestSimplePlayback("bear-320x240-v_frag-vp9-cenc.mp4");
}

#if BUILDFLAG(IS_MAC) || (BUILDFLAG(IS_FUCHSIA) && defined(ARCH_CPU_ARM_FAMILY))
// TODO(https://crbug.com/1250305): Fails on dcheck-enabled builds on 11.0.
// TODO(https://crbug.com/1280308): Fails on Fuchsia-arm64
#define MAYBE_Playback_VideoOnly_WebM_VP9Profile2 \
  DISABLED_Playback_VideoOnly_WebM_VP9Profile2
#else
#define MAYBE_Playback_VideoOnly_WebM_VP9Profile2 \
  Playback_VideoOnly_WebM_VP9Profile2
#endif
IN_PROC_BROWSER_TEST_P(EncryptedMediaTest,
                       MAYBE_Playback_VideoOnly_WebM_VP9Profile2) {
  TestSimplePlayback("bear-320x240-v-vp9_profile2_subsample_cenc-v.webm");
}

#if BUILDFLAG(IS_MAC) || (BUILDFLAG(IS_FUCHSIA) && defined(ARCH_CPU_ARM_FAMILY))
// TODO(https://crbug.com/1250305): Fails on dcheck-enabled builds on 11.0.
// TODO(https://crbug.com/1280308): Fails on Fuchsia-arm64
#define MAYBE_Playback_VideoOnly_MP4_VP9Profile2 \
  DISABLED_Playback_VideoOnly_MP4_VP9Profile2
#else
#define MAYBE_Playback_VideoOnly_MP4_VP9Profile2 \
  Playback_VideoOnly_MP4_VP9Profile2
#endif
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       MAYBE_Playback_VideoOnly_MP4_VP9Profile2) {
  TestSimplePlayback("bear-320x240-v-vp9_profile2_subsample_cenc-v.mp4");
}

#if BUILDFLAG(ENABLE_AV1_DECODER)

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoOnly_WebM_AV1) {
  TestSimplePlayback("bear-av1-cenc.webm");
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, Playback_VideoOnly_WebM_AV1_10bit) {
  TestSimplePlayback("bear-av1-320x180-10bit-cenc.webm");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_VideoOnly_MP4_AV1) {
  TestSimplePlayback("bear-av1-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_VideoOnly_MP4_AV1_10bit) {
  TestSimplePlayback("bear-av1-320x180-10bit-cenc.mp4");
}

#endif  // BUILDFLAG(ENABLE_AV1_DECODER)

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, InvalidResponseKeyError) {
  RunInvalidResponseTest();
}

// This is not really an "encrypted" media test. Keep it here for completeness.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, ConfigChangeVideo_ClearToClear) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "ConfigChange test requires video playback.";

  TestConfigChange(ConfigChangeType::CLEAR_TO_CLEAR);
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       ConfigChangeVideo_ClearToEncrypted) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "ConfigChange test requires video playback.";

  TestConfigChange(ConfigChangeType::CLEAR_TO_ENCRYPTED);
}

// TODO(crbug.com/1045376): Flaky on multiple platforms.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       DISABLED_ConfigChangeVideo_EncryptedToClear) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "ConfigChange test requires video playback.";

  TestConfigChange(ConfigChangeType::ENCRYPTED_TO_CLEAR);
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       ConfigChangeVideo_EncryptedToEncrypted) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "ConfigChange test requires video playback.";

  TestConfigChange(ConfigChangeType::ENCRYPTED_TO_ENCRYPTED);
}

IN_PROC_BROWSER_TEST_P(EncryptedMediaTest, FrameSizeChangeVideo) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "FrameSizeChange test requires video playback.";

  TestFrameSizeChange();
}

// Only use MSE since this is independent to the demuxer.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, PolicyCheck) {
  TestPolicyCheck();
}

// Only use MSE since this is independent to the demuxer.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, RemoveTemporarySession) {
  if (!IsPlayBackPossible(CurrentKeySystem()))
    GTEST_SKIP() << "RemoveTemporarySession test requires license server.";

  base::StringPairs query_params{{"keySystem", CurrentKeySystem()}};
  RunEncryptedMediaTestPage("eme_remove_session_test.html", CurrentKeySystem(),
                            query_params, media::kEndedTitle);
}

// Only use MSE since this is independent to the demuxer.
IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, EncryptedMediaDisabled) {
  DisableEncryptedMedia();

  // Clear Key key system is always supported.
  std::string expected_title = media::IsClearKey(CurrentKeySystem())
                                   ? media::kEndedTitle
                                   : kEmeNotSupportedError;

  RunEncryptedMediaTest(kDefaultEmePlayer, "bear-a_enc-a.webm",
                        CurrentKeySystem(), CurrentSourceType(),
                        kNoSessionToLoad, false, PlayCount::ONCE,
                        expected_title);
}

#if BUILDFLAG(USE_PROPRIETARY_CODECS)

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_VideoOnly_MP4) {
  TestSimplePlayback("bear-640x360-v_frag-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_VideoOnly_MP4_MDAT) {
  TestSimplePlayback("bear-640x360-v_frag-cenc-mdat.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest, Playback_Encryption_CBCS) {
  TestSimplePlayback("bear-640x360-v_frag-cbcs.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_EncryptedVideo_MP4_ClearAudio_WEBM) {
  TestDifferentContainers("bear-640x360-v_frag-cenc.mp4",
                          "bear-320x240-audio-only.webm");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_ClearVideo_WEBM_EncryptedAudio_MP4) {
  TestDifferentContainers("bear-320x240-video-only.webm",
                          "bear-640x360-a_frag-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_EncryptedVideo_WEBM_EncryptedAudio_MP4) {
  TestDifferentContainers("bear-320x240-v_enc-v.webm",
                          "bear-640x360-a_frag-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_EncryptedVideo_CBCS_EncryptedAudio_CENC) {
  TestDifferentContainers("bear-640x360-v_frag-cbcs.mp4",
                          "bear-640x360-a_frag-cenc.mp4");
}

IN_PROC_BROWSER_TEST_P(MseEncryptedMediaTest,
                       Playback_EncryptedVideo_CENC_EncryptedAudio_CBCS) {
  TestDifferentContainers("bear-640x360-v_frag-cenc.mp4",
                          "bear-640x360-a_frag-cbcs.mp4");
}

#endif  // BUILDFLAG(USE_PROPRIETARY_CODECS)

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)

// Test CDM_10 through CDM_11.
static_assert(media::CheckSupportedCdmInterfaceVersions(10, 11),
              "Mismatch between implementation and test coverage");
INSTANTIATE_TEST_SUITE_P(CDM_10, ECKEncryptedMediaTest, Values(10));
INSTANTIATE_TEST_SUITE_P(CDM_11, ECKEncryptedMediaTest, Values(11));

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, InitializeCDMFail) {
  TestNonPlaybackCases(kExternalClearKeyInitializeFailKeySystem,
                       kEmeNotSupportedError);
}

// TODO(1019187): Failing on win7.
#if BUILDFLAG(IS_WIN)
#define MAYBE_CDMCrashDuringDecode DISABLED_CDMCrashDuringDecode
#else
#define MAYBE_CDMCrashDuringDecode CDMCrashDuringDecode
#endif
// When CDM crashes, we should still get a decode error and all sessions should
// be closed.
IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, MAYBE_CDMCrashDuringDecode) {
  TestNonPlaybackCases(kExternalClearKeyCrashKeySystem,
                       kEmeSessionClosedAndError);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, FileIOTest) {
  TestNonPlaybackCases(kExternalClearKeyFileIOTestKeySystem, kUnitTestSuccess);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, PlatformVerificationTest) {
  TestNonPlaybackCases(kExternalClearKeyPlatformVerificationTestKeySystem,
                       kUnitTestSuccess);
}

// Intermittent leaks on ASan/LSan runs: crbug.com/889923
#if defined(LEAK_SANITIZER) || defined(ADDRESS_SANITIZER)
#define MAYBE_MessageTypeTest DISABLED_MessageTypeTest
#else
#define MAYBE_MessageTypeTest MessageTypeTest
#endif
IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, MAYBE_MessageTypeTest) {
  TestPlaybackCase(kExternalClearKeyMessageTypeTestKeySystem, kNoSessionToLoad,
                   media::kEndedTitle);

  int num_received_message_types = 0;
  EXPECT_TRUE(content::ExecuteScriptAndExtractInt(
      browser()->tab_strip_model()->GetActiveWebContents(),
      "window.domAutomationController.send("
      "document.querySelector('video').receivedMessageTypes.size);",
      &num_received_message_types));

  // Expects 3 message types: 'license-request', 'license-renewal' and
  // 'individualization-request'.
  EXPECT_EQ(3, num_received_message_types);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, LoadPersistentLicense) {
  TestPlaybackCase(kExternalClearKeyKeySystem, kPersistentLicense,
                   media::kEndedTitle);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, LoadUnknownSession) {
  TestPlaybackCase(kExternalClearKeyKeySystem, kUnknownSession,
                   kEmeSessionNotFound);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, LoadSessionAfterClose) {
  base::StringPairs query_params{{"keySystem", kExternalClearKeyKeySystem}};
  RunEncryptedMediaTestPage("eme_load_session_after_close_test.html",
                            kExternalClearKeyKeySystem, query_params,
                            media::kEndedTitle);
}

const char kExternalClearKeyDecryptOnlyKeySystem[] =
    "org.chromium.externalclearkey.decryptonly";

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, DecryptOnly_VideoAudio_WebM) {
  RunSimpleEncryptedMediaTest("bear-320x240-av_enc-av.webm",
                              kExternalClearKeyDecryptOnlyKeySystem,
                              SrcType::MSE);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, DecryptOnly_VideoOnly_MP4_VP9) {
  RunSimpleEncryptedMediaTest("bear-320x240-v_frag-vp9-cenc.mp4",
                              kExternalClearKeyDecryptOnlyKeySystem,
                              SrcType::MSE);
}

#if BUILDFLAG(USE_PROPRIETARY_CODECS)

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, DecryptOnly_VideoOnly_MP4_CBCS) {
  // 'cbcs' decryption is only supported on CDM 10 or later as long as
  // the appropriate buildflag is enabled.
  std::string expected_result =
      GetCdmInterfaceVersion() >= 10 ? media::kEndedTitle : media::kErrorTitle;
  RunEncryptedMediaTest(kDefaultEmePlayer, "bear-640x360-v_frag-cbcs.mp4",
                        kExternalClearKeyDecryptOnlyKeySystem, SrcType::MSE,
                        kNoSessionToLoad, false, PlayCount::ONCE,
                        expected_result);
}

// Encryption Scheme tests. ClearKey key system is covered in
// content/browser/media/encrypted_media_browsertest.cc.
IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, Playback_Encryption_CENC) {
  RunEncryptedMediaMultipleFileTest(
      kExternalClearKeyKeySystem, "bear-640x360-v_frag-cenc.mp4",
      "bear-640x360-a_frag-cenc.mp4", media::kEndedTitle);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, Playback_Encryption_CBC1) {
  RunEncryptedMediaMultipleFileTest(kExternalClearKeyKeySystem,
                                    "bear-640x360-v_frag-cbc1.mp4",
                                    std::string(), media::kErrorTitle);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, Playback_Encryption_CENS) {
  RunEncryptedMediaMultipleFileTest(kExternalClearKeyKeySystem,
                                    "bear-640x360-v_frag-cens.mp4",
                                    std::string(), media::kErrorTitle);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, Playback_Encryption_CBCS) {
  // 'cbcs' decryption is only supported on CDM 10 or later as long as
  // the appropriate buildflag is enabled.
  std::string expected_result =
      GetCdmInterfaceVersion() >= 10 ? media::kEndedTitle : media::kErrorTitle;
  RunEncryptedMediaMultipleFileTest(
      kExternalClearKeyKeySystem, "bear-640x360-v_frag-cbcs.mp4",
      "bear-640x360-a_frag-cbcs.mp4", expected_result);
}

#endif  // BUILDFLAG(USE_PROPRIETARY_CODECS)

#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, VerifyCdmHostTest) {
  TestNonPlaybackCases(kExternalClearKeyVerifyCdmHostTestKeySystem,
                       kUnitTestSuccess);
}
#endif  // BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, StorageIdTest) {
  TestNonPlaybackCases(kExternalClearKeyStorageIdTestKeySystem,
                       kUnitTestSuccess);
}

// TODO(crbug.com/902310): Times out in debug builds.
#ifdef NDEBUG
#define MAYBE_MultipleCdmTypes MultipeCdmTypes
#else
#define MAYBE_MultipleCdmTypes DISABLED_MultipeCdmTypes
#endif
IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaTest, MAYBE_MultipleCdmTypes) {
  base::StringPairs empty_query_params;
  RunMediaTestPage("multiple_cdm_types.html", empty_query_params,
                   media::kEndedTitle, true);
}

// Output Protection Tests. Run with different capture inputs. "monitor"
// simulates the whole screen being captured. "window" simulates the Chrome
// window being captured. "browser" simulates the current Chrome tab being
// captured.

INSTANTIATE_TEST_SUITE_P(Capture_Monitor,
                         ECKEncryptedMediaOutputProtectionTest,
                         Values("monitor"));
INSTANTIATE_TEST_SUITE_P(Capture_Window,
                         ECKEncryptedMediaOutputProtectionTest,
                         Values("window"));
INSTANTIATE_TEST_SUITE_P(Capture_Browser,
                         ECKEncryptedMediaOutputProtectionTest,
                         Values("browser"));

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaOutputProtectionTest, BeforeMediaKeys) {
  TestOutputProtection(/*create_recorder_before_media_keys=*/true);
}

IN_PROC_BROWSER_TEST_P(ECKEncryptedMediaOutputProtectionTest, AfterMediaKeys) {
  TestOutputProtection(/*create_recorder_before_media_keys=*/false);
}

// Incognito tests. Ideally we would run all above tests in incognito mode to
// ensure that everything works. However, that would add a lot of extra tests
// that aren't really testing anything different, as normal playback does not
// save anything to disk. Instead we are only running the tests that actually
// have the CDM do file access.

IN_PROC_BROWSER_TEST_F(ECKIncognitoEncryptedMediaTest, FileIO) {
  // Try the FileIO test using the default CDM API while running in incognito.
  TestNonPlaybackCases(kExternalClearKeyFileIOTestKeySystem, kUnitTestSuccess);
}

IN_PROC_BROWSER_TEST_F(ECKIncognitoEncryptedMediaTest, LoadSessionAfterClose) {
  // Loading a session should work in incognito mode.
  base::StringPairs query_params{{"keySystem", kExternalClearKeyKeySystem}};
  RunEncryptedMediaTestPage("eme_load_session_after_close_test.html",
                            kExternalClearKeyKeySystem, query_params,
                            media::kEndedTitle);
}
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)
