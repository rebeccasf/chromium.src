// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implements the Chrome Extensions Tab Capture API.

#include "chrome/browser/extensions/api/tab_capture/tab_capture_api.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/cxx17_backports.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/extensions/api/tab_capture/tab_capture_registry.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/features/feature.h"
#include "extensions/common/features/feature_provider.h"
#include "extensions/common/features/simple_feature.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/switches.h"
#include "net/base/url_util.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"

using content::DesktopMediaID;
using content::WebContentsMediaCaptureId;
using extensions::api::tab_capture::MediaStreamConstraint;

namespace TabCapture = extensions::api::tab_capture;
namespace GetCapturedTabs = TabCapture::GetCapturedTabs;

namespace extensions {
namespace {

const char kCapturingSameTab[] = "Cannot capture a tab with an active stream.";
const char kFindingTabError[] = "Error finding tab to capture.";
const char kNoAudioOrVideo[] = "Capture failed. No audio or video requested.";
const char kGrantError[] =
    "Extension has not been invoked for the current page (see activeTab "
    "permission). Chrome pages cannot be captured.";

const char kInvalidOriginError[] = "Caller tab.url is not a valid URL.";
const char kInvalidTabIdError[] = "Invalid tab specified.";
const char kTabUrlNotSecure[] =
    "URL scheme for the specified tab is not secure.";

// Keys/values passed to renderer-side JS bindings.
const char kMediaStreamSource[] = "chromeMediaSource";
const char kMediaStreamSourceId[] = "chromeMediaSourceId";
const char kMediaStreamSourceTab[] = "tab";

bool OptionsSpecifyAudioOrVideo(const TabCapture::CaptureOptions& options) {
  return (options.audio && *options.audio) || (options.video && *options.video);
}

DesktopMediaID BuildDesktopMediaID(content::WebContents* target_contents,
                                   TabCapture::CaptureOptions* options) {
  content::RenderFrameHost* const target_frame =
      target_contents->GetMainFrame();
  DesktopMediaID source(
      DesktopMediaID::TYPE_WEB_CONTENTS, DesktopMediaID::kNullId,
      WebContentsMediaCaptureId(target_frame->GetProcess()->GetID(),
                                target_frame->GetRoutingID()));
  return source;
}

// Add Chrome-specific source identifiers to the MediaStreamConstraints objects
// in |options| to provide references to the |target_contents| to be captured.
void AddMediaStreamSourceConstraints(content::WebContents* target_contents,
                                     TabCapture::CaptureOptions* options,
                                     const std::string& device_id) {
  DCHECK(options);
  DCHECK(target_contents);

  MediaStreamConstraint* constraints_to_modify[2] = {nullptr, nullptr};

  if (options->audio && *options->audio) {
    if (!options->audio_constraints)
      options->audio_constraints = std::make_unique<MediaStreamConstraint>();
    constraints_to_modify[0] = options->audio_constraints.get();
  }

  if (options->video && *options->video) {
    if (!options->video_constraints)
      options->video_constraints = std::make_unique<MediaStreamConstraint>();
    constraints_to_modify[1] = options->video_constraints.get();
  }

  // Append chrome specific tab constraints.
  for (MediaStreamConstraint* msc : constraints_to_modify) {
    if (!msc)
      continue;
    base::DictionaryValue* constraint = &msc->mandatory.additional_properties;
    constraint->SetStringKey(kMediaStreamSource, kMediaStreamSourceTab);
    constraint->SetStringKey(kMediaStreamSourceId, device_id);
  }
}

// Find the last-active browser that matches a profile this ExtensionFunction
// can access.  We can't use FindLastActiveWithProfile() because we may want to
// include incognito profile browsers.
Browser* GetLastActiveBrowser(const Profile* profile,
                              const bool match_incognito_profile) {
  BrowserList* browser_list = BrowserList::GetInstance();
  Browser* target_browser = nullptr;
  for (auto iter = browser_list->begin_browsers_ordered_by_activation();
       iter != browser_list->end_browsers_ordered_by_activation(); ++iter) {
    Profile* browser_profile = (*iter)->profile();
    if (browser_profile == profile ||
        (match_incognito_profile &&
         browser_profile->GetOriginalProfile() == profile)) {
      target_browser = *iter;
      break;
    }
  }

  return target_browser;
}

// Get the id of the allowlisted extension. At the moment two switches can
// contain it. Prioritize the non-deprecated one.
std::string GetAllowlistedExtensionID() {
  std::string id = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
      switches::kAllowlistedExtensionID);
  if (id.empty()) {
    id = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
        switches::kDEPRECATED_AllowlistedExtensionID);
  }
  return id;
}

}  // namespace

ExtensionFunction::ResponseAction TabCaptureCaptureFunction::Run() {
  std::unique_ptr<api::tab_capture::Capture::Params> params =
      TabCapture::Capture::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  Profile* profile = Profile::FromBrowserContext(browser_context());
  const bool match_incognito_profile = include_incognito_information();
  Browser* target_browser =
      GetLastActiveBrowser(profile, match_incognito_profile);
#if 0
  if (!target_browser)
    return RespondNow(Error(kFindingTabError));
#endif

  content::WebContents* target_contents = nullptr;
  if (target_browser)
    target_contents = target_browser->tab_strip_model()->GetActiveWebContents();
  else
    target_contents = GetSenderWebContents();
  if (!target_contents)
    return RespondNow(Error(kFindingTabError));

  content::WebContents* const extension_web_contents = GetSenderWebContents();
  EXTENSION_FUNCTION_VALIDATE(extension_web_contents);

  const GURL& extension_origin =
      extension_web_contents->GetLastCommittedURL().DeprecatedGetOriginAsURL();
  AllowedScreenCaptureLevel capture_level =
      capture_policy::GetAllowedCaptureLevel(
          extension_web_contents->GetLastCommittedURL()
              .DeprecatedGetOriginAsURL(),
          extension_web_contents);

  DesktopMediaList::WebContentsFilter includable_web_contents_filter =
      capture_policy::GetIncludableWebContentsFilter(extension_origin,
                                                     capture_level);
  if (!includable_web_contents_filter.Run(target_contents)) {
    return RespondNow(Error(kGrantError));
  }

  const std::string& extension_id = extension()->id();

  // Make sure either we have been granted permission to capture through an
  // extension icon click or our extension is allowlisted.
  if (!extension()->is_nwjs_app() && !extension()->permissions_data()->HasAPIPermissionForTab(
          sessions::SessionTabHelper::IdForTab(target_contents).id(),
          mojom::APIPermissionID::kTabCaptureForTab) &&
      (GetAllowlistedExtensionID() != extension_id)) {
    return RespondNow(Error(kGrantError));
  }

  if (!OptionsSpecifyAudioOrVideo(params->options))
    return RespondNow(Error(kNoAudioOrVideo));

  DesktopMediaID source =
      BuildDesktopMediaID(target_contents, &params->options);
  TabCaptureRegistry* registry = TabCaptureRegistry::Get(browser_context());
  std::string device_id = registry->AddRequest(
      target_contents, extension_id, false, extension()->url(), source,
      extension()->name(), extension_web_contents);
  if (device_id.empty()) {
    return RespondNow(Error(kCapturingSameTab));
  }
  AddMediaStreamSourceConstraints(target_contents, &params->options, device_id);

  // At this point, everything is set up in the browser process.  It's now up to
  // the custom JS bindings in the extension's render process to request a
  // MediaStream using navigator.webkitGetUserMedia().  The result dictionary,
  // passed to SetResult() here, contains the extra "hidden options" that will
  // allow the Chrome platform implementation for getUserMedia() to start the
  // virtual audio/video capture devices and set up all the data flows.  The
  // custom JS bindings can be found here:
  // chrome/renderer/resources/extensions/tab_capture_custom_bindings.js
  std::unique_ptr<base::DictionaryValue> result(new base::DictionaryValue());
  result->MergeDictionary(params->options.ToValue().get());
  return RespondNow(
      OneArgument(base::Value::FromUniquePtrValue(std::move(result))));
}

ExtensionFunction::ResponseAction TabCaptureGetCapturedTabsFunction::Run() {
  TabCaptureRegistry* registry = TabCaptureRegistry::Get(browser_context());
  std::unique_ptr<base::ListValue> list(new base::ListValue());
  if (registry)
    registry->GetCapturedTabs(extension()->id(), list.get());
  return RespondNow(
      OneArgument(base::Value::FromUniquePtrValue(std::move(list))));
}

ExtensionFunction::ResponseAction TabCaptureGetMediaStreamIdFunction::Run() {
  std::unique_ptr<api::tab_capture::GetMediaStreamId::Params> params =
      TabCapture::GetMediaStreamId::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  content::WebContents* target_contents = nullptr;
  if (params->options && params->options->target_tab_id) {
    if (!ExtensionTabUtil::GetTabById(*(params->options->target_tab_id),
                                      browser_context(), true,
                                      &target_contents)) {
      return RespondNow(Error(kInvalidTabIdError));
    }
  } else {
    Profile* profile = Profile::FromBrowserContext(browser_context());
    const bool match_incognito_profile = include_incognito_information();
    Browser* target_browser =
        GetLastActiveBrowser(profile, match_incognito_profile);
    if (!target_browser)
      return RespondNow(Error(kFindingTabError));

    target_contents = target_browser->tab_strip_model()->GetActiveWebContents();
  }
  if (!target_contents)
    return RespondNow(Error(kFindingTabError));

  const std::string& extension_id = extension()->id();

  // Make sure either we have been granted permission to capture through an
  // extension icon click or our extension is allowlisted.
  if (!extension()->permissions_data()->HasAPIPermissionForTab(
          sessions::SessionTabHelper::IdForTab(target_contents).id(),
          mojom::APIPermissionID::kTabCaptureForTab) &&
      (GetAllowlistedExtensionID() != extension_id)) {
    return RespondNow(Error(kGrantError));
  }

  // |consumer_contents| is the WebContents for which the stream is created.
  content::WebContents* consumer_contents = nullptr;
  std::string consumer_name;
  GURL origin;
  if (params->options && params->options->consumer_tab_id) {
    if (!ExtensionTabUtil::GetTabById(*(params->options->consumer_tab_id),
                                      browser_context(), true,
                                      &consumer_contents)) {
      return RespondNow(Error(kInvalidTabIdError));
    }

    origin =
        consumer_contents->GetLastCommittedURL().DeprecatedGetOriginAsURL();
    if (!origin.is_valid()) {
      return RespondNow(Error(kInvalidOriginError));
    }

    if (!network::IsUrlPotentiallyTrustworthy(origin)) {
      return RespondNow(Error(kTabUrlNotSecure));
    }

    consumer_name = net::GetHostAndOptionalPort(origin);
  } else {
    origin = extension()->url();
    consumer_name = extension()->name();
    consumer_contents = GetSenderWebContents();
  }
  EXTENSION_FUNCTION_VALIDATE(consumer_contents);

  DesktopMediaID source = BuildDesktopMediaID(target_contents, nullptr);
  TabCaptureRegistry* registry = TabCaptureRegistry::Get(browser_context());
  std::string device_id =
      registry->AddRequest(target_contents, extension_id, false, origin, source,
                           consumer_name, consumer_contents);
  if (device_id.empty()) {
    return RespondNow(Error(kCapturingSameTab));
  }

  return RespondNow(OneArgument(base::Value(device_id)));
}

}  // namespace extensions
