// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/accelerator_table.h"

#include <stddef.h>

#include "base/containers/contains.h"
#include "base/feature_list.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/app/chrome_command_ids.h"
#include "printing/buildflags/buildflags.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/ui_base_features.h"
#include "ui/events/event_constants.h"

namespace {

// NOTE: Between each ifdef block, keep the list in the same
// (mostly-alphabetical) order as the Windows accelerators in
// ../../app/chrome_dll.rc.
// Do not use Ctrl-Alt as a shortcut modifier, as it is used by i18n keyboards:
// http://blogs.msdn.com/b/oldnewthing/archive/2004/03/29/101121.aspx
const AcceleratorMapping kAcceleratorMap[] = {
// To add an accelerator to macOS that uses modifier keys, either:
//   1) Update the main menu built in main_menu_builder.mm to include a new menu
//      item with the appropriate modifier.
//   2) Update GetShortcutsNotPresentInMainMenu() in
//      global_keyboard_shortcuts_mac.mm.
    { ui::VKEY_F4, ui::EF_ALT_DOWN, IDC_CLOSE_WINDOW },
#if 0
    {ui::VKEY_F7, ui::EF_NONE, IDC_CARET_BROWSING_TOGGLE},
#endif
#if defined(NWJS_SDK)
    {ui::VKEY_F12, ui::EF_NONE, IDC_DEV_TOOLS_TOGGLE},
#endif
#if 0
    {ui::VKEY_ESCAPE, ui::EF_NONE, IDC_CLOSE_FIND_OR_STOP},

#if !BUILDFLAG(IS_MAC)
    {ui::VKEY_D, ui::EF_PLATFORM_ACCELERATOR, IDC_BOOKMARK_THIS_TAB},
    {ui::VKEY_D, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_BOOKMARK_ALL_TABS},
    {ui::VKEY_W, ui::EF_PLATFORM_ACCELERATOR, IDC_CLOSE_TAB},
    {ui::VKEY_W, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_CLOSE_WINDOW},
    {ui::VKEY_F, ui::EF_PLATFORM_ACCELERATOR, IDC_FIND},
    {ui::VKEY_A, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_TAB_SEARCH},
    {ui::VKEY_G, ui::EF_PLATFORM_ACCELERATOR, IDC_FIND_NEXT},
    {ui::VKEY_G, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_FIND_PREVIOUS},
    {ui::VKEY_L, ui::EF_PLATFORM_ACCELERATOR, IDC_FOCUS_LOCATION},
    {ui::VKEY_O, ui::EF_PLATFORM_ACCELERATOR, IDC_OPEN_FILE},
    {ui::VKEY_P, ui::EF_PLATFORM_ACCELERATOR, IDC_PRINT},
    {ui::VKEY_R, ui::EF_PLATFORM_ACCELERATOR, IDC_RELOAD},
    {ui::VKEY_R, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_RELOAD_BYPASSING_CACHE},
    {ui::VKEY_S, ui::EF_PLATFORM_ACCELERATOR, IDC_SAVE_PAGE},
    {ui::VKEY_9, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_LAST_TAB},
    {ui::VKEY_NUMPAD9, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_LAST_TAB},
// TODO(crbug.com/1052397): Revisit the macro expression once build flag switch
// of lacros-chrome is complete.
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS_LACROS)
    {ui::VKEY_9, ui::EF_ALT_DOWN, IDC_SELECT_LAST_TAB},
    {ui::VKEY_NUMPAD9, ui::EF_ALT_DOWN, IDC_SELECT_LAST_TAB},
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS_LACROS)
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS_LACROS) || BUILDFLAG(IS_WIN)
    {ui::VKEY_NEXT, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN, IDC_MOVE_TAB_NEXT},
    {ui::VKEY_PRIOR, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN,
     IDC_MOVE_TAB_PREVIOUS},
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS_LACROS) ||
        // BUILDFLAG(IS_WIN)
    // Control modifier is rarely used on Mac, so we allow it only in several
    // specific cases.
    {ui::VKEY_TAB, ui::EF_CONTROL_DOWN, IDC_SELECT_NEXT_TAB},
    {ui::VKEY_NEXT, ui::EF_CONTROL_DOWN, IDC_SELECT_NEXT_TAB},
    {ui::VKEY_TAB, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_SELECT_PREVIOUS_TAB},
    {ui::VKEY_PRIOR, ui::EF_CONTROL_DOWN, IDC_SELECT_PREVIOUS_TAB},
    {ui::VKEY_1, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_0},
    {ui::VKEY_NUMPAD1, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_0},
    {ui::VKEY_2, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_1},
    {ui::VKEY_NUMPAD2, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_1},
    {ui::VKEY_3, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_2},
    {ui::VKEY_NUMPAD3, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_2},
    {ui::VKEY_4, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_3},
    {ui::VKEY_NUMPAD4, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_3},
    {ui::VKEY_5, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_4},
    {ui::VKEY_NUMPAD5, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_4},
    {ui::VKEY_6, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_5},
    {ui::VKEY_NUMPAD6, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_5},
    {ui::VKEY_7, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_6},
    {ui::VKEY_NUMPAD7, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_6},
    {ui::VKEY_8, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_7},
    {ui::VKEY_NUMPAD8, ui::EF_PLATFORM_ACCELERATOR, IDC_SELECT_TAB_7},
// TODO(crbug.com/1052397): Revisit the macro expression once build flag switch
// of lacros-chrome is complete.
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS_LACROS)
    {ui::VKEY_1, ui::EF_ALT_DOWN, IDC_SELECT_TAB_0},
    {ui::VKEY_NUMPAD1, ui::EF_ALT_DOWN, IDC_SELECT_TAB_0},
    {ui::VKEY_2, ui::EF_ALT_DOWN, IDC_SELECT_TAB_1},
    {ui::VKEY_NUMPAD2, ui::EF_ALT_DOWN, IDC_SELECT_TAB_1},
    {ui::VKEY_3, ui::EF_ALT_DOWN, IDC_SELECT_TAB_2},
    {ui::VKEY_NUMPAD3, ui::EF_ALT_DOWN, IDC_SELECT_TAB_2},
    {ui::VKEY_4, ui::EF_ALT_DOWN, IDC_SELECT_TAB_3},
    {ui::VKEY_NUMPAD4, ui::EF_ALT_DOWN, IDC_SELECT_TAB_3},
    {ui::VKEY_5, ui::EF_ALT_DOWN, IDC_SELECT_TAB_4},
    {ui::VKEY_NUMPAD5, ui::EF_ALT_DOWN, IDC_SELECT_TAB_4},
    {ui::VKEY_6, ui::EF_ALT_DOWN, IDC_SELECT_TAB_5},
    {ui::VKEY_NUMPAD6, ui::EF_ALT_DOWN, IDC_SELECT_TAB_5},
    {ui::VKEY_7, ui::EF_ALT_DOWN, IDC_SELECT_TAB_6},
    {ui::VKEY_NUMPAD7, ui::EF_ALT_DOWN, IDC_SELECT_TAB_6},
    {ui::VKEY_8, ui::EF_ALT_DOWN, IDC_SELECT_TAB_7},
    {ui::VKEY_NUMPAD8, ui::EF_ALT_DOWN, IDC_SELECT_TAB_7},
    {ui::VKEY_BROWSER_FAVORITES, ui::EF_NONE, IDC_SHOW_BOOKMARK_BAR},
#endif  // BUILDFLAG(IS_LINUX) && !BUILDFLAG(IS_CHROMEOS)
    {ui::VKEY_B, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_SHOW_BOOKMARK_BAR},
    {ui::VKEY_OEM_MINUS, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_MINUS},
    {ui::VKEY_SUBTRACT, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_MINUS},
    {ui::VKEY_0, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_NORMAL},
    {ui::VKEY_NUMPAD0, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_NORMAL},
    {ui::VKEY_OEM_PLUS, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_PLUS},
    {ui::VKEY_ADD, ui::EF_PLATFORM_ACCELERATOR, IDC_ZOOM_PLUS},

    {ui::VKEY_F1, ui::EF_NONE, IDC_HELP_PAGE_VIA_KEYBOARD},
    {ui::VKEY_F3, ui::EF_NONE, IDC_FIND_NEXT},
    {ui::VKEY_F3, ui::EF_SHIFT_DOWN, IDC_FIND_PREVIOUS},
    {ui::VKEY_F4, ui::EF_CONTROL_DOWN, IDC_CLOSE_TAB},
    {ui::VKEY_F4, ui::EF_ALT_DOWN, IDC_CLOSE_WINDOW},
    {ui::VKEY_F5, ui::EF_NONE, IDC_RELOAD},
    {ui::VKEY_F5, ui::EF_CONTROL_DOWN, IDC_RELOAD_BYPASSING_CACHE},
    {ui::VKEY_F5, ui::EF_SHIFT_DOWN, IDC_RELOAD_BYPASSING_CACHE},
    {ui::VKEY_F6, ui::EF_NONE, IDC_FOCUS_NEXT_PANE},
    {ui::VKEY_F6, ui::EF_SHIFT_DOWN, IDC_FOCUS_PREVIOUS_PANE},
    {ui::VKEY_F6, ui::EF_CONTROL_DOWN, IDC_FOCUS_WEB_CONTENTS_PANE},
    {ui::VKEY_F10, ui::EF_NONE, IDC_FOCUS_MENU_BAR},
    {ui::VKEY_F11, ui::EF_NONE, IDC_FULLSCREEN},
    {ui::VKEY_M, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_SHOW_AVATAR_MENU},

// Platform-specific key maps.
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    {ui::VKEY_BROWSER_BACK, ui::EF_NONE, IDC_BACK},
    {ui::VKEY_BROWSER_FORWARD, ui::EF_NONE, IDC_FORWARD},
    {ui::VKEY_BROWSER_HOME, ui::EF_NONE, IDC_HOME},
    {ui::VKEY_BROWSER_REFRESH, ui::EF_NONE, IDC_RELOAD},
    {ui::VKEY_BROWSER_REFRESH, ui::EF_CONTROL_DOWN, IDC_RELOAD_BYPASSING_CACHE},
    {ui::VKEY_BROWSER_REFRESH, ui::EF_SHIFT_DOWN, IDC_RELOAD_BYPASSING_CACHE},
    {ui::VKEY_CLOSE, ui::EF_NONE, IDC_CLOSE_TAB},
    {ui::VKEY_NEW, ui::EF_NONE, IDC_NEW_TAB},
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(IS_CHROMEOS)
    // Chrome OS supports the print key, however XKB conflates the print
    // and printscreen keys together so it is not supported on Linux.
    // See crbug.com/683097
    {ui::VKEY_PRINT, ui::EF_NONE, IDC_PRINT},
#endif  // BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS)
    // Chrome OS keyboard does not have delete key, so assign it to backspace.
    {ui::VKEY_BACK, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_CLEAR_BROWSING_DATA},
#else  // !(BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS))
    {ui::VKEY_DELETE, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_CLEAR_BROWSING_DATA},
#endif  // !(BUILDFLAG(IS_CHROMEOS_ASH) || BUILDFLAG(IS_CHROMEOS_LACROS))

#if BUILDFLAG(IS_CHROMEOS_ASH)
    // On Chrome OS, VKEY_BROWSER_SEARCH is handled in Ash.
    {ui::VKEY_OEM_2, ui::EF_CONTROL_DOWN, IDC_HELP_PAGE_VIA_KEYBOARD},
    {ui::VKEY_OEM_2, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_HELP_PAGE_VIA_KEYBOARD},
    {ui::VKEY_BROWSER_FAVORITES, ui::EF_NONE, IDC_SHOW_BOOKMARK_MANAGER},
    {ui::VKEY_BROWSER_STOP, ui::EF_NONE, IDC_STOP},
    // On Chrome OS, Search + Esc is used to call out task manager.
    {ui::VKEY_ESCAPE, ui::EF_COMMAND_DOWN, IDC_TASK_MANAGER},
#else   // BUILDFLAG(IS_CHROMEOS_ASH)
    {ui::VKEY_ESCAPE, ui::EF_SHIFT_DOWN, IDC_TASK_MANAGER},
    {ui::VKEY_LMENU, ui::EF_NONE, IDC_FOCUS_MENU_BAR},
    {ui::VKEY_MENU, ui::EF_NONE, IDC_FOCUS_MENU_BAR},
    {ui::VKEY_RMENU, ui::EF_NONE, IDC_FOCUS_MENU_BAR},
    // On Windows, all VKEY_BROWSER_* keys except VKEY_BROWSER_SEARCH are
    // handled via WM_APPCOMMAND.
    {ui::VKEY_BROWSER_SEARCH, ui::EF_NONE, IDC_FOCUS_SEARCH},
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(GOOGLE_CHROME_BRANDING) && !BUILDFLAG(IS_MAC)
    {ui::VKEY_I, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN, IDC_FEEDBACK},
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING) && !BUILDFLAG(IS_MAC)
    {ui::VKEY_N, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_NEW_INCOGNITO_WINDOW},
    {ui::VKEY_T, ui::EF_PLATFORM_ACCELERATOR, IDC_NEW_TAB},
    {ui::VKEY_N, ui::EF_PLATFORM_ACCELERATOR, IDC_NEW_WINDOW},
    {ui::VKEY_T, ui::EF_SHIFT_DOWN | ui::EF_PLATFORM_ACCELERATOR,
     IDC_RESTORE_TAB},

    // Alt by itself (or with just shift) is never used on Mac since it's used
    // to generate non-ASCII characters. Such commands are given Mac-specific
    // bindings as well. Mapping with just Alt appear here, and should have an
    // alternative mapping in the block above.
    {ui::VKEY_LEFT, ui::EF_ALT_DOWN, IDC_BACK},
    {ui::VKEY_LEFT, ui::EF_ALTGR_DOWN, IDC_BACK},
#if BUILDFLAG(ENABLE_PRINTING)
    {ui::VKEY_P, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN, IDC_BASIC_PRINT},
#endif  // ENABLE_PRINTING
    {ui::VKEY_I, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN, IDC_DEV_TOOLS},
    {ui::VKEY_J, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_DEV_TOOLS_CONSOLE},
    {ui::VKEY_C, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_DEV_TOOLS_INSPECT},
    {ui::VKEY_B, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN, IDC_FOCUS_BOOKMARKS},
    {ui::VKEY_A, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN,
     IDC_FOCUS_INACTIVE_POPUP_FOR_ACCESSIBILITY},
    {ui::VKEY_D, ui::EF_ALT_DOWN, IDC_FOCUS_LOCATION},
    {ui::VKEY_E, ui::EF_CONTROL_DOWN, IDC_FOCUS_SEARCH},
    {ui::VKEY_K, ui::EF_CONTROL_DOWN, IDC_FOCUS_SEARCH},
    {ui::VKEY_T, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN, IDC_FOCUS_TOOLBAR},
    {ui::VKEY_RIGHT, ui::EF_ALT_DOWN, IDC_FORWARD},
    {ui::VKEY_RIGHT, ui::EF_ALTGR_DOWN, IDC_FORWARD},
    {ui::VKEY_HOME, ui::EF_ALT_DOWN, IDC_HOME},
    {ui::VKEY_E, ui::EF_ALT_DOWN, IDC_SHOW_APP_MENU},
    {ui::VKEY_F, ui::EF_ALT_DOWN, IDC_SHOW_APP_MENU},
    {ui::VKEY_O, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_SHOW_BOOKMARK_MANAGER},
    {ui::VKEY_J, ui::EF_CONTROL_DOWN, IDC_SHOW_DOWNLOADS},
    {ui::VKEY_H, ui::EF_CONTROL_DOWN, IDC_SHOW_HISTORY},
    {ui::VKEY_U, ui::EF_CONTROL_DOWN, IDC_VIEW_SOURCE},
#if !BUILDFLAG(IS_CHROMEOS_ASH)
    // On Chrome OS, these keys are assigned to change UI scale.
    {ui::VKEY_OEM_MINUS, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
     IDC_ZOOM_MINUS},
    {ui::VKEY_OEM_PLUS, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN, IDC_ZOOM_PLUS},
    // TODO(https://crbug.com/1016439): This is a temporary hotkey. Chrome OS
    // uses this for switching IMEs, but since this feature is only exposed via
    // command line flag at the moment, we'll exclude them entirely for now.
    {ui::VKEY_SPACE, ui::EF_CONTROL_DOWN, IDC_TOGGLE_QUICK_COMMANDS},
#endif  // !BUILDFLAG(IS_CHROMEOS_ASH)
#endif  // !BUILDFLAG(IS_MAC)
#endif
};

#if BUILDFLAG(IS_CHROMEOS_ASH)
// Accelerators to enable if features::IsNewShortcutMappingEnabled is false.
const AcceleratorMapping kDisableWithNewMappingAcceleratorMap[] = {
    // On Chrome OS, Control + Search + 7 toggles caret browsing.
    // Note that VKEY_F7 is not a typo; Search + 7 maps to F7 for accelerators.
    {ui::VKEY_F7, ui::EF_CONTROL_DOWN, IDC_CARET_BROWSING_TOGGLE},
};

// Accelerators to enable if features::IsNewShortcutMappingEnabled is true.
const AcceleratorMapping kEnableWithNewMappingAcceleratorMap[] = {
    // On Chrome OS, Control + Search + 7 toggles caret browsing.
    {ui::VKEY_7, ui::EF_CONTROL_DOWN | ui::EF_COMMAND_DOWN,
     IDC_CARET_BROWSING_TOGGLE},
};
#endif

constexpr int kDebugModifier =
    ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_SHIFT_DOWN;

// Accelerators to enable if features::UIDebugTools is true.
constexpr AcceleratorMapping kUIDebugAcceleratorMap[] = {
    {ui::VKEY_T, kDebugModifier, IDC_DEBUG_TOGGLE_TABLET_MODE},
    {ui::VKEY_V, kDebugModifier, IDC_DEBUG_PRINT_VIEW_TREE},
    {ui::VKEY_M, kDebugModifier, IDC_DEBUG_PRINT_VIEW_TREE_DETAILS},
};

const int kRepeatableCommandIds[] = {
    IDC_FIND_NEXT,           IDC_FIND_PREVIOUS,       IDC_FOCUS_NEXT_PANE,
    IDC_FOCUS_PREVIOUS_PANE, IDC_MOVE_TAB_NEXT,       IDC_MOVE_TAB_PREVIOUS,
    IDC_SELECT_NEXT_TAB,     IDC_SELECT_PREVIOUS_TAB,
};

} // namespace

std::vector<AcceleratorMapping> GetAcceleratorList() {
  static base::NoDestructor<std::vector<AcceleratorMapping>> accelerators;

  if (accelerators->empty()) {
    accelerators->insert(accelerators->begin(), std::begin(kAcceleratorMap),
                         std::end(kAcceleratorMap));

#if BUILDFLAG(IS_CHROMEOS_ASH)
    if (::features::IsNewShortcutMappingEnabled()) {
      accelerators->insert(accelerators->begin(),
                           std::begin(kEnableWithNewMappingAcceleratorMap),
                           std::end(kEnableWithNewMappingAcceleratorMap));
    } else {
      accelerators->insert(accelerators->begin(),
                           std::begin(kDisableWithNewMappingAcceleratorMap),
                           std::end(kDisableWithNewMappingAcceleratorMap));
    }
#endif

    if (base::FeatureList::IsEnabled(features::kUIDebugTools)) {
      accelerators->insert(accelerators->begin(),
                           std::begin(kUIDebugAcceleratorMap),
                           std::end(kUIDebugAcceleratorMap));
    }
  }

  return *accelerators;
}

bool GetStandardAcceleratorForCommandId(int command_id,
                                        ui::Accelerator* accelerator) {
#if BUILDFLAG(IS_MAC)
  // On macOS, the cut/copy/paste accelerators are defined in the main menu
  // built in main_menu_builder.mm and the accelerator is user configurable. All
  // of this is handled by CommandDispatcher.
  NOTREACHED();
  return false;
#else
  // The standard Ctrl-X, Ctrl-V and Ctrl-C are not defined as accelerators
  // anywhere else.
  switch (command_id) {
    case IDC_CUT:
      *accelerator = ui::Accelerator(ui::VKEY_X, ui::EF_PLATFORM_ACCELERATOR);
      return true;
    case IDC_COPY:
      *accelerator = ui::Accelerator(ui::VKEY_C, ui::EF_PLATFORM_ACCELERATOR);
      return true;
    case IDC_PASTE:
      *accelerator = ui::Accelerator(ui::VKEY_V, ui::EF_PLATFORM_ACCELERATOR);
      return true;
  }
  return false;
#endif
}

bool IsCommandRepeatable(int command_id) {
  return base::Contains(kRepeatableCommandIds, command_id);
}
