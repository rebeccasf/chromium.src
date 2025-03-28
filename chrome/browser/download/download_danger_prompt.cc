// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/download_danger_prompt.h"

#include "base/metrics/histogram_functions.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/safe_browsing/chrome_user_population_helper.h"
#include "chrome/browser/safe_browsing/download_protection/download_protection_service.h"
#include "chrome/browser/safe_browsing/download_protection/download_protection_util.h"
#include "chrome/browser/safe_browsing/safe_browsing_service.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/safe_browsing/content/browser/web_ui/safe_browsing_ui.h"
#include "components/safe_browsing/content/common/file_type_policies.h"
#include "components/safe_browsing/core/common/proto/csd.pb.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_item_utils.h"

using safe_browsing::ClientDownloadResponse;
using safe_browsing::ClientSafeBrowsingReportRequest;

namespace {
//const char kDownloadDangerPromptPrefix[] = "Download.DownloadDangerPrompt";

}  // namespace

void DownloadDangerPrompt::SendSafeBrowsingDownloadReport(
    ClientSafeBrowsingReportRequest::ReportType report_type,
    bool did_proceed,
    const download::DownloadItem& download) {
#if 0
  safe_browsing::SafeBrowsingService* sb_service =
      g_browser_process->safe_browsing_service();
  Profile* profile = Profile::FromBrowserContext(
      content::DownloadItemUtils::GetBrowserContext(&download));
  auto report = std::make_unique<ClientSafeBrowsingReportRequest>();
  report->set_type(report_type);
  ClientDownloadResponse::Verdict download_verdict =
      safe_browsing::DownloadDangerTypeToDownloadResponseVerdict(
          download.GetDangerType());
  if (download_verdict == ClientDownloadResponse::SAFE) {
    // Don't send report if the verdict is SAFE.
    return;
  }
  report->set_download_verdict(download_verdict);
  report->set_url(download.GetURL().spec());
  report->set_did_proceed(did_proceed);
  *report->mutable_population() =
      safe_browsing::GetUserPopulationForProfile(profile);
  std::string token =
      safe_browsing::DownloadProtectionService::GetDownloadPingToken(&download);
  if (!token.empty())
    report->set_token(token);
  std::string serialized_report;
  if (report->SerializeToString(&serialized_report)) {
    sb_service->SendSerializedDownloadReport(profile, serialized_report);

    // The following is to log this ClientSafeBrowsingReportRequest on any open
    // chrome://safe-browsing pages.
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(
            &safe_browsing::WebUIInfoSingleton::AddToCSBRRsSent,
            base::Unretained(safe_browsing::WebUIInfoSingleton::GetInstance()),
            std::move(report)));
  } else {
    DLOG(ERROR) << "Unable to serialize the threat report.";
  }
#endif
}

void DownloadDangerPrompt::RecordDownloadDangerPrompt(
    bool did_proceed,
    const download::DownloadItem& download) {
#if 0
  int64_t file_type_uma_value =
      safe_browsing::FileTypePolicies::GetInstance()->UmaValueForFile(
          download.GetTargetFilePath());
  download::DownloadDangerType danger_type = download.GetDangerType();

  base::UmaHistogramSparse(
      base::StringPrintf("%s.%s.Shown", kDownloadDangerPromptPrefix,
                         download::GetDownloadDangerTypeString(danger_type)),
      file_type_uma_value);
  if (did_proceed) {
    base::UmaHistogramSparse(
        base::StringPrintf("%s.%s.Proceed", kDownloadDangerPromptPrefix,
                           download::GetDownloadDangerTypeString(danger_type)),
        file_type_uma_value);
  }
#endif
}
