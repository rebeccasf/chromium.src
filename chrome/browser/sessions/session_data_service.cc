// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sessions/session_data_service.h"

#include "content/nw/src/nw_content.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/app_window/app_window_registry.h"
#include "chrome/browser/extensions/extension_service.h"

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/metrics/histogram_functions.h"
#include "base/time/time.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/exit_type_service.h"
#include "chrome/browser/sessions/session_data_deleter.h"
#include "chrome/browser/sessions/sessions_features.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "storage/browser/quota/special_storage_policy.h"

namespace {
// Pref name for Status preference.
extern const char kSessionDataStatusPref[] = "sessions.session_data_status";
}  // namespace

// static
void SessionDataService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterIntegerPref(kSessionDataStatusPref,
                                static_cast<int>(Status::kUninitialized));
}

SessionDataService::SessionDataService(
    Profile* profile,
    std::unique_ptr<SessionDataDeleter> deleter)
    : profile_(profile), deleter_(std::move(deleter)) {
  DCHECK(profile_);
  DCHECK(!profile_->IsOffTheRecord());
  DCHECK(deleter_);

  Status last_status = static_cast<Status>(
      profile_->GetPrefs()->GetInteger(kSessionDataStatusPref));

  int int_status = static_cast<int>(last_status);
  if (int_status < 0 || int_status > static_cast<int>(Status::kMaxValue))
    last_status = Status::kUninitialized;

  SetStatusPref(Status::kInitialized);
  auto* policy = profile_->GetSpecialStoragePolicy();
  if (policy && policy->HasSessionOnlyOrigins()) {
    RecordHistogramForLastSession(last_status);
    if (base::FeatureList::IsEnabled(kDeleteSessionOnlyDataOnStartup))
      MaybeContinueDeletionFromLastSesssion(last_status);
  }

  for (Browser* browser : *BrowserList::GetInstance())
    OnBrowserAdded(browser);

  BrowserList::AddObserver(this);
}

SessionDataService::~SessionDataService() {
  BrowserList::RemoveObserver(this);
}

void SessionDataService::RecordHistogramForLastSession(Status last_status) {
  if (last_status == Status::kUninitialized)
    return;
  base::UmaHistogramEnumeration("Session.SessionData.StatusFromLastSession",
                                last_status);
}

void SessionDataService::MaybeContinueDeletionFromLastSesssion(
    Status last_status) {
  switch (last_status) {
    case Status::kUninitialized:
    case Status::kDeletionFinished:
    case Status::kNoDeletionDueToForceKeepSessionData:
      return;  // No deletion needed.
    case Status::kInitialized:
      // Deletion did not happen on shutdown and we didn't even update the
      // status preference. Check profile status:
      switch (ExitTypeService::GetLastSessionExitType(profile_)) {
        case ExitType::kCrashed:
          // To allow the user to continue a session after a crash, we will not
          // delete cookies.
          return;
        case ExitType::kClean:
        case ExitType::kForcedShutdown:
          // In case of a regular shutdown that skipped deletion, we should
          // delete cookies.
          break;
      }
      break;
    case Status::kDeletionStarted:
    case Status::kNoDeletionDueToShutdown:
      break;  // Deletion needed.
  }

  // Skip session cookie deletion as they already get cleared on startup.
  // (SQLitePersistentCookieStore::Backend::DeleteSessionCookiesOnStartup)
  deleter_->DeleteSessionOnlyData(
      /*skip_session_cookies=*/true,
      base::BindOnce(&SessionDataService::OnCleanupAtStartupFinished,
                     base::Unretained(this), base::TimeTicks::Now()));
}

void SessionDataService::OnCleanupAtStartupFinished(
    base::TimeTicks time_started) {
  base::UmaHistogramMediumTimes("Session.SessionData.StartupCleanupTime",
                                base::TimeTicks::Now() - time_started);
}

void SessionDataService::SetStatusPref(Status status) {
  profile_->GetPrefs()->SetInteger(kSessionDataStatusPref,
                                   static_cast<int>(status));
}

void SessionDataService::OnBrowserAdded(Browser* browser) {
  if (browser->profile() != profile_)
    return;

  // A window was opened. Ensure that we run another cleanup the next time
  // all windows are closed.
  SetStatusPref(Status::kInitialized);
  cleanup_started_ = false;
}

void SessionDataService::OnBrowserRemoved(Browser* browser) {
  if (browser->profile() != profile_)
    return;

  // Check for any open windows for the current profile.
  for (auto* open_browser : *BrowserList::GetInstance()) {
    if (open_browser->profile() == profile_)
      return;
  }

  std::string id = nw::GetMainExtensionId();
  base::FilePath path;
  extensions::ExtensionRegistry* extension_registry =
    extensions::ExtensionRegistry::Get(ProfileManager::GetPrimaryUserProfile());
  if (!id.empty() && extension_registry) {
    const extensions::Extension* extension =
      extension_registry->GetExtensionById(id, extensions::ExtensionRegistry::EVERYTHING);
    if (extension) {
      if (extensions::ProcessManager::Get(profile_)->GetLazyKeepaliveCount(extension) > 0)
        return;
    }
    //additional checking for NWJS#5355
    extensions::AppWindowRegistry* registry = extensions::AppWindowRegistry::Factory::GetForBrowserContext(profile_, false);
    if (registry && !registry->app_windows().empty())
      return;
  }

  // Session cookies should stay alive on platforms where the browser stays
  // alive without windows.
  bool skip_session_cookies = browser_defaults::kBrowserAliveWithNoWindows;

  // Clear session data if the last window for a profile has been closed.
  StartCleanupInternal(skip_session_cookies);
}

void SessionDataService::StartCleanup() {
  StartCleanupInternal(false);
}

void SessionDataService::StartCleanupInternal(bool skip_session_cookies) {
  if (cleanup_started_)
    return;

  if (force_keep_session_state_)
    return;

  if (browser_shutdown::IsTryingToQuit()) {
    SetStatusPref(Status::kNoDeletionDueToShutdown);
    return;
  }

  cleanup_started_ = true;
  SetStatusPref(Status::kDeletionStarted);

  // Using base::Unretained is safe as DeleteSessionOnlyData() uses a
  // ScopedProfileKeepAlive.
  deleter_->DeleteSessionOnlyData(
      skip_session_cookies,
      base::BindOnce(&SessionDataService::OnCleanupAtSessionEndFinished,
                     base::Unretained(this), base::TimeTicks::Now()));
}

void SessionDataService::OnCleanupAtSessionEndFinished(
    base::TimeTicks time_started) {
  SetStatusPref(Status::kDeletionFinished);
  auto* policy = profile_->GetSpecialStoragePolicy();
  if (policy && policy->HasSessionOnlyOrigins()) {
    base::UmaHistogramMediumTimes("Session.SessionData.CleanupTime",
                                  base::TimeTicks::Now() - time_started);
  }
}

void SessionDataService::SetForceKeepSessionState() {
  SetStatusPref(Status::kNoDeletionDueToForceKeepSessionData);
  force_keep_session_state_ = true;
}
