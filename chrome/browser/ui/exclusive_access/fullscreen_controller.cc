// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma clang diagnostic ignored "-Wunreachable-code"

#include "chrome/browser/ui/exclusive_access/fullscreen_controller.h"

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/observer_list.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "chrome/browser/app_mode/app_mode_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/blocked_content/popunder_preventer.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_context.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_within_tab_helper.h"
#include "chrome/browser/ui/status_bubble.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/extension.h"
#include "third_party/blink/public/common/features.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/display/types/display_constants.h"

#if !BUILDFLAG(IS_MAC)
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#endif

using content::WebContents;

namespace {

const char kFullscreenBubbleReshowsHistogramName[] =
    "ExclusiveAccess.BubbleReshowsPerSession.Fullscreen";

int64_t GetDisplayId(WebContents* web_contents) {
  DCHECK(web_contents);
  auto* screen = display::Screen::GetScreen();
  auto display = screen->GetDisplayNearestView(web_contents->GetNativeView());
  return display.id();
}

bool IsAnotherScreen(WebContents* web_contents, const int64_t display_id) {
  if (display_id == display::kInvalidDisplayId)
    return false;
  return display_id != GetDisplayId(web_contents);
}

}  // namespace

FullscreenController::FullscreenController(ExclusiveAccessManager* manager)
    : ExclusiveAccessControllerBase(manager) {}

FullscreenController::~FullscreenController() = default;

void FullscreenController::AddObserver(FullscreenObserver* observer) {
  observer_list_.AddObserver(observer);
}

void FullscreenController::RemoveObserver(FullscreenObserver* observer) {
  observer_list_.RemoveObserver(observer);
}

bool FullscreenController::IsFullscreenForBrowser() const {
  return exclusive_access_manager()->context()->IsFullscreen() &&
         !IsFullscreenCausedByTab();
}

void FullscreenController::ToggleBrowserFullscreenMode() {
  extension_caused_fullscreen_ = GURL();
  ToggleFullscreenModeInternal(BROWSER, nullptr, display::kInvalidDisplayId);
}

void FullscreenController::ToggleBrowserFullscreenModeWithExtension(
    const GURL& extension_url) {
  // |extension_caused_fullscreen_| will be reset if this causes fullscreen to
  // exit.
  extension_caused_fullscreen_ = extension_url;
  ToggleFullscreenModeInternal(BROWSER, nullptr, display::kInvalidDisplayId);
}

bool FullscreenController::IsWindowFullscreenForTabOrPending() const {
  return exclusive_access_tab() || is_tab_fullscreen_for_testing_;
}

bool FullscreenController::IsExtensionFullscreenOrPending() const {
  return !extension_caused_fullscreen_.is_empty();
}

bool FullscreenController::IsControllerInitiatedFullscreen() const {
  return toggled_into_fullscreen_;
}

bool FullscreenController::IsTabFullscreen() const {
  return tab_fullscreen_ || is_tab_fullscreen_for_testing_;
}

bool FullscreenController::IsFullscreenForTabOrPending(
    const WebContents* web_contents) const {
  if (IsFullscreenWithinTab(web_contents))
    return true;
  if (web_contents == exclusive_access_tab()) {
    // If we're handling OnTabDeactivated(), |web_contents| is the
    // deactivated contents. On the other hand,
    // exclusive_access_manager()->context()->GetActiveWebContents() returns
    // newly activated contents. That's because deactivation of tab is notified
    // after TabStripModel's internal state is consistent.
    DCHECK(web_contents ==
               exclusive_access_manager()->context()->GetActiveWebContents() ||
           web_contents == deactivated_contents_);
    return true;
  }
  return false;
}

bool FullscreenController::IsFullscreenCausedByTab() const {
  return state_prior_to_tab_fullscreen_ == STATE_NORMAL;
}

bool FullscreenController::CanEnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const int64_t display_id) {
  DCHECK(requesting_frame);
  auto* web_contents = WebContents::FromRenderFrameHost(requesting_frame);
  DCHECK(web_contents);

  if (web_contents !=
      exclusive_access_manager()->context()->GetActiveWebContents())
    return false;

  return true;
}

void FullscreenController::EnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const int64_t display_id) {
  DCHECK(requesting_frame);
  // This function should never fail. Any possible failures must be checked in
  // |CanEnterFullscreenModeForTab()| instead. Silently dropping the request
  // could cause requestFullscreen promises to hang. If we are in this function,
  // the renderer expects a visual property update to call
  // |blink::FullscreenController::DidEnterFullscreen| to resolve promises.
  DCHECK(CanEnterFullscreenModeForTab(requesting_frame, display_id));
  auto* web_contents = WebContents::FromRenderFrameHost(requesting_frame);
  DCHECK(web_contents);

  if (MaybeToggleFullscreenWithinTab(web_contents, true)) {
    // During tab capture of fullscreen-within-tab views, the browser window
    // fullscreen state is unchanged, so return now.
    return;
  }

  if (base::FeatureList::IsEnabled(
          blink::features::kWindowPlacementFullscreenCompanionWindow)) {
    if (!popunder_preventer_)
      popunder_preventer_ = std::make_unique<PopunderPreventer>(web_contents);
    else
      popunder_preventer_->WillActivateWebContents(web_contents);
  }

  // Keep the current state. |SetTabWithExclusiveAccess| may change the return
  // value of |IsWindowFullscreenForTabOrPending|.
  const bool requesting_another_screen =
      IsAnotherScreen(web_contents, display_id);
  const bool was_window_fullscreen_for_tab_or_pending =
      !requesting_another_screen && IsWindowFullscreenForTabOrPending();

  SetTabWithExclusiveAccess(web_contents);
  requesting_origin_ =
      requesting_frame->GetLastCommittedURL().DeprecatedGetOriginAsURL();

  if (was_window_fullscreen_for_tab_or_pending) {
    // While an element is in fullscreen, requesting fullscreen for a different
    // element in the tab is handled in the renderer process if both elements
    // are in the same process. But the request will come to the browser when
    // the element is in a different process, such as OOPIF, because the
    // renderer doesn't know if an element in other renderer process is in
    // fullscreen.
    DCHECK(tab_fullscreen_);
  } else {
    ExclusiveAccessContext* exclusive_access_context =
        exclusive_access_manager()->context();
    // This is needed on Mac as entering into Tab Fullscreen might change the
    // top UI style.
    exclusive_access_context->UpdateUIForTabFullscreen();

    state_prior_to_tab_fullscreen_ =
        IsFullscreenForBrowser() ? STATE_BROWSER_FULLSCREEN : STATE_NORMAL;

    if (!exclusive_access_context->IsFullscreen() ||
        requesting_another_screen) {
      EnterFullscreenModeInternal(TAB, requesting_frame, display_id);
      return;
    }

    // We need to update the fullscreen exit bubble, e.g., going from browser
    // fullscreen to tab fullscreen will need to show different content.
    tab_fullscreen_ = true;
    exclusive_access_manager()->UpdateExclusiveAccessExitBubbleContent(
        ExclusiveAccessBubbleHideCallback());
  }

  // This is only a change between Browser and Tab fullscreen. We generate
  // a fullscreen notification now because there is no window change.
  PostFullscreenChangeNotification();
}

void FullscreenController::ExitFullscreenModeForTab(WebContents* web_contents) {
  popunder_preventer_.reset();

  if (MaybeToggleFullscreenWithinTab(web_contents, false)) {
    // During tab capture of fullscreen-within-tab views, the browser window
    // fullscreen state is unchanged, so return now.
    return;
  }

  if (!IsWindowFullscreenForTabOrPending() ||
      web_contents != exclusive_access_tab()) {
    return;
  }

  ExclusiveAccessContext* exclusive_access_context =
      exclusive_access_manager()->context();

  if (!exclusive_access_context->IsFullscreen())
    return;

  if (IsFullscreenCausedByTab()) {
    // Tab Fullscreen -> Normal.
    ToggleFullscreenModeInternal(TAB, nullptr, display::kInvalidDisplayId);
    return;
  }

  // Tab Fullscreen -> Browser Fullscreen.
  // Exiting tab fullscreen mode may require updating top UI.
  // All exiting tab fullscreen to non-fullscreen mode cases are handled in
  // BrowserNonClientFrameView::OnFullscreenStateChanged(); but exiting tab
  // fullscreen to browser fullscreen should be handled here.
  const bool was_browser_fullscreen =
      state_prior_to_tab_fullscreen_ == STATE_BROWSER_FULLSCREEN;

  NotifyTabExclusiveAccessLost();
  if (was_browser_fullscreen)
    exclusive_access_context->UpdateUIForTabFullscreen();

  // This is only a change between Browser and Tab fullscreen. We generate
  // a fullscreen notification now because there is no window change.
  PostFullscreenChangeNotification();

  // For Tab Fullscreen -> Browser Fullscreen, enter browser fullscreen on the
  // display that originated the browser fullscreen prior to the tab fullscreen.
  // crbug.com/1313606.
  if (was_browser_fullscreen &&
      display_id_prior_to_tab_fullscreen_ != display::kInvalidDisplayId &&
      display_id_prior_to_tab_fullscreen_ != GetDisplayId(web_contents)) {
    EnterFullscreenModeInternal(BROWSER, nullptr,
                                display_id_prior_to_tab_fullscreen_);
  }
}

void FullscreenController::FullscreenTabOpeningPopup(
    content::WebContents* opener,
    content::WebContents* popup) {
  DCHECK(base::FeatureList::IsEnabled(
      blink::features::kWindowPlacementFullscreenCompanionWindow));
  DCHECK_EQ(exclusive_access_tab(), opener);
  DCHECK(popunder_preventer_);
  popunder_preventer_->AddPotentialPopunder(popup);
}

void FullscreenController::OnTabDeactivated(
    content::WebContents* web_contents) {
  base::AutoReset<content::WebContents*> auto_resetter(&deactivated_contents_,
                                                       web_contents);
  ExclusiveAccessControllerBase::OnTabDeactivated(web_contents);
}

void FullscreenController::OnTabDetachedFromView(WebContents* old_contents) {
  if (!IsFullscreenWithinTab(old_contents))
    return;

  // A fullscreen-within-tab view undergoing screen capture has been detached
  // and is no longer visible to the user. Set it to exactly the WebContents'
  // preferred size. See 'FullscreenWithinTab Note'.
  //
  // When the user later selects the tab to show |old_contents| again, UI code
  // elsewhere (e.g., views::WebView) will resize the view to fit within the
  // browser window once again.

  // If the view has been detached from the browser window (e.g., to drag a tab
  // off into a new browser window), return immediately to avoid an unnecessary
  // resize.
  if (!old_contents->GetDelegate())
    return;

  // Do nothing if tab capture ended after toggling fullscreen, or a preferred
  // size was never specified by the capturer.
  if (!old_contents->IsBeingCaptured() ||
      old_contents->GetPreferredSize().IsEmpty()) {
    return;
  }

  old_contents->Resize(gfx::Rect(old_contents->GetPreferredSize()));
}

void FullscreenController::OnTabClosing(WebContents* web_contents) {
  if (IsFullscreenWithinTab(web_contents))
    web_contents->ExitFullscreen(
        /* will_cause_resize */ IsFullscreenCausedByTab());
  else
    ExclusiveAccessControllerBase::OnTabClosing(web_contents);
}

void FullscreenController::WindowFullscreenStateChanged() {
  ExclusiveAccessContext* const exclusive_access_context =
      exclusive_access_manager()->context();
  bool exiting_fullscreen = !exclusive_access_context->IsFullscreen();

  PostFullscreenChangeNotification();
  if (exiting_fullscreen) {
    toggled_into_fullscreen_ = false;
    extension_caused_fullscreen_ = GURL();
    NotifyTabExclusiveAccessLost();
  } else {
    toggled_into_fullscreen_ = true;
  }
}

bool FullscreenController::HandleUserPressedEscape() {
  return false;
  WebContents* const active_web_contents =
      exclusive_access_manager()->context()->GetActiveWebContents();
  if (IsFullscreenWithinTab(active_web_contents)) {
    active_web_contents->ExitFullscreen(
        /* will_cause_resize */ IsFullscreenCausedByTab());
    return true;
  }

  if (!IsWindowFullscreenForTabOrPending())
    return false;

  ExitExclusiveAccessIfNecessary();
  return true;
}

void FullscreenController::ExitExclusiveAccessToPreviousState() {
  if (IsWindowFullscreenForTabOrPending())
    ExitFullscreenModeForTab(exclusive_access_tab());
  else if (IsFullscreenForBrowser())
    ExitFullscreenModeInternal();
}

GURL FullscreenController::GetURLForExclusiveAccessBubble() const {
  if (exclusive_access_tab())
    return GetRequestingOrigin();
  return extension_caused_fullscreen_;
}

void FullscreenController::ExitExclusiveAccessIfNecessary() {
  if (IsWindowFullscreenForTabOrPending())
    ExitFullscreenModeForTab(exclusive_access_tab());
  else
    NotifyTabExclusiveAccessLost();
}

void FullscreenController::PostFullscreenChangeNotification() {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&FullscreenController::NotifyFullscreenChange,
                                ptr_factory_.GetWeakPtr()));
}

void FullscreenController::NotifyFullscreenChange() {
  for (auto& observer : observer_list_)
    observer.OnFullscreenStateChanged();
}

void FullscreenController::NotifyTabExclusiveAccessLost() {
  if (exclusive_access_tab()) {
    WebContents* web_contents = exclusive_access_tab();
    SetTabWithExclusiveAccess(nullptr);
    requesting_origin_ = GURL();
    bool will_cause_resize = IsFullscreenCausedByTab();
    state_prior_to_tab_fullscreen_ = STATE_INVALID;
    tab_fullscreen_ = false;
    web_contents->ExitFullscreen(will_cause_resize);
    exclusive_access_manager()->UpdateExclusiveAccessExitBubbleContent(
        ExclusiveAccessBubbleHideCallback());
  }
}

void FullscreenController::RecordBubbleReshowsHistogram(
    int bubble_reshow_count) {
  UMA_HISTOGRAM_COUNTS_100(kFullscreenBubbleReshowsHistogramName,
                           bubble_reshow_count);
}

void FullscreenController::ToggleFullscreenModeInternal(
    FullscreenInternalOption option,
    content::RenderFrameHost* requesting_frame,
    const int64_t display_id) {
  ExclusiveAccessContext* const exclusive_access_context =
      exclusive_access_manager()->context();
  bool enter_fullscreen = !exclusive_access_context->IsFullscreen();

  if (enter_fullscreen)
    EnterFullscreenModeInternal(option, requesting_frame, display_id);
  else
    ExitFullscreenModeInternal();
}

void FullscreenController::EnterFullscreenModeInternal(
    FullscreenInternalOption option,
    content::RenderFrameHost* requesting_frame,
    int64_t display_id) {
#if !BUILDFLAG(IS_MAC)
  // Do not enter fullscreen mode if disallowed by pref. This prevents the user
  // from manually entering fullscreen mode and also disables kiosk mode on
  // desktop platforms.
  if (!exclusive_access_manager()
           ->context()
           ->GetProfile()
           ->GetPrefs()
           ->GetBoolean(prefs::kFullscreenAllowed)) {
    return;
  }
#endif

  toggled_into_fullscreen_ = true;
  bool entering_tab_fullscreen = option == TAB && !tab_fullscreen_;
  GURL url;
  if (option == TAB) {
    url = GetRequestingOrigin();
    tab_fullscreen_ = true;
  } else {
    if (!extension_caused_fullscreen_.is_empty())
      url = extension_caused_fullscreen_;
  }

  if (option == TAB && display_id != display::kInvalidDisplayId) {
    // Check, but do not prompt, for permission to request a specific screen.
    // Sites generally need permission to get the display id in the first place.
    if (!requesting_frame ||
        requesting_frame->GetBrowserContext()
                ->GetPermissionController()
                ->GetPermissionStatusForCurrentDocument(
                    content::PermissionType::WINDOW_PLACEMENT,
                    requesting_frame) !=
            blink::mojom::PermissionStatus::GRANTED) {
      display_id = display::kInvalidDisplayId;
    }
    if (entering_tab_fullscreen) {
      display_id_prior_to_tab_fullscreen_ =
          GetDisplayId(WebContents::FromRenderFrameHost(requesting_frame));
    }
  }

  if (option == BROWSER)
    base::RecordAction(base::UserMetricsAction("ToggleFullscreen"));
  // TODO(scheib): Record metrics for WITH_TOOLBAR, without counting transitions
  // from tab fullscreen out to browser with toolbar.

  exclusive_access_manager()->context()->EnterFullscreen(
      url, exclusive_access_manager()->GetExclusiveAccessExitBubbleType(),
      display_id);

  exclusive_access_manager()->UpdateExclusiveAccessExitBubbleContent(
      ExclusiveAccessBubbleHideCallback());

  // Once the window has become fullscreen it'll call back to
  // WindowFullscreenStateChanged(). We don't do this immediately as
  // BrowserWindow::EnterFullscreen() asks for bookmark_bar_state_, so we let
  // the BrowserWindow invoke WindowFullscreenStateChanged when appropriate.
}

void FullscreenController::ExitFullscreenModeInternal() {
  // In kiosk mode, we always want to be fullscreen.
  if (chrome::IsRunningInAppMode())
    return;

  RecordExitingUMA();
  toggled_into_fullscreen_ = false;
#if BUILDFLAG(IS_MAC)
  // Mac windows report a state change instantly, and so we must also clear
  // state_prior_to_tab_fullscreen_ to match them else other logic using
  // state_prior_to_tab_fullscreen_ will be incorrect.
  NotifyTabExclusiveAccessLost();
#endif
  exclusive_access_manager()->context()->ExitFullscreen();
  extension_caused_fullscreen_ = GURL();

  exclusive_access_manager()->UpdateExclusiveAccessExitBubbleContent(
      ExclusiveAccessBubbleHideCallback());
}

bool FullscreenController::MaybeToggleFullscreenWithinTab(
    WebContents* web_contents,
    bool enter_fullscreen) {
  if (enter_fullscreen) {
    if (web_contents->IsBeingVisiblyCaptured()) {
      FullscreenWithinTabHelper::CreateForWebContents(web_contents);
      FullscreenWithinTabHelper::FromWebContents(web_contents)
          ->SetIsFullscreenWithinTab(true);
      return true;
    }
  } else {
    if (IsFullscreenWithinTab(web_contents)) {
      FullscreenWithinTabHelper::RemoveForWebContents(web_contents);
      return true;
    }
  }

  return false;
}

bool FullscreenController::IsFullscreenWithinTab(
    const WebContents* web_contents) const {
  if (is_tab_fullscreen_for_testing_)
    return true;

  // Note: On Mac, some of the OnTabXXX() methods get called with a nullptr
  // value
  // for web_contents. Check for that here.
  const FullscreenWithinTabHelper* const helper =
      web_contents ? FullscreenWithinTabHelper::FromWebContents(web_contents)
                   : nullptr;
  if (helper && helper->is_fullscreen_within_tab()) {
    DCHECK_NE(exclusive_access_tab(), web_contents);
    return true;
  }
  return false;
}

GURL FullscreenController::GetRequestingOrigin() const {
  DCHECK(exclusive_access_tab());

  if (!requesting_origin_.is_empty())
    return requesting_origin_;

  return exclusive_access_tab()->GetLastCommittedURL();
}

GURL FullscreenController::GetEmbeddingOrigin() const {
  DCHECK(exclusive_access_tab());

  return exclusive_access_tab()->GetLastCommittedURL();
}
