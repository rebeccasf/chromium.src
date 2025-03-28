// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/component_updater/registration.h"

#include "base/callback.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/metrics/histogram_functions.h"
#include "base/path_service.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/buildflags.h"
#include "chrome/browser/component_updater/app_provisioning_component_installer.h"
#include "chrome/browser/component_updater/autofill_regex_remover.h"
#include "chrome/browser/component_updater/chrome_client_side_phishing_component_installer.h"
#include "chrome/browser/component_updater/chrome_origin_trials_component_installer.h"
#include "chrome/browser/component_updater/crl_set_component_installer.h"
#include "chrome/browser/component_updater/crowd_deny_component_installer.h"
#include "chrome/browser/component_updater/file_type_policies_component_installer.h"
#include "chrome/browser/component_updater/first_party_sets_component_installer.h"
#include "chrome/browser/component_updater/hyphenation_component_installer.h"
#include "chrome/browser/component_updater/mei_preload_component_installer.h"
#include "chrome/browser/component_updater/pki_metadata_component_installer.h"
#include "chrome/browser/component_updater/ssl_error_assistant_component_installer.h"
#include "chrome/browser/component_updater/subresource_filter_component_installer.h"
#include "chrome/browser/component_updater/trust_token_key_commitments_component_installer.h"
#include "chrome/browser/component_updater/url_param_classification_component_installer.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/chrome_paths.h"
#include "components/component_updater/component_updater_service.h"
#include "components/component_updater/crl_set_remover.h"
#include "components/component_updater/installer_policies/autofill_states_component_installer.h"
#include "components/component_updater/installer_policies/on_device_head_suggest_component_installer.h"
#include "components/component_updater/installer_policies/optimization_hints_component_installer.h"
#include "components/component_updater/installer_policies/safety_tips_component_installer.h"
#include "components/nacl/common/buildflags.h"
#include "device/vr/buildflags/buildflags.h"
#include "ppapi/buildflags/buildflags.h"
#include "third_party/widevine/cdm/buildflags.h"

#if BUILDFLAG(IS_WIN)
#include "chrome/browser/component_updater/sw_reporter_installer_win.h"
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
#include "chrome/browser/component_updater/third_party_module_list_component_installer_win.h"
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING)
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/component_updater/recovery_component_installer.h"
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#include "chrome/browser/component_updater/recovery_improved_component_installer.h"
#endif  // BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_ANDROID)
#include "chrome/browser/component_updater/desktop_sharing_hub_component_remover.h"
#endif  // BUILDFLAG(IS_ANDROID)

#if !BUILDFLAG(IS_ANDROID)
#include "chrome/browser/component_updater/commerce_heuristics_component_installer.h"
#include "chrome/browser/component_updater/desktop_screenshot_editor_component_installer.h"
#include "chrome/browser/component_updater/desktop_sharing_hub_component_installer.h"
#include "chrome/browser/component_updater/zxcvbn_data_component_installer.h"
#include "chrome/browser/resource_coordinator/tab_manager.h"
#include "media/base/media_switches.h"
#endif  // !BUILDFLAG(IS_ANDROID)

#if BUILDFLAG(IS_CHROMEOS_ASH)
#include "chrome/browser/component_updater/smart_dim_component_installer.h"
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(ENABLE_NACL)
#include "chrome/browser/component_updater/pnacl_component_installer.h"
#endif  // BUILDFLAG(ENABLE_NACL)

#if BUILDFLAG(ENABLE_VR)
#include "chrome/browser/component_updater/vr_assets_component_installer.h"
#endif

#if BUILDFLAG(ENABLE_MEDIA_FOUNDATION_WIDEVINE_CDM)
#include "chrome/browser/component_updater/media_foundation_widevine_cdm_component_installer.h"
#endif

#if BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)
#include "chrome/browser/component_updater/widevine_cdm_component_installer.h"
#endif  // BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)

#if BUILDFLAG(IS_LINUX)
#include "chrome/browser/component_updater/screen_ai_component_installer.h"
#endif  // BUILDFLAG(IS_LINUX)

#if defined(USE_AURA)
#include "ui/aura/env.h"
#endif

namespace component_updater {

void RegisterComponentsForUpdate() {

#if 0
#if BUILDFLAG(IS_WIN)
  RegisterRecoveryImprovedComponent(cus, g_browser_process->local_state());
#endif  // BUILDFLAG(IS_WIN)
#endif  // 0

#if BUILDFLAG(IS_MAC)
  auto* const cus = g_browser_process->component_updater();
  RegisterRecoveryImprovedComponent(cus, g_browser_process->local_state());
  RegisterRecoveryComponent(cus, g_browser_process->local_state());
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(ENABLE_MEDIA_FOUNDATION_WIDEVINE_CDM)
  RegisterMediaFoundationWidevineCdmComponent(cus);
#endif

#if BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)
  //RegisterWidevineCdmComponent(cus);
#endif  // BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)

#if 0
#if BUILDFLAG(ENABLE_NACL) && !BUILDFLAG(IS_ANDROID)
#if BUILDFLAG(IS_CHROMEOS_ASH)
  // PNaCl on Chrome OS is on rootfs and there is no need to download it. But
  // Chrome4ChromeOS on Linux doesn't contain PNaCl so enable component
  // installer when running on Linux. See crbug.com/422121 for more details.
  if (!base::SysInfo::IsRunningOnChromeOS()) {
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)
    RegisterPnaclComponent(cus);
#if BUILDFLAG(IS_CHROMEOS_ASH)
  }
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)
#endif  // BUILDFLAG(ENABLE_NACL) && !BUILDFLAG(IS_ANDROID)

  RegisterSubresourceFilterComponent(cus);
  RegisterOnDeviceHeadSuggestComponent(
      cus, g_browser_process->GetApplicationLocale());
  RegisterOptimizationHintsComponent(cus);
  RegisterTrustTokenKeyCommitmentsComponentIfTrustTokensEnabled(cus);
  RegisterFirstPartySetsComponent(cus);

  base::FilePath path;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &path)) {
    // The CRLSet component previously resided in a different location: delete
    // the old file.
    component_updater::DeleteLegacyCRLSet(path);

    component_updater::DeleteAutofillRegex(path);

#if BUILDFLAG(IS_ANDROID)
    // Clean up any desktop sharing hubs that were installed on Android.
    component_updater::DeleteDesktopSharingHub(path);
#endif  // BUILDFLAG(IS_ANDROID)
  }
  RegisterSSLErrorAssistantComponent(cus);

  // Since file type policies are per-platform, and we don't support
  // Fuchsia-specific component versions, we don't dynamically update file type
  // policies on Fuchsia.
#if !BUILDFLAG(IS_FUCHSIA)
  RegisterFileTypePoliciesComponent(cus);
#endif

#if !BUILDFLAG(IS_CHROMEOS_ASH)
  // CRLSetFetcher attempts to load a CRL set from either the local disk or
  // network.
  // For Chrome OS this registration is delayed until user login.
  component_updater::RegisterCRLSetComponent(cus);
#endif  // !BUILDFLAG(IS_CHROMEOS_ASH)

  RegisterOriginTrialsComponent(cus);
  RegisterMediaEngagementPreloadComponent(cus, base::OnceClosure());

#if BUILDFLAG(IS_WIN)
  // SwReporter is only needed for official builds.  However, to enable testing
  // on chromium build bots, it is always registered here and
  // RegisterSwReporterComponent() has support for running only in official
  // builds or tests.
  RegisterSwReporterComponent(cus);
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
  RegisterThirdPartyModuleListComponent(cus);
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING)
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(ENABLE_VR)
  if (component_updater::ShouldRegisterVrAssetsComponentOnStartup()) {
    component_updater::RegisterVrAssetsComponent(cus);
  }
#endif

  MaybeRegisterPKIMetadataComponent(cus);

  RegisterSafetyTipsComponent(cus);
  RegisterCrowdDenyComponent(cus);

#if BUILDFLAG(IS_CHROMEOS_ASH)
  RegisterSmartDimComponent(cus);
  RegisterAppProvisioningComponent(cus);
#endif  // !BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(USE_MINIKIN_HYPHENATION) && !BUILDFLAG(IS_ANDROID)
  RegisterHyphenationComponent(cus);
#endif

#if !BUILDFLAG(IS_ANDROID)
  RegisterDesktopScreenshotEditorComponent(cus);
  RegisterDesktopSharingHubComponent(cus);
  RegisterZxcvbnDataComponent(cus);
  RegisterCommerceHeuristicsComponent(cus);
#endif  // !BUILDFLAG(IS_ANDROID)

  RegisterAutofillStatesComponent(cus, g_browser_process->local_state());

  RegisterClientSidePhishingComponent(cus);

#if BUILDFLAG(IS_LINUX)
  RegisterScreenAIComponent(cus, g_browser_process->local_state());
#endif  // BUILDFLAG(IS_LINUX)

  RegisterUrlParamClassificationComponent(cus);
#endif // disable component updater
}

}  // namespace component_updater
