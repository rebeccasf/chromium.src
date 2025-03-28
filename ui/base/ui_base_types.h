// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_UI_BASE_TYPES_H_
#define UI_BASE_UI_BASE_TYPES_H_

#include <cstdint>
#include <type_traits>

#include "base/component_export.h"
#include "ui/gfx/geometry/rect.h"

namespace ui {

class Event;

// This enum must be version-skew tolerant. It is persisted to disk by ChromeOS
// full restore, and read from disk by a possibly newer version of chrome. This
// means that it's ok to add new values, but existing values should never be
// changed or removed.
//
// Window "show" state.
enum WindowShowState {
  // A default un-set state.
  SHOW_STATE_DEFAULT = 0,
  SHOW_STATE_NORMAL = 1,
  SHOW_STATE_MINIMIZED = 2,
  SHOW_STATE_MAXIMIZED = 3,
  SHOW_STATE_INACTIVE = 4,  // Views only, not persisted.
  SHOW_STATE_FULLSCREEN = 5,
  SHOW_STATE_HIDDEN = 6,
  SHOW_STATE_END = 7  // The end of show state enum.
};

// Dialog button identifiers used to specify which buttons to show the user.
enum DialogButton {
  DIALOG_BUTTON_NONE = 0,
  DIALOG_BUTTON_OK = 1,
  DIALOG_BUTTON_CANCEL = 2,
  DIALOG_BUTTON_LAST = DIALOG_BUTTON_CANCEL,
};

// Specifies the type of modality applied to a window. Different modal
// treatments may be handled differently by the window manager.
enum ModalType {
  MODAL_TYPE_NONE   = 0,  // Window is not modal.
  MODAL_TYPE_WINDOW = 1,  // Window is modal to its transient parent.
  MODAL_TYPE_CHILD  = 2,  // Window is modal to a child of its transient parent.
  MODAL_TYPE_SYSTEM = 3   // Window is modal to all other windows.
};

// The class of window and its overall z-order. Not all platforms provide this
// level of z-order granularity. For such platforms, which only provide a
// distinction between "normal" and "always on top" windows, any of the values
// here that aren't |kNormal| are treated equally as "always on top".
enum class ZOrderLevel {
  // The default level for windows.
  kNormal = 0,

  // A "floating" window z-ordered above other normal windows.
  //
  // Note this is the traditional _desktop_ concept of a "floating window".
  // Android has a concept of "freeform window mode" in which apps are presented
  // in separate "floating" windows that can be moved and resized by the user.
  // That's not what this is.
  kFloatingWindow,

  // UI elements are used to annotate positions on the screen, and thus must
  // appear above floating windows.
  kFloatingUIElement,

  // There have been horrific security decisions that have been made on the web
  // platform that are now expected behavior and cannot easily be changed. The
  // only way to mitigate problems with these decisions is to inform the user by
  // presenting them with a message that they are in a state that they might not
  // expect, and this message must be presented in a UI that cannot be
  // interfered with or covered up. Thus this level for Security UI that must be
  // Z-ordered in front of everything else. Note that this is useful in
  // situations where window modality (as in ModalType) cannot or should not be
  // used.
  kSecuritySurface,
};

// TODO(varunjain): Remove MENU_SOURCE_NONE (crbug.com/250964)
// A Java counterpart will be generated for this enum.
// GENERATED_JAVA_ENUM_PACKAGE: org.chromium.ui.base
// These are used in histograms, do not remove/renumber entries. Only add at the
// end just before MENU_SOURCE_TYPE_LAST. Also remember to update the
// MenuSourceType enum listing in tools/metrics/histograms/enums.xml.
// Lastly, any new type here needs to be synced with ui_base_types.mojom.
enum MenuSourceType {
  MENU_SOURCE_NONE = 0,
  MENU_SOURCE_MOUSE = 1,
  MENU_SOURCE_KEYBOARD = 2,
  MENU_SOURCE_TOUCH = 3,
  MENU_SOURCE_TOUCH_EDIT_MENU = 4,
  MENU_SOURCE_LONG_PRESS = 5,
  MENU_SOURCE_LONG_TAP = 6,
  MENU_SOURCE_TOUCH_HANDLE = 7,
  MENU_SOURCE_STYLUS = 8,
  MENU_SOURCE_ADJUST_SELECTION = 9,
  MENU_SOURCE_ADJUST_SELECTION_RESET = 10,
  MENU_SOURCE_TYPE_LAST = MENU_SOURCE_ADJUST_SELECTION_RESET
};

// Menu types that are used to position menu windows correctly.
enum class MenuType {
  // A context menu opened either via a right click or a long tap.
  kRootContextMenu = 0,

  // A root non-context menu. Example: The three dot menu.
  kRootMenu,

  // A child menu opened by clicking on a nested menu entry of either
  // |kRootContextMenu| or |kRootParentMenu|.
  kChildMenu,
};

// Where an owned anchored window should be anchored to. Used by such backends
// as Wayland, which doesn't provide clients with on screen coordinates, but
// rather forces them to position children windows relative to toplevel windows.
// They use anchor bounds, anchor position, gravity and constraints to
// reposition such windows if the originally intended position caused the
// surface to be constrained.
enum class OwnedWindowAnchorPosition {
  kNone,
  kTop,
  kBottom,
  kLeft,
  kRight,
  kTopLeft,
  kBottomLeft,
  kTopRight,
  kBottomRight,
};

// What direction an owned window should be positioned relatively to its anchor.
enum class OwnedWindowAnchorGravity {
  kNone,
  kTop,
  kBottom,
  kLeft,
  kRight,
  kTopLeft,
  kBottomLeft,
  kTopRight,
  kBottomRight,
};

// How an owned window can be resized/repositioned by a system compositor.
enum class OwnedWindowConstraintAdjustment : uint32_t {
  kAdjustmentNone = 0,
  kAdjustmentSlideX = 1 << 0,
  kAdjustmentSlideY = 1 << 1,
  kAdjustmentFlipX = 1 << 2,
  kAdjustmentFlipY = 1 << 3,
  kAdjustmentResizeX = 1 << 4,
  kAdjustmentRezizeY = 1 << 5,
};

// Structure that describes anchor for an owned window. Owned windows are the
// windows that must be owned by their context (for example, menus, tooltips),
// and they must be positioned in such a way that a system compositor can
// reposition them using provided anchor. Ozone/Wayland is an example user of
// this.
struct OwnedWindowAnchor {
  gfx::Rect anchor_rect;
  OwnedWindowAnchorPosition anchor_position = OwnedWindowAnchorPosition::kNone;
  OwnedWindowAnchorGravity anchor_gravity = OwnedWindowAnchorGravity::kNone;
  OwnedWindowConstraintAdjustment constraint_adjustment =
      OwnedWindowConstraintAdjustment::kAdjustmentNone;
};

COMPONENT_EXPORT(UI_BASE)
MenuSourceType GetMenuSourceTypeForEvent(const ui::Event& event);

}  // namespace ui

inline constexpr ui::OwnedWindowConstraintAdjustment operator|(
    ui::OwnedWindowConstraintAdjustment l,
    ui::OwnedWindowConstraintAdjustment r) {
  using T = std::underlying_type_t<ui::OwnedWindowConstraintAdjustment>;
  return static_cast<ui::OwnedWindowConstraintAdjustment>(static_cast<T>(l) |
                                                          static_cast<T>(r));
}

inline constexpr ui::OwnedWindowConstraintAdjustment operator&(
    ui::OwnedWindowConstraintAdjustment l,
    ui::OwnedWindowConstraintAdjustment r) {
  using T = std::underlying_type_t<ui::OwnedWindowConstraintAdjustment>;
  return static_cast<ui::OwnedWindowConstraintAdjustment>(static_cast<T>(l) &
                                                          static_cast<T>(r));
}

#endif  // UI_BASE_UI_BASE_TYPES_H_
