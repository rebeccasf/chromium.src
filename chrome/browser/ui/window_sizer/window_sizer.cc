// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma clang diagnostic ignored "-Wunreachable-code"
#include "chrome/browser/ui/window_sizer/window_sizer.h"

#include <algorithm>
#include "content/nw/src/nw_base.h"
#include <utility>

#include "base/command_line.h"
#include "base/memory/raw_ptr.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window_state.h"
#include "chrome/common/chrome_switches.h"
#include "components/prefs/pref_service.h"
#include "ui/base/ui_base_switches.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"

#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
#include "chrome/browser/ui/window_sizer/window_sizer_chromeos.h"
#endif

namespace {

// Minimum height of the visible part of a window.
const int kMinVisibleHeight = 30;
// Minimum width of the visible part of a window.
const int kMinVisibleWidth = 30;

///////////////////////////////////////////////////////////////////////////////
// An implementation of WindowSizer::StateProvider that gets the last active
// and persistent state from the browser window and the user's profile.
class DefaultStateProvider : public WindowSizer::StateProvider {
 public:
  explicit DefaultStateProvider(const Browser* browser) : browser_(browser) {}

  DefaultStateProvider(const DefaultStateProvider&) = delete;
  DefaultStateProvider& operator=(const DefaultStateProvider&) = delete;

  // Overridden from WindowSizer::StateProvider:
  bool GetPersistentState(gfx::Rect* bounds,
                          gfx::Rect* work_area,
                          ui::WindowShowState* show_state) const override {
    DCHECK(bounds);
    DCHECK(show_state);

    if (!browser_ || !browser_->profile()->GetPrefs())
      return false;

    if (browser_->is_type_popup() && browser_->windows_key().empty())
      return false;
    const base::Value* pref = chrome::GetWindowPlacementDictionaryReadOnly(
        chrome::GetWindowName(browser_), browser_->profile()->GetPrefs());

    absl::optional<gfx::Rect> pref_bounds = RectFromPrefixedPref(pref, "");
    absl::optional<gfx::Rect> pref_area =
        RectFromPrefixedPref(pref, "work_area_");
    absl::optional<bool> maximized =
        pref ? pref->FindBoolPath("maximized") : absl::nullopt;
    absl::optional<bool> fullscreen =
        pref ? pref->FindBoolPath("fullscreen") : absl::nullopt;

    if (!pref_bounds || !maximized)
      return false;

    *bounds = pref_bounds.value();
    if (pref_area)
      *work_area = pref_area.value();
    if (*show_state == ui::SHOW_STATE_DEFAULT && maximized.value())
      *show_state = ui::SHOW_STATE_MAXIMIZED;
    if (*show_state == ui::SHOW_STATE_DEFAULT && fullscreen && fullscreen.value())
      *show_state = ui::SHOW_STATE_FULLSCREEN;

    return true;
  }

  bool GetLastActiveWindowState(
      gfx::Rect* bounds,
      ui::WindowShowState* show_state) const override {
    DCHECK(show_state);
    // Applications and devtools are always restored with the same position.
    if (browser_ && (browser_->is_type_app() || browser_->is_type_app_popup() ||
                     browser_->is_type_devtools()))
      return false;

    // If a reference browser is set, use its window. Otherwise find last
    // active. Panels are never used as reference browsers as panels are
    // specially positioned.
    BrowserWindow* window = NULL;
    // Window may be null if browser is just starting up.
    if (browser_ && browser_->window()) {
      window = browser_->window();
    } else {
      const BrowserList* browser_list = BrowserList::GetInstance();
      for (auto it = browser_list->begin_browsers_ordered_by_activation();
           it != browser_list->end_browsers_ordered_by_activation(); ++it) {
        Browser* last_active = *it;
        if (last_active && last_active->is_type_normal()) {
          window = last_active->window();
          DCHECK(window);
          break;
        }
      }
    }

    if (window) {
#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
      if (window->IsVisible())
        *bounds = window->GetRestoredBounds();
#else
      *bounds = window->GetRestoredBounds();
#endif
      if (*show_state == ui::SHOW_STATE_DEFAULT && window->IsMaximized())
        *show_state = ui::SHOW_STATE_MAXIMIZED;
      return true;
    }

    return false;
  }

 private:
  static absl::optional<gfx::Rect> RectFromPrefixedPref(
      const base::Value* pref,
      const std::string& prefix) {
    if (!pref)
      return absl::nullopt;

    absl::optional<int> top, left, bottom, right;

    top = pref->FindIntKey(prefix + "top");
    left = pref->FindIntKey(prefix + "left");
    bottom = pref->FindIntKey(prefix + "bottom");
    right = pref->FindIntKey(prefix + "right");

    if (!top || !left || !bottom || !right)
      return absl::nullopt;

    return gfx::Rect(left.value(), top.value(),
                     std::max(0, right.value() - left.value()),
                     std::max(0, bottom.value() - top.value()));
  }

  std::string app_name_;

  // If set, is used as the reference browser for GetLastActiveWindowState.
  raw_ptr<const Browser> browser_;
};

// This function, unlike base::clamp(), does not check if `min` is greater than
// `max`, and returns a bogus answer if it is. TODO(crbug.com/1235666) migrate
// all code that calls this function to use base::clamp() instead.
constexpr int BrokenClampThatShouldNotBeUsed(int value, int min, int max) {
  return std::min(std::max(value, min), max);
}

}  // namespace

WindowSizer::WindowSizer(std::unique_ptr<StateProvider> state_provider,
                         const Browser* browser)
    : state_provider_(std::move(state_provider)), browser_(browser) {}

WindowSizer::~WindowSizer() = default;

// static
bool WindowSizer::GetBrowserWindowBoundsAndShowState(
    const gfx::Rect& specified_bounds,
    const Browser* browser,
    gfx::Rect* window_bounds,
    ui::WindowShowState* show_state) {
  return GetBrowserWindowBoundsAndShowState(
      std::make_unique<DefaultStateProvider>(browser), specified_bounds,
      browser, window_bounds, show_state);
}

#if !BUILDFLAG(IS_LINUX)
// Linux has its own implementation, see WindowSizerLinux.
// static
bool WindowSizer::GetBrowserWindowBoundsAndShowState(
    std::unique_ptr<StateProvider> state_provider,
    const gfx::Rect& specified_bounds,
    const Browser* browser,
    gfx::Rect* bounds,
    ui::WindowShowState* show_state) {
  DCHECK(bounds);
  DCHECK(show_state);
#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
  WindowSizerChromeOS sizer(std::move(state_provider), browser);
#else
  WindowSizer sizer(std::move(state_provider), browser);
#endif
  // Pre-populate the window state with our default.
  *show_state = GetWindowDefaultShowState(browser);
  *bounds = specified_bounds;
  return sizer.DetermineWindowBoundsAndShowState(specified_bounds, bounds, show_state);
}
#endif  // !BUILDFLAG(IS_LINUX)

bool WindowSizer::DetermineWindowBoundsAndShowState(
    const gfx::Rect& specified_bounds,
    gfx::Rect* bounds,
    ui::WindowShowState* show_state) {
  // In upstream, non empty case is only used in chrome tests, so we ignore it.
  // In NW, the non empty value is the API parameter passed by browser->override_bounds()
  // Use this strategy as the central place to determine window size
  // priority: saved > API parameter > default in manifest > default value
  if (true || bounds->IsEmpty()) {
    // See if there's last active window's placement information.
    if (GetLastActiveWindowBounds(bounds, show_state))
      return false;
    gfx::Rect saved;
    // See if there's saved placement information.
    if (GetSavedWindowBounds(&saved, show_state)) {
      *bounds = saved;
      return true;
    }
    if (!bounds->IsEmpty())
      return false;
    // No saved placement, figure out some sensible default size based on
    // the user's screen size.
    *bounds = GetDefaultWindowBounds(GetDisplayForNewWindow());
    return false;
  }

  // In case that there was a bound given we need to make sure that it is
  // visible and fits on the screen.
  // Find the size of the work area of the monitor that intersects the bounds
  // of the anchor window. Note: AdjustBoundsToBeVisibleOnMonitorContaining
  // does not exactly what we want: It makes only sure that "a minimal part"
  // is visible on the screen.
  gfx::Rect work_area =
      display::Screen::GetScreen()->GetDisplayMatching(*bounds).work_area();

  AdjustWorkAreaForPlatform(work_area);

  // Resize so that it fits.
  bounds->AdjustToFit(work_area);
  return false;
}

void WindowSizer::AdjustWorkAreaForPlatform(gfx::Rect& work_area) {}

bool WindowSizer::GetLastActiveWindowBounds(
    gfx::Rect* bounds,
    ui::WindowShowState* show_state) const {
  DCHECK(bounds);
  DCHECK(show_state);
  if (!state_provider_.get() ||
      !state_provider_->GetLastActiveWindowState(bounds, show_state))
    return false;
  bounds->Offset(kWindowTilePixels, kWindowTilePixels);
  AdjustBoundsToBeVisibleOnDisplay(
      display::Screen::GetScreen()->GetDisplayMatching(*bounds), gfx::Rect(),
      bounds);
  return true;
}

bool WindowSizer::GetSavedWindowBounds(gfx::Rect* bounds,
                                       ui::WindowShowState* show_state) const {
  DCHECK(bounds);
  DCHECK(show_state);
  gfx::Rect saved_work_area;
  if (!state_provider_.get() ||
      !state_provider_->GetPersistentState(bounds,
                                           &saved_work_area,
                                           show_state))
    return false;
  AdjustBoundsToBeVisibleOnDisplay(GetDisplayForNewWindow(*bounds),
                                   saved_work_area, bounds);
  return true;
}

gfx::Rect WindowSizer::GetDefaultWindowBounds(
    const display::Display& display) const {
  gfx::Rect work_area = display.work_area();

  int default_width = 800;
  int default_height = 600;
  nw::Package* package = nw::package();
  if (package && package->window()) {
    package->window()->GetInteger("width", &default_width);
    package->window()->GetInteger("height", &default_height);
  }

#if 0
  // The default size is either some reasonably wide width, or if the work
  // area is narrower, then the work area width less some aesthetic padding.
  int default_width = std::min(work_area.width() - 2 * kWindowTilePixels,
                               kWindowMaxDefaultWidth);
  int default_height = work_area.height() - 2 * kWindowTilePixels;

#if !BUILDFLAG(IS_MAC)
  // For wider aspect ratio displays at higher resolutions, we might size the
  // window narrower to allow two windows to easily be placed side-by-side.
  gfx::Rect screen_size =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  double width_to_height =
    static_cast<double>(screen_size.width()) / screen_size.height();

  // The least wide a screen can be to qualify for the halving described above.
  static const int kMinScreenWidthForWindowHalving = 1600;
  // We assume 16:9/10 is a fairly standard indicator of a wide aspect ratio
  // computer display.
  if (((width_to_height * 10) >= 16) &&
      work_area.width() > kMinScreenWidthForWindowHalving) {
    // Halve the work area, subtracting aesthetic padding on either side.
    // The padding is set so that two windows, side by side have
    // kWindowTilePixels between screen edge and each other.
    default_width = static_cast<int>(work_area.width() / 2. -
        1.5 * kWindowTilePixels);
  }
#endif  // !BUILDFLAG(IS_MAC)
#endif
  return gfx::Rect(kWindowTilePixels + work_area.x(),
                   kWindowTilePixels + work_area.y(),
                   default_width, default_height);
}

void WindowSizer::AdjustBoundsToBeVisibleOnDisplay(
    const display::Display& display,
    const gfx::Rect& saved_work_area,
    gfx::Rect* bounds) const {
  DCHECK(bounds);

  // If |bounds| is empty, reset to the default size.
  if (bounds->IsEmpty()) {
    gfx::Rect default_bounds = GetDefaultWindowBounds(display);
    if (bounds->height() <= 0)
      bounds->set_height(default_bounds.height());
    if (bounds->width() <= 0)
      bounds->set_width(default_bounds.width());
  }

  // Ensure the minimum height and width.
  bounds->set_height(std::max(kMinVisibleHeight, bounds->height()));
  bounds->set_width(std::max(kMinVisibleWidth, bounds->width()));

  gfx::Rect work_area = display.work_area();
  // Ensure that the title bar is not above the work area.
  if (bounds->y() < work_area.y())
    bounds->set_y(work_area.y());

  // Reposition and resize the bounds if the saved_work_area is different from
  // the current work area and the current work area doesn't completely contain
  // the bounds.
  if (!saved_work_area.IsEmpty() &&
      saved_work_area != work_area &&
      !work_area.Contains(*bounds)) {
    bounds->set_width(std::min(bounds->width(), work_area.width()));
    bounds->set_height(std::min(bounds->height(), work_area.height()));
    // TODO(crbug.com/1235666): Make sure these use correct ranges (lo <= hi)
    // and migrate to base::clamp().
    bounds->set_x(BrokenClampThatShouldNotBeUsed(
        bounds->x(), work_area.x(), work_area.right() - bounds->width()));
    bounds->set_y(BrokenClampThatShouldNotBeUsed(
        bounds->y(), work_area.y(), work_area.bottom() - bounds->height()));
  }

#if BUILDFLAG(IS_MAC)
  // Limit the maximum height.  On the Mac the sizer is on the
  // bottom-right of the window, and a window cannot be moved "up"
  // past the menubar.  If the window is too tall you'll never be able
  // to shrink it again.  Windows does not have this limitation
  // (e.g. can be resized from the top).
  bounds->set_height(std::min(work_area.height(), bounds->height()));

  // On mac, we want to be aggressive about repositioning windows that are
  // partially offscreen.  If the window is partially offscreen horizontally,
  // move it to be flush with the left edge of the work area.
  if (bounds->x() < work_area.x() || bounds->right() > work_area.right())
    bounds->set_x(work_area.x());

  // If the window is partially offscreen vertically, move it to be flush with
  // the top of the work area.
  if (bounds->y() < work_area.y() || bounds->bottom() > work_area.bottom())
    bounds->set_y(work_area.y());
#else
  // On non-Mac platforms, we are less aggressive about repositioning.  Simply
  // ensure that at least kMinVisibleWidth * kMinVisibleHeight is visible.
  const int min_y = work_area.y() + kMinVisibleHeight - bounds->height();
  const int min_x = work_area.x() + kMinVisibleWidth - bounds->width();
  const int max_y = work_area.bottom() - kMinVisibleHeight;
  const int max_x = work_area.right() - kMinVisibleWidth;
  // TODO(crbug.com/1235666): Make sure these use correct ranges (lo <= hi)
  // and migrate to base::clamp().
  bounds->set_y(BrokenClampThatShouldNotBeUsed(bounds->y(), min_y, max_y));
  bounds->set_x(BrokenClampThatShouldNotBeUsed(bounds->x(), min_x, max_x));
#endif  // BUILDFLAG(IS_MAC)
}

// static
ui::WindowShowState WindowSizer::GetWindowDefaultShowState(
    const Browser* browser) {
  if (!browser)
    return ui::SHOW_STATE_DEFAULT;

  // Only tabbed browsers and dev tools use the command line.
  bool use_command_line =
      browser->is_type_normal() || browser->is_type_devtools();

#if defined(USE_AURA)
  // We use the apps save state as well on aura.
  use_command_line = use_command_line || browser->is_type_app() ||
                     browser->is_type_app_popup();
#endif

  if (use_command_line && base::CommandLine::ForCurrentProcess()->HasSwitch(
                              switches::kStartMaximized)) {
    return ui::SHOW_STATE_MAXIMIZED;
  }

  return browser->initial_show_state();
}

// static
display::Display WindowSizer::GetDisplayForNewWindow(const gfx::Rect& bounds) {
#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
  // Prefer the display where the user last activated a window.
  return display::Screen::GetScreen()->GetDisplayForNewWindows();
#else
  return display::Screen::GetScreen()->GetDisplayMatching(bounds);
#endif  // BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
}
