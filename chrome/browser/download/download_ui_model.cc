// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/download_ui_model.h"

#include "base/feature_list.h"
#include "base/i18n/rtl.h"
#include "base/observer_list.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/download/download_commands.h"
#include "chrome/browser/download/offline_item_utils.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager_factory.h"
#include "chrome/common/url_constants.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/safe_browsing/buildflags.h"
#include "components/safe_browsing/core/common/features.h"
#include "components/vector_icons/vector_icons.h"
#include "net/base/mime_util.h"
#include "third_party/blink/public/common/mime_util/mime_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/time_format.h"
#include "ui/base/text/bytes_formatting.h"

#if !BUILDFLAG(IS_ANDROID)
#include "chrome/browser/ui/browser.h"
#include "ui/views/vector_icons.h"
#endif

using download::DownloadItem;
using offline_items_collection::FailState;
using safe_browsing::DownloadFileType;

namespace {

// TODO(qinmin): Migrate this description generator to OfflineItemUtils once
// that component gets used to build desktop UI.
std::u16string FailStateDescription(FailState fail_state) {
  int string_id = IDS_DOWNLOAD_INTERRUPTED_STATUS;
  std::u16string status_text;

  switch (fail_state) {
    case FailState::FILE_ACCESS_DENIED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_ACCESS_DENIED;
      break;
    case FailState::FILE_NO_SPACE:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_DISK_FULL;
      break;
    case FailState::FILE_NAME_TOO_LONG:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_PATH_TOO_LONG;
      break;
    case FailState::FILE_TOO_LARGE:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_FILE_TOO_LARGE;
      break;
    case FailState::FILE_VIRUS_INFECTED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_VIRUS;
      break;
    case FailState::FILE_TRANSIENT_ERROR:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_TEMPORARY_PROBLEM;
      break;
    case FailState::FILE_BLOCKED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_BLOCKED;
      break;
    case FailState::FILE_SECURITY_CHECK_FAILED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_SECURITY_CHECK_FAILED;
      break;
    case FailState::FILE_TOO_SHORT:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_FILE_TOO_SHORT;
      break;
    case FailState::FILE_SAME_AS_SOURCE:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_FILE_SAME_AS_SOURCE;
      break;
    case FailState::NETWORK_INVALID_REQUEST:
    case FailState::NETWORK_FAILED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_NETWORK_ERROR;
      break;
    case FailState::NETWORK_TIMEOUT:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_NETWORK_TIMEOUT;
      break;
    case FailState::NETWORK_DISCONNECTED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_NETWORK_DISCONNECTED;
      break;
    case FailState::NETWORK_SERVER_DOWN:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_SERVER_DOWN;
      break;
    case FailState::SERVER_FAILED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_SERVER_PROBLEM;
      break;
    case FailState::SERVER_BAD_CONTENT:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_NO_FILE;
      break;
    case FailState::USER_CANCELED:
      string_id = IDS_DOWNLOAD_STATUS_CANCELLED;
      break;
    case FailState::USER_SHUTDOWN:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_SHUTDOWN;
      break;
    case FailState::CRASH:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_CRASH;
      break;
    case FailState::SERVER_UNAUTHORIZED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_UNAUTHORIZED;
      break;
    case FailState::SERVER_CERT_PROBLEM:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_SERVER_CERT_PROBLEM;
      break;
    case FailState::SERVER_FORBIDDEN:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_FORBIDDEN;
      break;
    case FailState::SERVER_UNREACHABLE:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_UNREACHABLE;
      break;
    case FailState::SERVER_CONTENT_LENGTH_MISMATCH:
      string_id = IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_CONTENT_LENGTH_MISMATCH;
      break;
    case FailState::NO_FAILURE:
      NOTREACHED();
      [[fallthrough]];
    // fallthrough
    case FailState::CANNOT_DOWNLOAD:
    case FailState::NETWORK_INSTABILITY:
    case FailState::SERVER_NO_RANGE:
    case FailState::SERVER_CROSS_ORIGIN_REDIRECT:
    case FailState::FILE_FAILED:
    case FailState::FILE_HASH_MISMATCH:
      string_id = IDS_DOWNLOAD_INTERRUPTED_STATUS;
  }

  status_text = l10n_util::GetStringUTF16(string_id);

  return status_text;
}

}  // namespace

DownloadUIModel::DownloadUIModel()
    : DownloadUIModel::DownloadUIModel(std::make_unique<StatusTextBuilder>()) {}

DownloadUIModel::DownloadUIModel(
    std::unique_ptr<StatusTextBuilderBase> status_text_builder)
    : status_text_builder_(std::move(status_text_builder)) {
  status_text_builder_->SetModel(this);
}

DownloadUIModel::~DownloadUIModel() = default;

void DownloadUIModel::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void DownloadUIModel::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

base::WeakPtr<DownloadUIModel> DownloadUIModel::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

bool DownloadUIModel::HasSupportedImageMimeType() const {
  if (blink::IsSupportedImageMimeType(GetMimeType()))
    return true;

  std::string mime;
  base::FilePath::StringType extension_with_dot =
      GetTargetFilePath().FinalExtension();
  if (!extension_with_dot.empty() &&
      net::GetWellKnownMimeTypeFromExtension(extension_with_dot.substr(1),
                                             &mime) &&
      blink::IsSupportedImageMimeType(mime)) {
    return true;
  }

  return false;
}

std::u16string DownloadUIModel::GetProgressSizesString() const {
  std::u16string size_ratio;
  int64_t size = GetCompletedBytes();
  int64_t total = GetTotalBytes();
  if (total > 0) {
    ui::DataUnits amount_units = ui::GetByteDisplayUnits(total);
    std::u16string simple_size =
        ui::FormatBytesWithUnits(size, amount_units, false);

    // In RTL locales, we render the text "size/total" in an RTL context. This
    // is problematic since a string such as "123/456 MB" is displayed
    // as "MB 123/456" because it ends with an LTR run. In order to solve this,
    // we mark the total string as an LTR string if the UI layout is
    // right-to-left so that the string "456 MB" is treated as an LTR run.
    std::u16string simple_total =
        base::i18n::GetDisplayStringInLTRDirectionality(
            ui::FormatBytesWithUnits(total, amount_units, true));
    size_ratio = l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_SIZES,
                                            simple_size, simple_total);
  } else {
    size_ratio = ui::FormatBytes(size);
  }
  return size_ratio;
}

std::u16string DownloadUIModel::GetStatusText() const {
  return status_text_builder_->GetStatusText(GetState());
}

std::u16string DownloadUIModel::StatusTextBuilderBase::GetStatusText(
    download::DownloadItem::DownloadState state) const {
  DCHECK(model_);
  switch (state) {
    case DownloadItem::IN_PROGRESS:
      return GetInProgressStatusText();
    case DownloadItem::COMPLETE:
      return GetCompletedStatusText();
    case DownloadItem::INTERRUPTED: {
      const FailState fail_state = model_->GetLastFailState();
      if (fail_state != FailState::USER_CANCELED) {
        return GetInterruptedStatusText(fail_state);
      }
    }
      [[fallthrough]];
    case DownloadItem::CANCELLED:
      return l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_CANCELLED);
    case DownloadItem::MAX_DOWNLOAD_STATE:
      NOTREACHED();
      return std::u16string();
  }
}

std::u16string DownloadUIModel::GetTooltipText() const {
  std::u16string tooltip = GetFileNameToReportUser().LossyDisplayName();
  if (GetState() == DownloadItem::INTERRUPTED &&
      GetLastFailState() != FailState::USER_CANCELED) {
    tooltip +=
        u"\n" + status_text_builder_->GetFailStateMessage(GetLastFailState());
  }
  return tooltip;
}

// Ordering of the warning texts should be the same as order in the
// GetDesiredDownloadItemMode() method.
std::u16string DownloadUIModel::GetWarningText(const std::u16string& filename,
                                               size_t* offset) const {
  *offset = std::string::npos;

  if (ShouldShowIncognitoWarning()) {
    return l10n_util::GetStringFUTF16(IDS_PROMPT_INCOGNITO_WARNING, filename,
                                      offset);
  }

  switch (GetMixedContentStatus()) {
    case download::DownloadItem::MixedContentStatus::BLOCK:
      return l10n_util::GetStringFUTF16(
          IDS_PROMPT_DOWNLOAD_MIXED_CONTENT_BLOCKED, filename, offset);
    case download::DownloadItem::MixedContentStatus::WARN:
      return l10n_util::GetStringFUTF16(
          IDS_PROMPT_DOWNLOAD_MIXED_CONTENT_WARNING, filename, offset);
    case download::DownloadItem::MixedContentStatus::UNKNOWN:
    case download::DownloadItem::MixedContentStatus::SAFE:
    case download::DownloadItem::MixedContentStatus::VALIDATED:
    case download::DownloadItem::MixedContentStatus::SILENT_BLOCK:
      break;
  }

  switch (GetDangerType()) {
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_URL:
      return l10n_util::GetStringUTF16(IDS_PROMPT_MALICIOUS_DOWNLOAD_URL);
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE:
      return IsExtensionDownload()
                 ? l10n_util::GetStringUTF16(
                       IDS_PROMPT_DANGEROUS_DOWNLOAD_EXTENSION)
                 : l10n_util::GetStringFUTF16(IDS_PROMPT_DANGEROUS_DOWNLOAD,
                                              filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_HOST:
      return l10n_util::GetStringFUTF16(IDS_PROMPT_MALICIOUS_DOWNLOAD_CONTENT,
                                        filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_ACCOUNT_COMPROMISE: {
      return base::FeatureList::IsEnabled(
                 safe_browsing::kSafeBrowsingCTDownloadWarning)
                 ? l10n_util::GetStringFUTF16(
                       IDS_PROMPT_DANGEROUS_DOWNLOAD_ACCOUNT_COMPROMISE,
                       filename, offset)
                 : l10n_util::GetStringFUTF16(
                       IDS_PROMPT_MALICIOUS_DOWNLOAD_CONTENT, filename, offset);
    }
    case download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT: {
      bool request_ap_verdicts = false;
#if BUILDFLAG(FULL_SAFE_BROWSING)
      request_ap_verdicts =
          safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
              profile())
              ->IsUnderAdvancedProtection();
#endif
      return l10n_util::GetStringFUTF16(
          request_ap_verdicts
              ? IDS_PROMPT_UNCOMMON_DOWNLOAD_CONTENT_IN_ADVANCED_PROTECTION
              : IDS_PROMPT_UNCOMMON_DOWNLOAD_CONTENT,
          filename, offset);
    }
    case download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED:
      return l10n_util::GetStringFUTF16(IDS_PROMPT_DOWNLOAD_CHANGES_SETTINGS,
                                        filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_TOO_LARGE:
      return l10n_util::GetStringFUTF16(IDS_PROMPT_DOWNLOAD_BLOCKED_TOO_LARGE,
                                        filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_PASSWORD_PROTECTED:
      return l10n_util::GetStringFUTF16(
          IDS_PROMPT_DOWNLOAD_BLOCKED_PASSWORD_PROTECTED, filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_WARNING:
      return l10n_util::GetStringFUTF16(
          IDS_PROMPT_DOWNLOAD_SENSITIVE_CONTENT_WARNING, filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_BLOCK:
      return l10n_util::GetStringFUTF16(
          IDS_PROMPT_DOWNLOAD_SENSITIVE_CONTENT_BLOCKED, filename, offset);
    case download::DOWNLOAD_DANGER_TYPE_PROMPT_FOR_SCANNING:
      return l10n_util::GetStringFUTF16(IDS_PROMPT_DEEP_SCANNING, filename,
                                        offset);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_UNSUPPORTED_FILETYPE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_SAFE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_OPENED_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_ASYNC_SCANNING:
    case download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_USER_VALIDATED:
    case download::DOWNLOAD_DANGER_TYPE_ALLOWLISTED_BY_POLICY:
    case download::DOWNLOAD_DANGER_TYPE_MAX:
      break;
  }

  return std::u16string();
}

std::u16string DownloadUIModel::GetWarningConfirmButtonText() const {
  const auto kDangerousFile = download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE;

  int warningConfirmButtonTextId = IDS_CONFIRM_DOWNLOAD;
  if (ShouldShowIncognitoWarning()) {
    warningConfirmButtonTextId = IDS_DOWNLOAD_ANYWAY;
  } else if (GetDangerType() == kDangerousFile && IsExtensionDownload()) {
    warningConfirmButtonTextId = IDS_CONTINUE_EXTENSION_DOWNLOAD;
  }

  return l10n_util::GetStringUTF16(warningConfirmButtonTextId);
}

std::u16string DownloadUIModel::GetShowInFolderText() const {
  std::u16string where = GetWebDriveName();
  if (where.empty()) {
    // "Show in <folder/Finder>"
    return l10n_util::GetStringUTF16(IDS_DOWNLOAD_LINK_SHOW);
  }
  // "Show in <WEB_DRIVE>"
  return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_LINK_SHOW_IN_WEB_DRIVE, where);
}

ContentId DownloadUIModel::GetContentId() const {
  NOTREACHED();
  return ContentId();
}

Profile* DownloadUIModel::profile() const {
  NOTREACHED();
  return nullptr;
}

std::u16string DownloadUIModel::GetTabProgressStatusText() const {
  return std::u16string();
}

int64_t DownloadUIModel::GetCompletedBytes() const {
  return 0;
}

int64_t DownloadUIModel::GetTotalBytes() const {
  return 0;
}

int DownloadUIModel::PercentComplete() const {
  return -1;
}

bool DownloadUIModel::IsDangerous() const {
  return false;
}

bool DownloadUIModel::MightBeMalicious() const {
  return false;
}

bool DownloadUIModel::IsMalicious() const {
  return false;
}

bool DownloadUIModel::IsMixedContent() const {
  return false;
}

bool DownloadUIModel::ShouldShowIncognitoWarning() const {
  return false;
}

bool DownloadUIModel::ShouldAllowDownloadFeedback() const {
  return false;
}

bool DownloadUIModel::ShouldRemoveFromShelfWhenComplete() const {
  return false;
}

bool DownloadUIModel::ShouldShowDownloadStartedAnimation() const {
  return true;
}

bool DownloadUIModel::ShouldShowInShelf() const {
  return true;
}

void DownloadUIModel::SetShouldShowInShelf(bool should_show) {}

bool DownloadUIModel::ShouldShowInBubble() const {
  return ShouldShowInShelf();
}

bool DownloadUIModel::ShouldNotifyUI() const {
  return true;
}

bool DownloadUIModel::WasUINotified() const {
  return false;
}

void DownloadUIModel::SetWasUINotified(bool should_notify) {}

bool DownloadUIModel::ShouldPreferOpeningInBrowser() const {
  return true;
}

void DownloadUIModel::SetShouldPreferOpeningInBrowser(bool preference) {}

DownloadFileType::DangerLevel DownloadUIModel::GetDangerLevel() const {
  return DownloadFileType::NOT_DANGEROUS;
}

void DownloadUIModel::SetDangerLevel(
    DownloadFileType::DangerLevel danger_level) {}

download::DownloadItem::MixedContentStatus
DownloadUIModel::GetMixedContentStatus() const {
  return download::DownloadItem::MixedContentStatus::UNKNOWN;
}

void DownloadUIModel::OpenUsingPlatformHandler() {}

bool DownloadUIModel::IsBeingRevived() const {
  return true;
}

void DownloadUIModel::SetIsBeingRevived(bool is_being_revived) {}

DownloadItem* DownloadUIModel::download() {
  return nullptr;
}

std::u16string DownloadUIModel::GetWebDriveName() const {
  return std::u16string();
}

std::u16string DownloadUIModel::GetWebDriveMessage(bool) const {
  return std::u16string();
}

base::FilePath DownloadUIModel::GetFileNameToReportUser() const {
  return base::FilePath();
}

base::FilePath DownloadUIModel::GetTargetFilePath() const {
  return base::FilePath();
}

void DownloadUIModel::OpenDownload() {
  NOTREACHED();
}

download::DownloadItem::DownloadState DownloadUIModel::GetState() const {
  return download::DownloadItem::IN_PROGRESS;
}

bool DownloadUIModel::IsPaused() const {
  return false;
}

download::DownloadDangerType DownloadUIModel::GetDangerType() const {
  return download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS;
}

bool DownloadUIModel::GetOpenWhenComplete() const {
  return false;
}

bool DownloadUIModel::IsOpenWhenCompleteByPolicy() const {
  return false;
}

bool DownloadUIModel::TimeRemaining(base::TimeDelta* remaining) const {
  return false;
}

base::Time DownloadUIModel::GetStartTime() const {
  return base::Time();
}

base::Time DownloadUIModel::GetEndTime() const {
  return base::Time();
}

bool DownloadUIModel::GetOpened() const {
  return false;
}

void DownloadUIModel::SetOpened(bool opened) {}

bool DownloadUIModel::IsDone() const {
  return false;
}

void DownloadUIModel::Pause() {}

void DownloadUIModel::Resume() {}

void DownloadUIModel::Cancel(bool user_cancel) {}

void DownloadUIModel::Remove() {}

void DownloadUIModel::SetOpenWhenComplete(bool open) {}

FailState DownloadUIModel::GetLastFailState() const {
  return FailState::NO_FAILURE;
}

base::FilePath DownloadUIModel::GetFullPath() const {
  return base::FilePath();
}

bool DownloadUIModel::CanResume() const {
  return false;
}

bool DownloadUIModel::AllDataSaved() const {
  return false;
}

bool DownloadUIModel::GetFileExternallyRemoved() const {
  return false;
}

GURL DownloadUIModel::GetURL() const {
  return GURL();
}

bool DownloadUIModel::HasUserGesture() const {
  return false;
}

GURL DownloadUIModel::GetOriginalURL() const {
  return GURL();
}

bool DownloadUIModel::ShouldPromoteOrigin() const {
  return false;
}

#if !BUILDFLAG(IS_ANDROID)
bool DownloadUIModel::IsCommandEnabled(
    const DownloadCommands* download_commands,
    DownloadCommands::Command command) const {
  switch (command) {
    case DownloadCommands::SHOW_IN_FOLDER:
    case DownloadCommands::OPEN_WHEN_COMPLETE:
    case DownloadCommands::PLATFORM_OPEN:
    case DownloadCommands::ALWAYS_OPEN_TYPE:
    case DownloadCommands::MAX:
      NOTREACHED();
      return false;
    case DownloadCommands::CANCEL:
      return !IsDone();
    case DownloadCommands::PAUSE:
      return !IsDone() && !IsPaused() &&
             GetState() == download::DownloadItem::IN_PROGRESS;
    case DownloadCommands::RESUME:
      return CanResume() &&
             (IsPaused() || GetState() != download::DownloadItem::IN_PROGRESS);
    case DownloadCommands::COPY_TO_CLIPBOARD:
      return download_commands->CanBeCopiedToClipboard();
    case DownloadCommands::DISCARD:
    case DownloadCommands::KEEP:
    case DownloadCommands::LEARN_MORE_SCANNING:
    case DownloadCommands::LEARN_MORE_INTERRUPTED:
    case DownloadCommands::LEARN_MORE_MIXED_CONTENT:
    case DownloadCommands::DEEP_SCAN:
    case DownloadCommands::BYPASS_DEEP_SCANNING:
      return true;
  }
  NOTREACHED();
  return false;
}

bool DownloadUIModel::IsCommandChecked(
    const DownloadCommands* download_commands,
    DownloadCommands::Command command) const {
  switch (command) {
    case DownloadCommands::OPEN_WHEN_COMPLETE:
    case DownloadCommands::ALWAYS_OPEN_TYPE:
    case DownloadCommands::MAX:
      NOTREACHED();
      return false;
    case DownloadCommands::PAUSE:
    case DownloadCommands::RESUME:
      return IsPaused();
    case DownloadCommands::SHOW_IN_FOLDER:
    case DownloadCommands::PLATFORM_OPEN:
    case DownloadCommands::CANCEL:
    case DownloadCommands::DISCARD:
    case DownloadCommands::KEEP:
    case DownloadCommands::LEARN_MORE_SCANNING:
    case DownloadCommands::LEARN_MORE_INTERRUPTED:
    case DownloadCommands::LEARN_MORE_MIXED_CONTENT:
    case DownloadCommands::COPY_TO_CLIPBOARD:
    case DownloadCommands::DEEP_SCAN:
    case DownloadCommands::BYPASS_DEEP_SCANNING:
      return false;
  }
  return false;
}

void DownloadUIModel::ExecuteCommand(DownloadCommands* download_commands,
                                     DownloadCommands::Command command) {
  switch (command) {
    case DownloadCommands::SHOW_IN_FOLDER:
    case DownloadCommands::OPEN_WHEN_COMPLETE:
    case DownloadCommands::ALWAYS_OPEN_TYPE:
    case DownloadCommands::MAX:
      NOTREACHED();
      break;
    case DownloadCommands::PLATFORM_OPEN:
      OpenUsingPlatformHandler();
      break;
    case DownloadCommands::CANCEL:
      Cancel(true /* Cancelled by user */);
      break;
    case DownloadCommands::DISCARD:
      Remove();
      break;
    case DownloadCommands::KEEP:
    case DownloadCommands::LEARN_MORE_SCANNING:
      NOTREACHED();
      break;
    case DownloadCommands::LEARN_MORE_INTERRUPTED:
      download_commands->GetBrowser()->OpenURL(content::OpenURLParams(
          download_commands->GetLearnMoreURLForInterruptedDownload(),
          content::Referrer(), WindowOpenDisposition::NEW_FOREGROUND_TAB,
          ui::PAGE_TRANSITION_LINK, false));
      break;
    case DownloadCommands::LEARN_MORE_MIXED_CONTENT:
      download_commands->GetBrowser()->OpenURL(content::OpenURLParams(
          GURL(chrome::kMixedContentDownloadBlockingLearnMoreUrl),
          content::Referrer(), WindowOpenDisposition::NEW_FOREGROUND_TAB,
          ui::PAGE_TRANSITION_LINK, false));
      break;
    case DownloadCommands::PAUSE:
      Pause();
      break;
    case DownloadCommands::RESUME:
      Resume();
      break;
    case DownloadCommands::COPY_TO_CLIPBOARD:
      download_commands->CopyFileAsImageToClipboard();
      break;
    case DownloadCommands::DEEP_SCAN:
      break;
    case DownloadCommands::BYPASS_DEEP_SCANNING:
      break;
  }
}

DownloadUIModel::BubbleUIInfo::BubbleUIInfo(const std::u16string& summary)
    : has_subpage(true), warning_summary(summary) {}
DownloadUIModel::BubbleUIInfo::BubbleUIInfo(bool has_progress_bar)
    : has_progress_bar(has_progress_bar) {}
DownloadUIModel::BubbleUIInfo::SubpageButton::SubpageButton(
    DownloadCommands::Command command,
    std::u16string label,
    bool is_prominent)
    : command(command), label(label), is_prominent(is_prominent) {}
DownloadUIModel::BubbleUIInfo::BubbleUIInfo() = default;
DownloadUIModel::BubbleUIInfo::~BubbleUIInfo() = default;
DownloadUIModel::BubbleUIInfo::BubbleUIInfo(const BubbleUIInfo& rhs) = default;
DownloadUIModel::BubbleUIInfo& DownloadUIModel::BubbleUIInfo::AddIconAndColor(
    const gfx::VectorIcon& vector_icon,
    ui::ColorId color_id) {
  secondary_color = color_id;
  icon_model_override = &vector_icon;
  return *this;
}
DownloadUIModel::BubbleUIInfo& DownloadUIModel::BubbleUIInfo::AddPrimaryButton(
    DownloadCommands::Command command) {
  has_primary_button = true;
  primary_button_command = command;
  return *this;
}
DownloadUIModel::BubbleUIInfo& DownloadUIModel::BubbleUIInfo::AddCheckbox(
    const std::u16string& label) {
  has_checkbox = true;
  checkbox_label = label;
  return *this;
}
DownloadUIModel::BubbleUIInfo& DownloadUIModel::BubbleUIInfo::AddSubpageButton(
    const std::u16string& label,
    DownloadCommands::Command command,
    bool is_prominent) {
  // The subpage of the bubble supports at most 2 buttons.
  DCHECK(subpage_buttons.size() <= 1);
  subpage_buttons.emplace_back(command, label, is_prominent);
  return *this;
}

DownloadUIModel::BubbleUIInfo&
DownloadUIModel::BubbleUIInfo::SetProgressBarLooping() {
  is_progress_bar_looping = true;
  return *this;
}

DownloadUIModel::BubbleUIInfo DownloadUIModel::GetBubbleUIInfoForInterrupted(
    FailState fail_state) const {
  switch (fail_state) {
    case FailState::FILE_BLOCKED:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_INTERRUPTED_SUBPAGE_SUMMARY_BLOCKED_ORGANIZATION))
          .AddIconAndColor(views::kInfoIcon, ui::kColorAlertHighSeverity);
    case FailState::FILE_NAME_TOO_LONG:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_INTERRUPTED_SUBPAGE_SUMMARY_PATH_TOO_LONG))
          .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                           ui::kColorAlertHighSeverity);
    case FailState::FILE_NO_SPACE:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_INTERRUPTED_SUBPAGE_SUMMARY_DISK_FULL))
          .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                           ui::kColorAlertHighSeverity);
    case FailState::SERVER_UNAUTHORIZED:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_INTERRUPTED_SUBPAGE_SUMMARY_FILE_UNAVAILABLE))
          .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                           ui::kColorAlertHighSeverity);
    // No Retry in these cases.
    case FailState::FILE_TOO_LARGE:
    case FailState::FILE_VIRUS_INFECTED:
    case FailState::FILE_SECURITY_CHECK_FAILED:
    case FailState::FILE_ACCESS_DENIED:
    case FailState::SERVER_FORBIDDEN:
      return DownloadUIModel::BubbleUIInfo(/*has_progress_bar=*/false)
          .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                           ui::kColorAlertHighSeverity);
    // Try retry in these cases, and in the default case.
    case FailState::FILE_SAME_AS_SOURCE:
    case FailState::NETWORK_INVALID_REQUEST:
    case FailState::NETWORK_FAILED:
    case FailState::NETWORK_INSTABILITY:
    case FailState::NETWORK_TIMEOUT:
    case FailState::NETWORK_DISCONNECTED:
    case FailState::NETWORK_SERVER_DOWN:
    case FailState::SERVER_BAD_CONTENT:
    case FailState::USER_CANCELED:
    case FailState::FILE_TRANSIENT_ERROR:
    case FailState::USER_SHUTDOWN:
    case FailState::CRASH:
    case FailState::SERVER_CONTENT_LENGTH_MISMATCH:
    case FailState::CANNOT_DOWNLOAD:
    case FailState::SERVER_NO_RANGE:
    case FailState::SERVER_CROSS_ORIGIN_REDIRECT:
    case FailState::FILE_FAILED:
    case FailState::FILE_HASH_MISMATCH:
    case FailState::SERVER_FAILED:
    case FailState::SERVER_CERT_PROBLEM:
    case FailState::SERVER_UNREACHABLE:
    case FailState::FILE_TOO_SHORT:
      break;
    case FailState::NO_FAILURE:
      return DownloadUIModel::BubbleUIInfo(/*has_progress_bar=*/false);
  }
  // TODO(bhatiarohit): Check if it is possible to retry downloads.
  return DownloadUIModel::BubbleUIInfo(/*has_progress_bar=*/false)
      .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                       ui::kColorAlertHighSeverity);
}

DownloadUIModel::BubbleUIInfo DownloadUIModel::GetBubbleUIInfoForWarning()
    const {
  switch (GetMixedContentStatus()) {
    case download::DownloadItem::MixedContentStatus::BLOCK:
    case download::DownloadItem::MixedContentStatus::WARN:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_WARNING_SUBPAGE_SUMMARY_INSECURE))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertMediumSeverity)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP, /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DownloadItem::MixedContentStatus::UNKNOWN:
    case download::DownloadItem::MixedContentStatus::SAFE:
    case download::DownloadItem::MixedContentStatus::VALIDATED:
    case download::DownloadItem::MixedContentStatus::SILENT_BLOCK:
      break;
  }

  switch (GetDangerType()) {
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE:
      if (IsExtensionDownload()) {
        return DownloadUIModel::BubbleUIInfo(
                   l10n_util::GetStringFUTF16(
                       IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_UNKNOWN_SOURCE,
                       l10n_util::GetStringUTF16(
                           IDS_EXTENSION_WEB_STORE_TITLE)))
            .AddIconAndColor(views::kInfoIcon, ui::kColorAlertMediumSeverity)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
                DownloadCommands::Command::KEEP,
                /*is_prominent=*/false)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
                DownloadCommands::Command::DISCARD,
                /*is_prominent=*/true);
      } else {
        // TODO(crbug.com/1320025): Update the strings to match the UX mock.
        return DownloadUIModel::BubbleUIInfo(
                   l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_MALICIOUS_URL_BLOCKED))
            .AddIconAndColor(views::kInfoIcon, ui::kColorSecondaryForeground)
            .AddPrimaryButton(DownloadCommands::Command::KEEP)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
                DownloadCommands::Command::KEEP,
                /*is_prominent=*/false)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
                DownloadCommands::Command::DISCARD,
                /*is_prominent=*/false);
      }
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_HOST:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_MALICIOUS_URL_BLOCKED))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertHighSeverity)
          .AddPrimaryButton(DownloadCommands::Command::DISCARD)
          .AddCheckbox(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CHECKBOX_BYPASS))
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_MALICIOUS_URL_BLOCKED))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertMediumSeverity)
          .AddPrimaryButton(DownloadCommands::Command::DISCARD)
          .AddCheckbox(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CHECKBOX_BYPASS))
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_PASSWORD_PROTECTED:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_ENCRYPTED))
          .AddIconAndColor(views::kInfoIcon, ui::kColorSecondaryForeground)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_ACCOUNT_COMPROMISE:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_WARNING_SUBPAGE_ACCOUNT_COMPROMISE))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertHighSeverity)
          .AddPrimaryButton(DownloadCommands::Command::DISCARD)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_URL:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_MALWARE))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertHighSeverity)
          .AddPrimaryButton(DownloadCommands::Command::DISCARD)
          .AddCheckbox(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CHECKBOX_BYPASS))
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_TOO_LARGE:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_TOO_BIG))
          .AddIconAndColor(views::kInfoIcon, ui::kColorSecondaryForeground)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);

    case download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT: {
      bool request_ap_verdicts = false;
#if BUILDFLAG(FULL_SAFE_BROWSING)
      request_ap_verdicts =
          safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
              profile())
              ->IsUnderAdvancedProtection();
#endif
      if (request_ap_verdicts) {
        return DownloadUIModel::BubbleUIInfo(
                   l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_ADVANCED_PROTECTION))
            .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                             ui::kColorAlertMediumSeverity)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
                DownloadCommands::Command::KEEP,
                /*is_prominent=*/false)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
                DownloadCommands::Command::DISCARD,
                /*is_prominent=*/true);
      } else {
        return DownloadUIModel::BubbleUIInfo(
                   l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_UNCOMMON_FILE))
            .AddIconAndColor(views::kInfoIcon, ui::kColorAlertMediumSeverity)
            .AddPrimaryButton(DownloadCommands::Command::DISCARD)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
                DownloadCommands::Command::KEEP,
                /*is_prominent=*/false)
            .AddSubpageButton(
                l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
                DownloadCommands::Command::DISCARD,
                /*is_prominent=*/true);
      }
    }
    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_WARNING:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_SENSITIVE_CONTENT))
          .AddIconAndColor(views::kInfoIcon, ui::kColorAlertMediumSeverity)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_CONTINUE),
              DownloadCommands::Command::KEEP,
              /*is_prominent=*/false)
          .AddSubpageButton(
              l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DELETE),
              DownloadCommands::Command::DISCARD,
              /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_BLOCK:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_WARNING_SUBPAGE_SUMMARY_BLOCKED_ORGANIZATION))
          .AddIconAndColor(views::kInfoIcon, ui::kColorAlertMediumSeverity);
    case download::DOWNLOAD_DANGER_TYPE_PROMPT_FOR_SCANNING:
      return DownloadUIModel::BubbleUIInfo(
                 l10n_util::GetStringUTF16(
                     IDS_DOWNLOAD_BUBBLE_SUBPAGE_SUMMARY_DEEP_SCANNING_PROMPT))
          .AddIconAndColor(vector_icons::kNotSecureWarningIcon,
                           ui::kColorAlertMediumSeverity)
          .AddPrimaryButton(DownloadCommands::Command::DEEP_SCAN)
          .AddSubpageButton(l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_OPEN),
                            DownloadCommands::Command::BYPASS_DEEP_SCANNING,
                            /*is_prominent=*/false)
          .AddSubpageButton(l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_SCAN),
                            DownloadCommands::Command::DEEP_SCAN,
                            /*is_prominent=*/true);
    case download::DOWNLOAD_DANGER_TYPE_ASYNC_SCANNING:
      return DownloadUIModel::BubbleUIInfo(/*has_progress_bar=*/true)
          .AddPrimaryButton(DownloadCommands::Command::BYPASS_DEEP_SCANNING)
          .SetProgressBarLooping();
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_UNSUPPORTED_FILETYPE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_SAFE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_OPENED_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_USER_VALIDATED:
    case download::DOWNLOAD_DANGER_TYPE_ALLOWLISTED_BY_POLICY:
    case download::DOWNLOAD_DANGER_TYPE_MAX:
      break;
  }
  bool has_progress_bar = GetState() == DownloadItem::IN_PROGRESS;
  BubbleUIInfo bubble_ui_info = DownloadUIModel::BubbleUIInfo(has_progress_bar);
  if (has_progress_bar) {
    bubble_ui_info.AddPrimaryButton(DownloadCommands::Command::CANCEL);
  }
  return bubble_ui_info;
}

DownloadUIModel::BubbleUIInfo DownloadUIModel::GetBubbleUIInfo() const {
  switch (GetState()) {
    case DownloadItem::IN_PROGRESS:
    case DownloadItem::COMPLETE:
      return GetBubbleUIInfoForWarning();
    case DownloadItem::INTERRUPTED: {
      const FailState fail_state = GetLastFailState();
      if (fail_state != FailState::USER_CANCELED) {
        return GetBubbleUIInfoForInterrupted(fail_state);
      }
    }
      [[fallthrough]];
    case DownloadItem::CANCELLED:
    case DownloadItem::MAX_DOWNLOAD_STATE:
      return DownloadUIModel::BubbleUIInfo(/*has_progress_bar=*/false)
          .AddIconAndColor(vector_icons::kFileDownloadOffIcon,
                           ui::kColorSecondaryForeground);
  }
}
#endif

std::string DownloadUIModel::GetMimeType() const {
  return "text/html";
}

bool DownloadUIModel::IsExtensionDownload() const {
  return false;
}

std::u16string DownloadUIModel::StatusTextBuilder::GetInProgressStatusText()
    const {
  DCHECK_EQ(DownloadItem::IN_PROGRESS, model_->GetState());
  const auto web_drive = model_->GetWebDriveName();

  base::TimeDelta time_remaining;
  // time_remaining is only known if the download isn't paused, and it isn't
  // going to be rerouted to a web drive.
  bool time_remaining_known =
      (!model_->IsPaused() && model_->TimeRemaining(&time_remaining) &&
       web_drive.empty());

  // Indication of progress. (E.g.:"100/200 MB" or "100MB")
  std::u16string size_ratio = model_->GetProgressSizesString();

  // The download is a CRX (app, extension, theme, ...) and it is being unpacked
  // and validated.
  if (model_->AllDataSaved() && model_->IsExtensionDownload()) {
    return l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_CRX_INSTALL_RUNNING);
  }

  // A paused download: "100/120 MB, Paused"
  if (model_->IsPaused()) {
    return l10n_util::GetStringFUTF16(
        IDS_DOWNLOAD_STATUS_IN_PROGRESS, size_ratio,
        l10n_util::GetStringUTF16(IDS_DOWNLOAD_PROGRESS_PAUSED));
  }

  // A download scheduled to be opened when complete: "Opening in 10 secs"
  if (web_drive.empty() && model_->GetOpenWhenComplete()) {
    if (!time_remaining_known)
      return l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_OPEN_WHEN_COMPLETE);

    return l10n_util::GetStringFUTF16(
        IDS_DOWNLOAD_STATUS_OPEN_IN,
        ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_DURATION,
                               ui::TimeFormat::LENGTH_SHORT, time_remaining));
  }

  // In progress download with known time left: "100/120 MB, 10 secs left"
  if (time_remaining_known) {
    return l10n_util::GetStringFUTF16(
        IDS_DOWNLOAD_STATUS_IN_PROGRESS, size_ratio,
        ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_REMAINING,
                               ui::TimeFormat::LENGTH_SHORT, time_remaining));
  }

  const auto completed_bytes = model_->GetCompletedBytes();
  const auto total_bytes = model_->GetTotalBytes();
  if (completed_bytes == 0) {
    // Instead of displaying "0 B" we say "Starting..."
    return l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_STARTING);
  } else if (completed_bytes < total_bytes || total_bytes == 0) {
    // In progress download with no known time left and non-zero completed
    // bytes: "100/120 MB • Resuming..." or "100 MB • Resuming...", or "100/120
    // MB" or "100 MB"
    return size_ratio;
  } else if (web_drive.size()) {
    // If all bytes of the file has been downloaded and it is being rerouted:
    // "Sending to <WEB_DRIVE>..."
    return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_UPLOADING, web_drive);
  } else {
    return std::u16string();
  }
}

std::u16string
DownloadUIModel::BubbleStatusTextBuilder::GetBubbleWarningStatusText() const {
  std::u16string prefix = base::StrCat(
      {l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_STATUS_BLOCKED),
       l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SEPERATOR)});

  switch (model_->GetMixedContentStatus()) {
    case download::DownloadItem::MixedContentStatus::BLOCK:
    case download::DownloadItem::MixedContentStatus::WARN:
      // "Blocked • Insecure download"
      return base::StrCat(
          {prefix, l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_WARNING_STATUS_INSECURE)});
    case download::DownloadItem::MixedContentStatus::UNKNOWN:
    case download::DownloadItem::MixedContentStatus::SAFE:
    case download::DownloadItem::MixedContentStatus::VALIDATED:
    case download::DownloadItem::MixedContentStatus::SILENT_BLOCK:
      break;
  }

  switch (model_->GetDangerType()) {
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE:
      // "Blocked • Unknown source"
      if (model_->IsExtensionDownload())
        return base::StrCat(
            {prefix, l10n_util::GetStringUTF16(
                         IDS_DOWNLOAD_BUBBLE_STATUS_UNKNOWN_SOURCE)});
      [[fallthrough]];
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_HOST:
    case download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED:
      // "Blocked • Dangerous"
      return base::StrCat({prefix, l10n_util::GetStringUTF16(
                                       IDS_DOWNLOAD_BUBBLE_STATUS_DANGEROUS)});

    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_PASSWORD_PROTECTED:
      // "Blocked • Encrypted"
      return base::StrCat({prefix, l10n_util::GetStringUTF16(
                                       IDS_DOWNLOAD_BUBBLE_STATUS_ENCRYPTED)});
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_ACCOUNT_COMPROMISE:
      // "Blocked • Insecure download"
      return base::StrCat(
          {prefix, l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_WARNING_STATUS_INSECURE)});
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_URL:
      // "Blocked • Malware"
      return base::StrCat({prefix, l10n_util::GetStringUTF16(
                                       IDS_DOWNLOAD_BUBBLE_STATUS_MALWARE)});
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_TOO_LARGE:
      // "Blocked • Too big"
      return base::StrCat({prefix, l10n_util::GetStringUTF16(
                                       IDS_DOWNLOAD_BUBBLE_STATUS_TOO_BIG)});
    case download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT: {
      bool request_ap_verdicts = false;
#if BUILDFLAG(FULL_SAFE_BROWSING)
      request_ap_verdicts =
          safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
              model_->profile())
              ->IsUnderAdvancedProtection();
#endif
      // "Blocked by Advanced Protection" or "Blocked • Uncommon file"
      return request_ap_verdicts
                 ? l10n_util::GetStringUTF16(
                       IDS_DOWNLOAD_BUBBLE_STATUS_ADVANCED_PROTECTION)
                 : base::StrCat(
                       {prefix, l10n_util::GetStringUTF16(
                                    IDS_DOWNLOAD_BUBBLE_STATUS_UNCOMMON_FILE)});
    }

    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_WARNING:
      // "Sensitive content"
      return l10n_util::GetStringUTF16(
          IDS_DOWNLOAD_BUBBLE_STATUS_SENSITIVE_CONTENT);
    case download::DOWNLOAD_DANGER_TYPE_SENSITIVE_CONTENT_BLOCK:
      // "Blocked by your organization"
      return l10n_util::GetStringUTF16(
          IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_BLOCKED_ORGANIZATION);
    case download::DOWNLOAD_DANGER_TYPE_PROMPT_FOR_SCANNING:
      return l10n_util::GetStringUTF16(
          IDS_DOWNLOAD_BUBBLE_STATUS_DEEP_SCANNING_PROMPT);
    case download::DOWNLOAD_DANGER_TYPE_ASYNC_SCANNING:
      return l10n_util::GetStringUTF16(
          IDS_DOWNLOAD_BUBBLE_STATUS_ASYNC_SCANNING);
    case download::DOWNLOAD_DANGER_TYPE_BLOCKED_UNSUPPORTED_FILETYPE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_SAFE:
    case download::DOWNLOAD_DANGER_TYPE_DEEP_SCANNED_OPENED_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_USER_VALIDATED:
    case download::DOWNLOAD_DANGER_TYPE_ALLOWLISTED_BY_POLICY:
    case download::DOWNLOAD_DANGER_TYPE_MAX:
      break;
  }

  return std::u16string();
}

std::u16string
DownloadUIModel::BubbleStatusTextBuilder::GetInProgressStatusText() const {
  DCHECK_EQ(DownloadItem::IN_PROGRESS, model_->GetState());

  std::u16string warning_status_text = GetBubbleWarningStatusText();
  if (!warning_status_text.empty())
    return warning_status_text;

  const auto web_drive = model_->GetWebDriveName();
  base::TimeDelta time_remaining;
  // time_remaining is only known if the download isn't paused, and it isn't
  // going to be rerouted to a web drive.
  bool time_remaining_known =
      (!model_->IsPaused() && model_->TimeRemaining(&time_remaining) &&
       web_drive.empty());

  // Indication of progress. (E.g.:"100/200 MB" or "100MB")
  std::u16string size_ratio = model_->GetProgressSizesString();

  std::u16string size_ratio_prefix = base::StrCat(
      {size_ratio,
       l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SEPERATOR)});

  const auto completed_bytes = model_->GetCompletedBytes();
  const auto total_bytes = model_->GetTotalBytes();

  std::u16string total_prefix = base::StrCat(
      {ui::FormatBytes(total_bytes),
       l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SEPERATOR)});

  // The download is a CRX (app, extension, theme, ...) and it is being unpacked
  // and validated.
  if (model_->AllDataSaved() && model_->IsExtensionDownload()) {
    return base::StrCat(
        {total_prefix,
         l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_CRX_INSTALL_RUNNING)});
  }

  // A paused download: "100/120 MB • Paused"
  if (model_->IsPaused()) {
    return base::StrCat({size_ratio_prefix, l10n_util::GetStringUTF16(
                                                IDS_DOWNLOAD_PROGRESS_PAUSED)});
  }

  // A download scheduled to be opened when complete: "↓ 100/120 MB • Opening in
  // 10 secs"
  if (web_drive.empty() && model_->GetOpenWhenComplete()) {
    if (!time_remaining_known)
      return base::StrCat(
          {size_ratio_prefix,
           l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_OPEN_WHEN_COMPLETE)});

    return base::StrCat(
        {l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SYMBOL),
         size_ratio_prefix,
         l10n_util::GetStringFUTF16(
             IDS_DOWNLOAD_STATUS_OPEN_IN,
             ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_DURATION,
                                    ui::TimeFormat::LENGTH_SHORT,
                                    time_remaining))});
  }

  // In progress download with known time left: "↓ 100/120 MB • 10 secs left"
  if (time_remaining_known) {
    return base::StrCat(
        {l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SYMBOL),
         size_ratio_prefix,
         ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_REMAINING,
                                ui::TimeFormat::LENGTH_SHORT, time_remaining)});
  }

  if (completed_bytes == 0) {
    // "0/120 MB • Starting..."
    return base::StrCat({size_ratio_prefix, l10n_util::GetStringUTF16(
                                                IDS_DOWNLOAD_STATUS_STARTING)});
  } else if (completed_bytes < total_bytes || total_bytes == 0) {
    // In progress download with no known time left and non-zero completed
    // bytes: "100/120 MB • Resuming..." or "100 MB • Resuming..."
    return base::StrCat(
        {size_ratio_prefix,
         l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_STATUS_RESUMING)});
  } else if (web_drive.size()) {
    // If all bytes of the file has been downloaded and it is being rerouted:
    // "120 MB • Sending to <WEB_DRIVE>..."
    return base::StrCat(
        {total_prefix,
         l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_UPLOADING, web_drive)});
  } else {
    // "120 MB • Done"
    return base::StrCat({total_prefix, l10n_util::GetStringUTF16(
                                           IDS_DOWNLOAD_BUBBLE_STATUS_DONE)});
  }
}

std::u16string
DownloadUIModel::StatusTextBuilderBase::GetCompletedRemovedOrSavedStatusText()
    const {
  if (model_->GetFileExternallyRemoved()) {
    return l10n_util::GetStringUTF16(IDS_DOWNLOAD_STATUS_REMOVED);
  }
  const auto web_drive = model_->GetWebDriveName();
  if (web_drive.size()) {
    // "Saved to <WEB_DRIVE>"
    return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_UPLOADED, web_drive);
  }
  return std::u16string();
}

std::u16string DownloadUIModel::StatusTextBuilder::GetCompletedStatusText()
    const {
  return GetCompletedRemovedOrSavedStatusText();
}

std::u16string
DownloadUIModel::BubbleStatusTextBuilder::GetCompletedStatusText() const {
  std::u16string warning_status_text = GetBubbleWarningStatusText();
  if (!warning_status_text.empty())
    return warning_status_text;

  std::u16string status_text = GetCompletedRemovedOrSavedStatusText();
  if (!status_text.empty())
    return status_text;

  if (model_->GetEndTime().is_null()) {
    // Offline items have these null.
    return l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_STATUS_DONE);
  } else {
    std::u16string total_text = ui::FormatBytes(model_->GetTotalBytes());
    std::u16string delta_str = ui::TimeFormat::Simple(
        ui::TimeFormat::FORMAT_ELAPSED, ui::TimeFormat::LENGTH_SHORT,
        base::Time::Now() - model_->GetEndTime());
    return base::StrCat(
        {total_text,
         l10n_util::GetStringUTF16(IDS_DOWNLOAD_BUBBLE_DOWNLOAD_SEPERATOR),
         delta_str});
  }
}

// To clarify variable / method names in methods below that help form failure
// status messages:
//                                long & descriptive / short & concise
// "Failed                      - <STATE_DESCRIPTION / STATE_MESSAGE>"
// "Fail to save to <WEB_DRIVE> - <STATE_DESCRIPTION / STATE_MESSAGE>"
// <                     DESCRIPTION/STATUS_TEXT                     >

std::u16string DownloadUIModel::StatusTextBuilderBase::GetFailStateMessage(
    offline_items_collection::FailState fail_state) const {
  std::u16string state_msg;
  if (fail_state != FailState::SERVER_FAILED ||
      (state_msg = model_->GetWebDriveMessage(/* verbose = */ false)).empty()) {
    return OfflineItemUtils::GetFailStateMessage(fail_state);
  }
  return state_msg;
}

void DownloadUIModel::set_clock_for_testing(base::Clock* clock) {
  clock_ = clock;
}

void DownloadUIModel::set_status_text_builder_for_testing(bool for_bubble) {
  if (for_bubble) {
    status_text_builder_ =
        std::make_unique<DownloadUIModel::BubbleStatusTextBuilder>();
  } else {
    status_text_builder_ =
        std::make_unique<DownloadUIModel::StatusTextBuilder>();
  }
  status_text_builder_->SetModel(this);
}

std::u16string DownloadUIModel::GetInterruptDescription() const {
  std::u16string state_description;
  const auto fail_state = GetLastFailState();
  if (fail_state != FailState::SERVER_FAILED ||
      (state_description = GetWebDriveMessage(/* verbose = */ true)).empty()) {
    state_description = FailStateDescription(fail_state);
  }

  const auto web_drive = GetWebDriveName();
  if (web_drive.empty()) {
    // "Failed - <STATE_DESCRIPTION>"
    return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_INTERRUPTED,
                                      state_description);
  }
  // else: file was rerouted. Formulate the message string accordingly.
  // "Fail to save to <WEB_DRIVE> - <STATE_DESCRIPTION>"
  return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_UPLOAD_INTERRUPTED,
                                    web_drive, state_description);
}

std::u16string DownloadUIModel::GetHistoryPageStatusText() const {
  if (GetLastFailState() == FailState::SERVER_FAILED) {
    // Display the full error description in case of server failure.
    return GetInterruptDescription();
  }
  return GetStatusText();
}

void DownloadUIModel::StatusTextBuilderBase::SetModel(DownloadUIModel* model) {
  model_ = model;
}

std::u16string DownloadUIModel::StatusTextBuilderBase::GetInterruptedStatusText(
    FailState fail_state) const {
  auto state_msg = GetFailStateMessage(fail_state);
  const auto web_drive = model_->GetWebDriveName();
  if (web_drive.empty()) {
    // "Failed - <STATE_MESSAGE>"
    return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_INTERRUPTED,
                                      state_msg);
  }
  // "Fail to save to <WEB_DRIVE> - <STATE_MESSAGE>"
  return l10n_util::GetStringFUTF16(IDS_DOWNLOAD_STATUS_UPLOAD_INTERRUPTED,
                                    web_drive, state_msg);
}

std::u16string
DownloadUIModel::BubbleStatusTextBuilder::GetInterruptedStatusText(
    FailState fail_state) const {
  int string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_WRONG;

  switch (fail_state) {
    case FailState::FILE_ACCESS_DENIED:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_NEEDS_PERMISSION;
      break;
    case FailState::FILE_NO_SPACE:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_DISK_FULL;
      break;
    case FailState::FILE_NAME_TOO_LONG:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_PATH_TOO_LONG;
      break;
    case FailState::FILE_TOO_LARGE:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_FILE_TOO_LARGE;
      break;
    case FailState::FILE_VIRUS_INFECTED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_STATUS_VIRUS;
      break;
    case FailState::FILE_BLOCKED:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_BLOCKED_ORGANIZATION;
      break;
    case FailState::FILE_SECURITY_CHECK_FAILED:
      string_id = IDS_DOWNLOAD_INTERRUPTED_STATUS_SECURITY_CHECK_FAILED;
      break;
    case FailState::FILE_TOO_SHORT:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_WRONG;
      break;
    case FailState::FILE_SAME_AS_SOURCE:
      string_id = IDS_DOWNLOAD_INTERRUPTED_STATUS_FILE_SAME_AS_SOURCE;
      break;
    case FailState::NETWORK_INVALID_REQUEST:
    case FailState::NETWORK_FAILED:
    case FailState::NETWORK_INSTABILITY:
    case FailState::NETWORK_TIMEOUT:
    case FailState::NETWORK_DISCONNECTED:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_NETWORK_ERROR;
      break;
    case FailState::NETWORK_SERVER_DOWN:
    case FailState::SERVER_FAILED:
    case FailState::SERVER_CERT_PROBLEM:
    case FailState::SERVER_UNREACHABLE:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_SITE_UNAVAILABLE;
      break;
    case FailState::SERVER_UNAUTHORIZED:
    case FailState::SERVER_FORBIDDEN:
    case FailState::SERVER_BAD_CONTENT:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_FILE_UNAVAILABLE;
      break;
    case FailState::USER_CANCELED:
      string_id = IDS_DOWNLOAD_STATUS_CANCELLED;
      break;

    case FailState::FILE_TRANSIENT_ERROR:
    case FailState::USER_SHUTDOWN:
    case FailState::CRASH:
    case FailState::SERVER_CONTENT_LENGTH_MISMATCH:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_FILE_UNFINISHED;
      break;
    case FailState::CANNOT_DOWNLOAD:
    case FailState::SERVER_NO_RANGE:
    case FailState::SERVER_CROSS_ORIGIN_REDIRECT:
    case FailState::FILE_FAILED:
    case FailState::FILE_HASH_MISMATCH:
      string_id = IDS_DOWNLOAD_BUBBLE_INTERRUPTED_STATUS_WRONG;
      break;
    case FailState::NO_FAILURE:
      NOTREACHED();
  }

  return l10n_util::GetStringUTF16(string_id);
}

#if BUILDFLAG(FULL_SAFE_BROWSING)
void DownloadUIModel::CompleteSafeBrowsingScan() {}
#endif

bool DownloadUIModel::ShouldShowDropdown() const {
  return true;
}
