// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ash/sharesheet/sharesheet_bubble_view.h"

#include <algorithm>
#include <memory>

#include "ash/constants/app_types.h"
#include "ash/frame/non_client_frame_view_ash.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/sharesheet/sharesheet_metrics.h"
#include "chrome/browser/sharesheet/sharesheet_service.h"
#include "chrome/browser/sharesheet/sharesheet_service_factory.h"
#include "chrome/browser/sharesheet/sharesheet_test_util.h"
#include "chrome/browser/sharesheet/sharesheet_types.h"
#include "chrome/browser/ui/ash/sharesheet/sharesheet_bubble_view_delegate.h"
#include "chrome/browser/ui/ash/sharesheet/sharesheet_header_view.h"
#include "chrome/browser/ui/ash/sharesheet/sharesheet_util.h"
#include "chrome/common/chrome_features.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/test/base/chrome_ash_test_base.h"
#include "chrome/test/base/testing_profile.h"
#include "chromeos/components/sharesheet/constants.h"
#include "components/services/app_service/public/cpp/intent_util.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/window.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/clipboard_test_util.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/base_event_utils.h"
#include "ui/lottie/resource.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace {

void Click(const views::View* view) {
  auto* root_window = view->GetWidget()->GetNativeWindow()->GetRootWindow();
  ui::test::EventGenerator event_generator(root_window);
  event_generator.MoveMouseTo(view->GetBoundsInScreen().CenterPoint());
  event_generator.ClickLeftButton();
}

}  // namespace

namespace ash {
namespace sharesheet {

class TestWidgetDelegate : public views::WidgetDelegateView {
 public:
  TestWidgetDelegate() = default;
  ~TestWidgetDelegate() override = default;

  // views::WidgetDelegateView:
  bool CanActivate() const override { return true; }

  std::unique_ptr<views::NonClientFrameView> CreateNonClientFrameView(
      views::Widget* widget) override {
    return std::make_unique<NonClientFrameViewAsh>(widget);
  }
};

class SharesheetBubbleViewTest : public ChromeAshTestBase {
 public:
  SharesheetBubbleViewTest() = default;

  // ChromeAshTestBase:
  void SetUp() override {
    ChromeAshTestBase::SetUp();

    scoped_feature_list_.InitAndEnableFeature(
        features::kSharesheetCopyToClipboard);

    profile_ = std::make_unique<TestingProfile>();

    // Set up parent window for sharesheet to anchor to.
    auto* widget = new views::Widget();
    views::Widget::InitParams params;
    params.delegate = new TestWidgetDelegate();
    params.context = GetContext();
    widget->Init(std::move(params));
    widget->Show();
    widget->GetNativeWindow()->SetProperty(aura::client::kShowStateKey,
                                           ui::SHOW_STATE_FULLSCREEN);
    gfx::Size window_size = widget->GetWindowBoundsInScreen().size();
    auto* content_view = new views::NativeViewHost();
    content_view->SetBounds(0, 0, window_size.width(), window_size.height());
    widget->GetContentsView()->AddChildView(content_view);

    parent_window_ = widget->GetNativeWindow();

    ui::ResourceBundle::SetLottieParsingFunctions(
        &lottie::ParseLottieAsStillImage,
        &lottie::ParseLottieAsThemedStillImage);
  }

  void ShowAndVerifyBubble(apps::mojom::IntentPtr intent,
                           ::sharesheet::LaunchSource source) {
    ::sharesheet::SharesheetService* const sharesheet_service =
        ::sharesheet::SharesheetServiceFactory::GetForProfile(profile_.get());
    sharesheet_service->ShowBubbleForTesting(
        parent_window_, std::move(intent),
        /*contains_hosted_document=*/false, source,
        /*delivered_callback=*/base::DoNothing(),
        /*close_callback=*/base::DoNothing());
    bubble_delegate_ = static_cast<SharesheetBubbleViewDelegate*>(
        sharesheet_service->GetUiDelegateForTesting(parent_window_));
    EXPECT_NE(bubble_delegate_, nullptr);
    sharesheet_bubble_view_ = bubble_delegate_->GetBubbleViewForTesting();
    EXPECT_NE(sharesheet_bubble_view_, nullptr);
    EXPECT_EQ(sharesheet_bubble_view_->GetID(), SHARESHEET_BUBBLE_VIEW_ID);

    ASSERT_TRUE(bubble_delegate_->IsBubbleVisible());
    sharesheet_widget_ = sharesheet_bubble_view_->GetWidget();
    ASSERT_EQ(sharesheet_widget_->GetName(), "SharesheetBubbleView");
    ASSERT_TRUE(IsSharesheetVisible());
  }

  void CloseBubble() {
    bubble_delegate_->CloseBubble(::sharesheet::SharesheetResult::kCancel);
    // |bubble_delegate_| and |sharesheet_bubble_view_| destruct on close.
    bubble_delegate_ = nullptr;
    sharesheet_bubble_view_ = nullptr;

    ASSERT_FALSE(IsSharesheetVisible());
  }

  void CloseBubbleWithEscKey() {
    GetEventGenerator()->PressAndReleaseKey(ui::VKEY_ESCAPE);
    // |bubble_delegate_| and |sharesheet_bubble_view_| destruct on close.
    bubble_delegate_ = nullptr;
    sharesheet_bubble_view_ = nullptr;

    ASSERT_FALSE(IsSharesheetVisible());
  }

  bool IsSharesheetVisible() { return sharesheet_widget_->IsVisible(); }

  SharesheetBubbleView* sharesheet_bubble_view() {
    return sharesheet_bubble_view_;
  }

  views::View* header_view() {
    return sharesheet_bubble_view_->GetViewByID(HEADER_VIEW_ID);
  }

  views::View* body_view() {
    return sharesheet_bubble_view_->GetViewByID(BODY_VIEW_ID);
  }

  views::View* footer_view() {
    return sharesheet_bubble_view_->GetViewByID(FOOTER_VIEW_ID);
  }

  Profile* profile() { return profile_.get(); }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  gfx::NativeWindow parent_window_;
  std::unique_ptr<TestingProfile> profile_;
  SharesheetBubbleViewDelegate* bubble_delegate_;
  SharesheetBubbleView* sharesheet_bubble_view_;
  views::Widget* sharesheet_widget_;
};

TEST_F(SharesheetBubbleViewTest, BubbleDoesOpenAndClose) {
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, EmptyState) {
  ShowAndVerifyBubble(::sharesheet::CreateInvalidIntent(),
                      ::sharesheet::LaunchSource::kUnknown);

  // Header should contain Share label.
  ASSERT_TRUE(header_view()->GetVisible());
  ASSERT_EQ(header_view()->children().size(), 1u);

  // Body view should contain 3 children, an image and 2 labels.
  ASSERT_TRUE(body_view()->GetVisible());
  ASSERT_EQ(body_view()->children().size(), 3u);

  // Footer should be an empty view that just acts as padding.
  ASSERT_TRUE(footer_view()->GetVisible());
  ASSERT_EQ(footer_view()->children().size(), 0u);
}

TEST_F(SharesheetBubbleViewTest, RecordLaunchSource) {
  base::HistogramTester histograms;

  auto source = ::sharesheet::LaunchSource::kFilesAppShareButton;
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(), source);
  CloseBubble();
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetLaunchSourceResultHistogram, source, 1);

  source = ::sharesheet::LaunchSource::kArcNearbyShare;
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(), source);
  CloseBubble();
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetLaunchSourceResultHistogram, source, 1);
}

TEST_F(SharesheetBubbleViewTest, RecordShareActionCount) {
  // Text intent should only show copy action.
  base::HistogramTester histograms;
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubble();
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kDriveAction, 0);
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kCopyAction, 1);

  // Drive intent should show only drive action.
  ShowAndVerifyBubble(::sharesheet::CreateDriveIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubble();
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kDriveAction, 1);
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kCopyAction, 1);

  // Invalid intent should not show any actions.
  ShowAndVerifyBubble(::sharesheet::CreateInvalidIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubble();
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kDriveAction, 1);
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetShareActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kCopyAction, 1);
}

TEST_F(SharesheetBubbleViewTest, ClickCopyToClipboard) {
  base::HistogramTester histograms;
  // Text intent should only show copy action.
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);

  // |targets_view| should only contain the copy to clipboard target.
  views::View* targets_view = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::TARGETS_DEFAULT_VIEW_ID);
  ASSERT_EQ(targets_view->children().size(), 1u);

  // Click on copy target.
  Click(targets_view->children()[0]);

  // Bubble should close after copy target was pressed.
  ASSERT_FALSE(IsSharesheetVisible());

  // Copy to clipboard was clicked on.
  histograms.ExpectBucketCount(
      ::sharesheet::kSharesheetUserActionResultHistogram,
      ::sharesheet::SharesheetMetrics::UserAction::kCopyAction, 1);

  // Check text copied correctly.
  std::u16string clipboard_text;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, /* data_dst = */ nullptr,
      &clipboard_text);
  EXPECT_EQ(::sharesheet::kTestText, base::UTF16ToUTF8(clipboard_text));
}

TEST_F(SharesheetBubbleViewTest, TextPreview) {
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 3 children, the 'Share' title, the text title and the text.
  ASSERT_EQ(text_views->children().size(), 3u);

  auto* share_title_text =
      static_cast<views::Label*>(text_views->children()[0]);
  ASSERT_EQ(share_title_text->GetText(),
            l10n_util::GetStringUTF16(IDS_SHARESHEET_TITLE_LABEL));
  auto* title_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(title_text->GetText(), u"title");
  auto* text = static_cast<views::Label*>(text_views->children()[2]);
  ASSERT_EQ(text->GetText(), u"text");

  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, TextPreviewNoTitle) {
  auto* text = "text";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(text, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the text.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* share_title_text =
      static_cast<views::Label*>(text_views->children()[0]);
  ASSERT_EQ(share_title_text->GetText(),
            l10n_util::GetStringUTF16(IDS_SHARESHEET_TITLE_LABEL));
  auto* text_label = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(text_label->GetText(), base::UTF8ToUTF16(text));

  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, TextPreviewOneFile) {
  storage::FileSystemURL url = ::sharesheet::FileInDownloads(
      profile(), base::FilePath(::sharesheet::kTestTextFile));
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromFiles(
                          {url.ToGURL()}, {::sharesheet::kMimeTypeText}),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the text.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* title_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(title_text->GetText(), u"text.txt");
  ASSERT_EQ(title_text->GetAccessibleName(), u"text.txt");
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, TextPreviewMultipleFiles) {
  storage::FileSystemURL url1 = ::sharesheet::FileInDownloads(
      profile(), base::FilePath(::sharesheet::kTestPdfFile));
  storage::FileSystemURL url2 = ::sharesheet::FileInDownloads(
      profile(), base::FilePath(::sharesheet::kTestTextFile));
  ShowAndVerifyBubble(
      apps_util::CreateShareIntentFromFiles(
          {url1.ToGURL(), url2.ToGURL()},
          {::sharesheet::kMimeTypePdf, ::sharesheet::kMimeTypeText}),
      ::sharesheet::LaunchSource::kUnknown);

  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the text.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* title_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(title_text->GetText(), u"2 files");
  ASSERT_EQ(title_text->GetAccessibleName(), u"2 files file.pdf, text.txt");
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, URLPreviewAverage) {
  auto* kTitleText = "URLTitle";
  auto* kURLText = "https://fake-url.com/fake";
  ShowAndVerifyBubble(
      apps_util::CreateShareIntentFromText(kURLText, kTitleText),
      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 3 children, the 'Share' title, the URL title, and the URL.
  ASSERT_EQ(text_views->children().size(), 3u);

  auto* title_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(title_text->GetText(), base::UTF8ToUTF16(kTitleText));
  auto* url_text = static_cast<views::Label*>(text_views->children()[2]);
  ASSERT_EQ(url_text->GetText(), base::UTF8ToUTF16(kURLText));
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, URLPreviewLongSubDomain) {
  auto* kURLText =
      "https://very-very-very-very-very-very-very-very-long-fake-url.com/fake";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(kURLText, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the URL.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* url_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(url_text->GetText(),
            u"very-very-very-very-very-very-very-very-long-fake-…");
  ASSERT_EQ(url_text->GetTooltipText(), base::UTF8ToUTF16(kURLText));
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, URLPreviewLongSubDirectory) {
  auto* kURLText =
      "https://fake-url.com/very-very-very-very-very-very-very-very-long-fake";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(kURLText, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the URL.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* url_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(url_text->GetText(),
            u"fake-url.com/very-very-very-very-very-very-very-v…");
  ASSERT_EQ(url_text->GetTooltipText(), base::UTF8ToUTF16(kURLText));
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest,
       URLPreviewLongSecondLevelDomainAndLongSubDirectory) {
  auto* kURLText =
      "https://very-very-very-very-very-very-very-very-long.fake-url.com/"
      "very-very-very-very-very-very-very-very-long-fake";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(kURLText, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the URL.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* url_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(url_text->GetText(),
            u"…fake-url.com/very-very-very-very-very-very-very…");
  ASSERT_EQ(url_text->GetTooltipText(), base::UTF8ToUTF16(kURLText));
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, URLPreviewInternationalCharacters) {
  auto* kURLText = "https://xn--p8j9a0d9c9a.xn--q9jyb4c/";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(kURLText, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the URL.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* url_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(url_text->GetText(), u"https://はじめよう.みんな");
  ASSERT_EQ(url_text->GetTooltipText(), u"https://はじめよう.みんな");
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, URLPreviewEmojis) {
  // Text is encoded in IDN.
  auto* kURLText = "https://hello.com/\xF0\x9F\x98\x81/";
  ShowAndVerifyBubble(apps_util::CreateShareIntentFromText(kURLText, ""),
                      ::sharesheet::LaunchSource::kUnknown);
  views::View* text_views = sharesheet_bubble_view()->GetViewByID(
      SharesheetViewID::HEADER_VIEW_TEXT_PREVIEW_ID);
  // There should be 2 children, the 'Share' title, and the URL.
  ASSERT_EQ(text_views->children().size(), 2u);

  auto* url_text = static_cast<views::Label*>(text_views->children()[1]);
  ASSERT_EQ(url_text->GetText(), u"https://hello.com/😁/");
  ASSERT_EQ(url_text->GetTooltipText(), u"https://hello.com/😁/");
  CloseBubble();
}

TEST_F(SharesheetBubbleViewTest, CloseWithEscKey) {
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubbleWithEscKey();
}

TEST_F(SharesheetBubbleViewTest, CloseMultipleTimes) {
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  CloseBubbleWithEscKey();
  CloseBubbleWithEscKey();
}

TEST_F(SharesheetBubbleViewTest, HoldEscapeKey) {
  GetEventGenerator()->PressKey(ui::VKEY_ESCAPE, ui::EventFlags::EF_NONE);
  ShowAndVerifyBubble(::sharesheet::CreateValidTextIntent(),
                      ::sharesheet::LaunchSource::kUnknown);
  GetEventGenerator()->ReleaseKey(ui::VKEY_ESCAPE, ui::EventFlags::EF_NONE);
  CloseBubbleWithEscKey();
}

}  // namespace sharesheet
}  // namespace ash
