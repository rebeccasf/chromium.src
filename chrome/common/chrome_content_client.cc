// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/nw/src/browser/nw_extensions_browser_hooks.h"
#include "chrome/common/chrome_content_client.h"

#include "components/crash/core/app/crash_reporter_client.h"
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "base/bind.h"
#include "base/containers/flat_set.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/child_process_host_flags.h"
#include "chrome/common/child_process_logging.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/crash_keys.h"
#include "chrome/common/media/cdm_registration.h"
#include "chrome/common/url_constants.h"
#include "chrome/grit/common_resources.h"
#include "chrome/grit/generated_resources.h"
#include "components/crash/core/common/crash_key.h"
#include "components/dom_distiller/core/url_constants.h"
#include "components/embedder_support/origin_trials/origin_trial_policy_impl.h"
#include "components/services/heap_profiling/public/cpp/profiling_client.h"
#include "components/strings/grit/components_strings.h"
#include "content/public/common/cdm_info.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "extensions/buildflags/buildflags.h"
#include "gpu/config/gpu_info.h"
#include "gpu/config/gpu_util.h"
#include "media/media_buildflags.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "net/http/http_util.h"
#include "pdf/buildflags.h"
#include "ppapi/buildflags/buildflags.h"
#include "third_party/widevine/cdm/buildflags.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/layout.h"
#include "ui/base/resource/resource_bundle.h"
#include "url/url_constants.h"

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include <fcntl.h>
#include "sandbox/linux/services/credentials.h"
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(IS_WIN)
#include "base/win/windows_version.h"
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/common/constants.h"
#endif

#if BUILDFLAG(ENABLE_NACL)
#include "components/nacl/common/nacl_constants.h"
#include "components/nacl/common/nacl_process_type.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "content/public/common/pepper_plugin_info.h"
#include "ppapi/shared_impl/ppapi_permissions.h"  // nogncheck
#endif

#if BUILDFLAG(ENABLE_PDF)
#include "components/pdf/common/internal_plugin_helpers.h"
#endif

#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
#include "chrome/common/media/cdm_host_file_path.h"
#endif

#if BUILDFLAG(IS_ANDROID)
#include "chrome/common/media/chrome_media_drm_bridge_client.h"
#endif

#include "content/nw/src/common/nw_content_common_hooks.h"

namespace {

#if BUILDFLAG(ENABLE_PLUGINS)
#if BUILDFLAG(ENABLE_PDF)
const char kPDFPluginExtension[] = "pdf";
const char kPDFPluginDescription[] = "Portable Document Format";
#endif  // BUILDFLAG(ENABLE_PDF)

#if BUILDFLAG(ENABLE_NACL)
content::PepperPluginInfo::GetInterfaceFunc g_nacl_get_interface;
content::PepperPluginInfo::PPP_InitializeModuleFunc g_nacl_initialize_module;
content::PepperPluginInfo::PPP_ShutdownModuleFunc g_nacl_shutdown_module;
#endif

// Appends the known built-in plugins to the given vector. Some built-in
// plugins are "internal" which means they are compiled into the Chrome binary,
// and some are extra shared libraries distributed with the browser (these are
// not marked internal, aside from being automatically registered, they're just
// regular plugins).
void ComputeBuiltInPlugins(std::vector<content::PepperPluginInfo>* plugins) {
#if BUILDFLAG(ENABLE_PDF)
  // TODO(thestig): Figure out how to make the PDF Viewer work without this
  // PPAPI plugin registration.
  content::PepperPluginInfo pdf_info;
  pdf_info.is_internal = true;
  pdf_info.is_out_of_process = true;
  pdf_info.name = ChromeContentClient::kPDFInternalPluginName;
  pdf_info.description = kPDFPluginDescription;
  pdf_info.path = base::FilePath(ChromeContentClient::kPDFPluginPath);
  content::WebPluginMimeType pdf_mime_type(
      pdf::kInternalPluginMimeType, kPDFPluginExtension, kPDFPluginDescription);
  pdf_info.mime_types.push_back(pdf_mime_type);
  plugins->push_back(pdf_info);
#endif  // BUILDFLAG(ENABLE_PDF)

#if BUILDFLAG(ENABLE_NACL)
  // Handle Native Client just like the PDF plugin. This means that it is
  // enabled by default for the non-portable case.  This allows apps installed
  // from the Chrome Web Store to use NaCl even if the command line switch
  // isn't set.  For other uses of NaCl we check for the command line switch.
  content::PepperPluginInfo nacl;
  // The nacl plugin is now built into the Chromium binary.
  nacl.is_internal = true;
  nacl.path = base::FilePath(ChromeContentClient::kNaClPluginFileName);
  nacl.name = nacl::kNaClPluginName;
  content::WebPluginMimeType nacl_mime_type(nacl::kNaClPluginMimeType,
                                            nacl::kNaClPluginExtension,
                                            nacl::kNaClPluginDescription);
  nacl.mime_types.push_back(nacl_mime_type);
  content::WebPluginMimeType pnacl_mime_type(nacl::kPnaclPluginMimeType,
                                             nacl::kPnaclPluginExtension,
                                             nacl::kPnaclPluginDescription);
  nacl.mime_types.push_back(pnacl_mime_type);
  nacl.internal_entry_points.get_interface = g_nacl_get_interface;
  nacl.internal_entry_points.initialize_module = g_nacl_initialize_module;
  nacl.internal_entry_points.shutdown_module = g_nacl_shutdown_module;
  nacl.permissions = ppapi::PERMISSION_PRIVATE | ppapi::PERMISSION_DEV;
  plugins->push_back(nacl);
#endif  // BUILDFLAG(ENABLE_NACL)
}
#endif  //  BUILDFLAG(ENABLE_PLUGINS)

}  // namespace

ChromeContentClient::ChromeContentClient() {
}

ChromeContentClient::~ChromeContentClient() {
}

void ChromeContentClient::LoadNWAppAsExtension(base::DictionaryValue* manifest,
                                               const base::FilePath& path,
                                               std::string* error) {
  nw::LoadNWAppAsExtensionHook(manifest, path, error);
}

#if BUILDFLAG(ENABLE_NACL)
void ChromeContentClient::SetNaClEntryFunctions(
    content::PepperPluginInfo::GetInterfaceFunc get_interface,
    content::PepperPluginInfo::PPP_InitializeModuleFunc initialize_module,
    content::PepperPluginInfo::PPP_ShutdownModuleFunc shutdown_module) {
  g_nacl_get_interface = get_interface;
  g_nacl_initialize_module = initialize_module;
  g_nacl_shutdown_module = shutdown_module;
}
#endif

void ChromeContentClient::SetActiveURL(const GURL& url,
                                       std::string top_origin) {
  static crash_reporter::CrashKeyString<1024> active_url("url-chunk");
  active_url.Set(url.possibly_invalid_spec());

  // Use a large enough size for Origin::GetDebugString.
  static crash_reporter::CrashKeyString<128> top_origin_key("top-origin");
  top_origin_key.Set(top_origin);
}

void ChromeContentClient::SetNWReportURL(const GURL& url) {
  static crash_reporter::CrashKeyString<1024> nwjs_url("url-nwjs");
  nwjs_url.Set(url.possibly_invalid_spec());
}

void ChromeContentClient::SetGpuInfo(const gpu::GPUInfo& gpu_info) {
  gpu::SetKeysForCrashLogging(gpu_info);
}

#if BUILDFLAG(ENABLE_PLUGINS)
// static
content::PepperPluginInfo* ChromeContentClient::FindMostRecentPlugin(
    const std::vector<std::unique_ptr<content::PepperPluginInfo>>& plugins) {
  if (plugins.empty())
    return nullptr;

  using PluginSortKey = std::tuple<base::Version, bool>;

  std::map<PluginSortKey, content::PepperPluginInfo*> plugin_map;

  for (auto& plugin : plugins) {
    base::Version version(plugin->version);
    DCHECK(version.IsValid());
    plugin_map[PluginSortKey(version, plugin->is_external)] = plugin.get();
  }

  return plugin_map.rbegin()->second;
}
#endif  // BUILDFLAG(ENABLE_PLUGINS)

void ChromeContentClient::AddPepperPlugins(
    std::vector<content::PepperPluginInfo>* plugins) {
#if BUILDFLAG(ENABLE_PLUGINS)
  ComputeBuiltInPlugins(plugins);
#endif  // BUILDFLAG(ENABLE_PLUGINS)
}

void ChromeContentClient::AddContentDecryptionModules(
    std::vector<content::CdmInfo>* cdms,
    std::vector<media::CdmHostFilePath>* cdm_host_file_paths) {
  if (cdms)
    RegisterCdmInfo(cdms);

#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
  if (cdm_host_file_paths)
    AddCdmHostFilePaths(cdm_host_file_paths);
#endif
}

// New schemes by which content can be retrieved should almost certainly be
// marked as "standard" schemes, even if they're internal, chrome-only schemes.
// "Standard" here just means that its URLs behave like 'normal' URL do.
//   - Standard schemes get canonicalized like "new-scheme://hostname/[path]"
//   - Whereas "new-scheme:hostname" is a valid nonstandard URL.
//   - Thus, hostnames can't be extracted from non-standard schemes.
//   - The presence of hostnames enables the same-origin policy. Resources like
//     "new-scheme://foo/" are kept separate from "new-scheme://bar/". For
//     a nonstandard scheme, every resource loaded from that scheme could
//     have access to every other resource.
//   - The same-origin policy is very important if webpages can be
//     loaded via the scheme. Try to organize the URL space of any new scheme
//     such that hostnames provide meaningful compartmentalization of
//     privileges.
//
// Example standard schemes: https://, chrome-extension://, chrome://, file://
// Example nonstandard schemes: mailto:, data:, javascript:, about:
static const char* const kChromeStandardURLSchemes[] = {
#if BUILDFLAG(ENABLE_EXTENSIONS)
    extensions::kExtensionScheme,
#endif
    chrome::kChromeNativeScheme,        chrome::kChromeSearchScheme,
    dom_distiller::kDomDistillerScheme,
#if BUILDFLAG(IS_ANDROID)
    content::kAndroidAppScheme,
#endif
};

void ChromeContentClient::AddAdditionalSchemes(Schemes* schemes) {
  for (auto* standard_scheme : kChromeStandardURLSchemes)
    schemes->standard_schemes.push_back(standard_scheme);

#if BUILDFLAG(IS_ANDROID)
  schemes->referrer_schemes.push_back(content::kAndroidAppScheme);
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
  schemes->extension_schemes.push_back(extensions::kExtensionScheme);
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
  schemes->savable_schemes.push_back(extensions::kExtensionScheme);
#endif
  schemes->savable_schemes.push_back(chrome::kChromeSearchScheme);
  schemes->savable_schemes.push_back(dom_distiller::kDomDistillerScheme);

  // chrome-search: resources shouldn't trigger insecure content warnings.
  schemes->secure_schemes.push_back(chrome::kChromeSearchScheme);

#if BUILDFLAG(ENABLE_EXTENSIONS)
  // Treat extensions as secure because communication with them is entirely in
  // the browser, so there is no danger of manipulation or eavesdropping on
  // communication with them by third parties.
  schemes->secure_schemes.push_back(extensions::kExtensionScheme);
#endif

  // chrome-native: is a scheme used for placeholder navigations that allow
  // UIs to be drawn with platform native widgets instead of HTML.  These pages
  // should be treated as empty documents that can commit synchronously.
  schemes->empty_document_schemes.push_back(chrome::kChromeNativeScheme);
  schemes->no_access_schemes.push_back(chrome::kChromeNativeScheme);
  schemes->secure_schemes.push_back(chrome::kChromeNativeScheme);

#if BUILDFLAG(ENABLE_EXTENSIONS)
  schemes->service_worker_schemes.push_back(extensions::kExtensionScheme);
  schemes->service_worker_schemes.push_back(url::kFileScheme);

  // As far as Blink is concerned, they should be allowed to receive CORS
  // requests. At the Extensions layer, requests will actually be blocked unless
  // overridden by the web_accessible_resources manifest key.
  // TODO(kalman): See what happens with a service worker.
  schemes->cors_enabled_schemes.push_back(extensions::kExtensionScheme);

  schemes->csp_bypassing_schemes.push_back(extensions::kExtensionScheme);
#endif

#if BUILDFLAG(IS_CHROMEOS_ASH)
  schemes->local_schemes.push_back(content::kExternalFileScheme);
#endif

#if BUILDFLAG(IS_ANDROID)
  schemes->local_schemes.push_back(url::kContentScheme);
#endif
}

std::u16string ChromeContentClient::GetLocalizedString(int message_id) {
  return l10n_util::GetStringUTF16(message_id);
}

std::u16string ChromeContentClient::GetLocalizedString(
    int message_id,
    const std::u16string& replacement) {
  return l10n_util::GetStringFUTF16(message_id, replacement);
}

base::StringPiece ChromeContentClient::GetDataResource(
    int resource_id,
    ui::ResourceScaleFactor scale_factor) {
  return ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
      resource_id, scale_factor);
}

base::RefCountedMemory* ChromeContentClient::GetDataResourceBytes(
    int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
      resource_id);
}

std::string ChromeContentClient::GetDataResourceString(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
      resource_id);
}

gfx::Image& ChromeContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(
      resource_id);
}

#if BUILDFLAG(IS_MAC)
base::FilePath ChromeContentClient::GetChildProcessPath(
    int child_flags,
    const base::FilePath& helpers_path) {
  std::string helper_name(chrome::kHelperProcessExecutableName);
  if (child_flags == chrome::kChildProcessHelperAlerts) {
    helper_name += chrome::kMacHelperSuffixAlerts;
    return helpers_path.Append(helper_name + ".app")
        .Append("Contents")
        .Append("MacOS")
        .Append(helper_name);
  }
  NOTREACHED() << "Unsupported child process flags!";
  return {};
}
#endif  // BUILDFLAG(IS_MAC)

std::string ChromeContentClient::GetProcessTypeNameInEnglish(int type) {
#if BUILDFLAG(ENABLE_NACL)
  switch (type) {
    case PROCESS_TYPE_NACL_LOADER:
      return "Native Client module";
    case PROCESS_TYPE_NACL_BROKER:
      return "Native Client broker";
  }
#endif

  NOTREACHED() << "Unknown child process type!";
  return "Unknown";
}

blink::OriginTrialPolicy* ChromeContentClient::GetOriginTrialPolicy() {
  // Prevent initialization race (see crbug.com/721144). There may be a
  // race when the policy is needed for worker startup (which happens on a
  // separate worker thread).
  base::AutoLock auto_lock(origin_trial_policy_lock_);
  if (!origin_trial_policy_)
    origin_trial_policy_ =
        std::make_unique<embedder_support::OriginTrialPolicyImpl>();
  return origin_trial_policy_.get();
}

#if BUILDFLAG(IS_ANDROID)
media::MediaDrmBridgeClient* ChromeContentClient::GetMediaDrmBridgeClient() {
  return new ChromeMediaDrmBridgeClient();
}
#endif  // BUILDFLAG(IS_ANDROID)

void ChromeContentClient::ExposeInterfacesToBrowser(
    scoped_refptr<base::SequencedTaskRunner> io_task_runner,
    mojo::BinderMap* binders) {
  binders->Add(
      base::BindRepeating(
          [](mojo::PendingReceiver<heap_profiling::mojom::ProfilingClient>
                 receiver) {
            static base::NoDestructor<heap_profiling::ProfilingClient>
                profiling_client;
            profiling_client->BindToInterface(std::move(receiver));
          }),
      io_task_runner);
}
