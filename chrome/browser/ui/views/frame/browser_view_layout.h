// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_LAYOUT_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_LAYOUT_H_

#include <memory>

#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/views/layout/layout_manager.h"

class BookmarkBarView;
class BrowserView;
class BrowserViewLayoutDelegate;
class ImmersiveModeController;
class InfoBarContainerView;
class TabStrip;
class TabStripRegionView;

namespace gfx {
class Point;
}  // namespace gfx

namespace views {
class View;
class Widget;
}  // namespace views

namespace web_modal {
class WebContentsModalDialogHost;
}

// The layout manager used in chrome browser.
class BrowserViewLayout : public views::LayoutManager {
 public:
  // The minimum width for the normal (tabbed or web app) browser window's
  // contents area. This should be wide enough that WebUI pages (e.g.
  // chrome://settings) and the various associated WebUI dialogs (e.g. Import
  // Bookmarks) can still be functional. This value provides a trade-off between
  // browser usability and privacy - specifically, the ability to browse in a
  // very small window, even on large monitors (which is why a minimum height is
  // not specified). This value is used for the main browser window only, not
  // for popups.
  static constexpr int kMainBrowserContentsMinimumWidth = 500;

  // |browser_view| may be null in tests.
  BrowserViewLayout(std::unique_ptr<BrowserViewLayoutDelegate> delegate,
                    gfx::NativeView host_view,
                    BrowserView* browser_view,
                    views::View* top_container,
                    TabStripRegionView* tab_strip_region_view,
                    TabStrip* tab_strip,
                    views::View* toolbar,
                    InfoBarContainerView* infobar_container,
                    views::View* contents_container,
                    views::View* side_search_side_panel,
                    views::View* left_aligned_side_panel_separator,
                    views::View* right_aligned_side_panel,
                    views::View* right_aligned_side_panel_separator,
                    views::View* lens_side_panel,
                    ImmersiveModeController* immersive_mode_controller,
                    views::View* contents_separator);

  BrowserViewLayout(const BrowserViewLayout&) = delete;
  BrowserViewLayout& operator=(const BrowserViewLayout&) = delete;

  ~BrowserViewLayout() override;

  // Sets or updates views that are not available when |this| is initialized.
  void set_tab_strip(TabStrip* tab_strip) { tab_strip_ = tab_strip; }
  void set_webui_tab_strip(views::View* webui_tab_strip) {
    webui_tab_strip_ = webui_tab_strip;
  }
  void set_loading_bar(views::View* loading_bar) { loading_bar_ = loading_bar; }
  void set_bookmark_bar(BookmarkBarView* bookmark_bar) {
    bookmark_bar_ = bookmark_bar;
  }
  void set_download_shelf(views::View* download_shelf) {
    download_shelf_ = download_shelf;
  }
  void set_contents_border_widget(views::Widget* contents_border_widget) {
    contents_border_widget_ = contents_border_widget;
  }
  views::Widget* contents_border_widget() { return contents_border_widget_; }

  // Sets the bounds for the contents border.
  // * If nullopt, no specific bounds are set, and the border will be drawn
  //   around the entire contents area.
  // * Otherwise, the blue border will be drawn around the indicated Rect,
  //   which is in View coordinates.
  // Note that *whether* the border is drawn is an orthogonal issue;
  // this function only controls where it's drawn when it is in fact drawn.
  void SetContentBorderBounds(
      const absl::optional<gfx::Rect>& region_capture_rect);
  void set_menu_bar(views::View* menu_bar) { menu_bar_ = menu_bar; }
  views::View* menu_bar() { return menu_bar_; }

  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHost();

  // Returns the view against which the dialog is positioned and parented.
  gfx::NativeView GetHostView();

  // Tests to see if the specified |point| (in nonclient view's coordinates)
  // is within the views managed by the laymanager. Returns one of
  // HitTestCompat enum defined in ui/base/hit_test.h.
  // See also ClientView::NonClientHitTest.
  int NonClientHitTest(const gfx::Point& point);

  // views::LayoutManager overrides:
  void Layout(views::View* host) override;
  gfx::Size GetMinimumSize(const views::View* host) const override;
  gfx::Size GetPreferredSize(const views::View* host) const override;

  // Returns true if an infobar is showing.
  bool IsInfobarVisible() const;

 private:
  FRIEND_TEST_ALL_PREFIXES(BrowserViewLayoutTest, BrowserViewLayout);
  FRIEND_TEST_ALL_PREFIXES(BrowserViewLayoutTest, Layout);
  FRIEND_TEST_ALL_PREFIXES(BrowserViewLayoutTest, LayoutDownloadShelf);
  class WebContentsModalDialogHostViews;

  // Layout the following controls, starting at |top|, returns the coordinate
  // of the bottom of the control, for laying out the next control.
  int LayoutTabStripRegion(int top);
  int LayoutWebUITabStrip(int top);
  int LayoutToolbar(int top);
  int LayoutBookmarkAndInfoBars(int top, int browser_view_y);
  int LayoutBookmarkBar(int top);
  int LayoutInfoBar(int top);

  // Layout the |contents_container_| view between the coordinates |top| and
  // |bottom|. See browser_view.h for details of the relationship between
  // |contents_container_| and other views.
  void LayoutContentsContainerView(int top, int bottom);

  // Layout the `side_panel`. This updates the passed in
  // `contents_container_bounds` to accommodate the side panel.
  void LayoutSidePanelView(views::View* side_panel,
                           gfx::Rect& contents_container_bounds);

  // Updates |top_container_|'s bounds. The new bounds depend on the size of
  // the bookmark bar and the toolbar.
  void UpdateTopContainerBounds();

  // Layout the Download Shelf, returns the coordinate of the top of the
  // control, for laying out the previous control.
  int LayoutDownloadShelf(int bottom);

  // Layout the contents border, which indicates the tab is being captured.
  void LayoutContentBorder();

  // Returns the y coordinate of the client area.
  int GetClientAreaTop();

  // The delegate interface. May be a mock in tests.
  const std::unique_ptr<BrowserViewLayoutDelegate> delegate_;

  // The view against which the web dialog is positioned and parented.
  gfx::NativeView const host_view_;

  // The owning browser view.
  const raw_ptr<BrowserView> browser_view_;

  // Child views that the layout manager manages.
  // NOTE: If you add a view, try to add it as a views::View, which makes
  // testing much easier.
  const raw_ptr<views::View> top_container_;
  const raw_ptr<TabStripRegionView> tab_strip_region_view_;
  views::View* menu_bar_;
  const raw_ptr<views::View> toolbar_;
  const raw_ptr<InfoBarContainerView> infobar_container_;
  const raw_ptr<views::View> contents_container_;
  const raw_ptr<views::View> side_search_side_panel_;
  const raw_ptr<views::View> left_aligned_side_panel_separator_;
  const raw_ptr<views::View> right_aligned_side_panel_;
  const raw_ptr<views::View> right_aligned_side_panel_separator_;
  const raw_ptr<views::View> lens_side_panel_;
  const raw_ptr<ImmersiveModeController> immersive_mode_controller_;
  const raw_ptr<views::View> contents_separator_;

  raw_ptr<views::View> webui_tab_strip_ = nullptr;
  raw_ptr<views::View> loading_bar_ = nullptr;
  raw_ptr<TabStrip> tab_strip_ = nullptr;
  raw_ptr<BookmarkBarView> bookmark_bar_ = nullptr;
  raw_ptr<views::View> download_shelf_ = nullptr;

  // The widget displaying a border on top of contents container for
  // highlighting the content. Not created by default.
  raw_ptr<views::Widget> contents_border_widget_ = nullptr;

  // The bounds within which the vertically-stacked contents of the BrowserView
  // should be laid out within. This is just the local bounds of the
  // BrowserView.
  // TODO(jamescook): Remove this and just use browser_view_->GetLocalBounds().
  gfx::Rect vertical_layout_rect_;

  // The host for use in positioning the web contents modal dialog.
  std::unique_ptr<WebContentsModalDialogHostViews> dialog_host_;

  // The latest dialog bounds applied during a layout pass.
  gfx::Rect latest_dialog_bounds_;

  // The latest contents bounds applied during a layout pass, in screen
  // coordinates.
  gfx::Rect latest_contents_bounds_;

  // Directly tied to SetContentBorderBounds() - more details there.
  absl::optional<gfx::Rect> dynamic_content_border_bounds_;

  // The distance the web contents modal dialog is from the top of the window,
  // in pixels.
  int web_contents_modal_dialog_top_y_ = -1;
};

#endif  // CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_LAYOUT_H_
