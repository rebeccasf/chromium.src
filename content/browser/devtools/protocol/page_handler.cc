// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/devtools/protocol/page_handler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_memory.h"
#include "base/numerics/safe_conversions.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "components/back_forward_cache/disabled_reason_id.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/devtools/devtools_agent_host_impl.h"
#include "content/browser/devtools/protocol/browser_handler.h"
#include "content/browser/devtools/protocol/devtools_mhtml_helper.h"
#include "content/browser/devtools/protocol/emulation_handler.h"
#include "content/browser/devtools/protocol/handler_helpers.h"
#include "content/browser/manifest/manifest_manager_host.h"
#include "content/browser/renderer_host/back_forward_cache_can_store_document_result.h"
#include "content/browser/renderer_host/back_forward_cache_disable.h"
#include "content/browser/renderer_host/back_forward_cache_metrics.h"
#include "content/browser/renderer_host/navigation_entry_impl.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/navigator.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/browser/web_contents/web_contents_view.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/common/referrer.h"
#include "content/public/common/result_codes.h"
#include "net/base/filename_util.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/common/manifest/manifest_util.h"
#include "third_party/blink/public/mojom/manifest/manifest.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_util.h"
#include "ui/gfx/skbitmap_operations.h"
#include "ui/snapshot/snapshot.h"

#if BUILDFLAG(IS_ANDROID)
#include "content/browser/renderer_host/compositor_impl_android.h"
#endif

namespace content {
namespace protocol {

namespace {

constexpr const char* kMhtml = "mhtml";
constexpr int kDefaultScreenshotQuality = 80;
constexpr int kFrameRetryDelayMs = 100;
constexpr int kCaptureRetryLimit = 2;
constexpr int kMaxScreencastFramesInFlight = 2;
constexpr char kCommandIsOnlyAvailableAtTopTarget[] =
    "Command can only be executed on top-level targets";

Binary EncodeImage(const gfx::Image& image,
                   const std::string& format,
                   int quality) {
  DCHECK(!image.IsEmpty());

  scoped_refptr<base::RefCountedMemory> data;
  if (format == protocol::Page::CaptureScreenshot::FormatEnum::Png) {
    data = image.As1xPNGBytes();
  } else if (format == Page::CaptureScreenshot::FormatEnum::Jpeg) {
    auto bytes = base::MakeRefCounted<base::RefCountedBytes>();
    if (gfx::JPEG1xEncodedDataFromImage(image, quality, &bytes->data()))
      data = bytes;
  } else if (format == Page::CaptureScreenshot::FormatEnum::Webp) {
    auto bytes = base::MakeRefCounted<base::RefCountedBytes>();
    if (gfx::WebpEncodedDataFromImage(image, quality, &bytes->data()))
      data = bytes;
  } else {
    NOTREACHED();
  }

  if (!data || !data->front())
    return protocol::Binary();

  return Binary::fromRefCounted(data);
}

Binary EncodeSkBitmap(const SkBitmap& image,
                      const std::string& format,
                      int quality) {
  return EncodeImage(gfx::Image::CreateFrom1xBitmap(image), format, quality);
}

std::unique_ptr<Page::ScreencastFrameMetadata> BuildScreencastFrameMetadata(
    const gfx::Size& surface_size,
    float device_scale_factor,
    float page_scale_factor,
    const gfx::PointF& root_scroll_offset,
    float top_controls_visible_height) {
  if (surface_size.IsEmpty() || device_scale_factor == 0)
    return nullptr;

  const gfx::SizeF content_size_dip =
      gfx::ScaleSize(gfx::SizeF(surface_size), 1 / device_scale_factor);
  float top_offset_dip = top_controls_visible_height;
  gfx::PointF root_scroll_offset_dip = root_scroll_offset;
  top_offset_dip /= device_scale_factor;
  root_scroll_offset_dip.Scale(1 / device_scale_factor);
  std::unique_ptr<Page::ScreencastFrameMetadata> page_metadata =
      Page::ScreencastFrameMetadata::Create()
          .SetPageScaleFactor(page_scale_factor)
          .SetOffsetTop(top_offset_dip)
          .SetDeviceWidth(content_size_dip.width())
          .SetDeviceHeight(content_size_dip.height())
          .SetScrollOffsetX(root_scroll_offset_dip.x())
          .SetScrollOffsetY(root_scroll_offset_dip.y())
          .SetTimestamp(base::Time::Now().ToDoubleT())
          .Build();
  return page_metadata;
}

// Determines the snapshot size that best-fits the Surface's content to the
// remote's requested image size.
gfx::Size DetermineSnapshotSize(const gfx::Size& surface_size,
                                int screencast_max_width,
                                int screencast_max_height) {
  if (surface_size.IsEmpty())
    return gfx::Size();  // Nothing to copy (and avoid divide-by-zero below).

  double scale = 1;
  if (screencast_max_width > 0) {
    scale = std::min(scale, static_cast<double>(screencast_max_width) /
                                surface_size.width());
  }
  if (screencast_max_height > 0) {
    scale = std::min(scale, static_cast<double>(screencast_max_height) /
                                surface_size.height());
  }
  return gfx::ToRoundedSize(gfx::ScaleSize(gfx::SizeF(surface_size), scale));
}

void GetMetadataFromFrame(const media::VideoFrame& frame,
                          double* device_scale_factor,
                          double* page_scale_factor,
                          gfx::PointF* root_scroll_offset,
                          double* top_controls_visible_height) {
  // Get metadata from |frame|. This will CHECK if metadata is missing.
  *device_scale_factor = *frame.metadata().device_scale_factor;
  *page_scale_factor = *frame.metadata().page_scale_factor;
  root_scroll_offset->set_x(*frame.metadata().root_scroll_offset_x);
  root_scroll_offset->set_y(*frame.metadata().root_scroll_offset_y);
  *top_controls_visible_height = *frame.metadata().top_controls_visible_height;
}

template <typename ProtocolCallback>
bool CanExecuteGlobalCommands(
    RenderFrameHost* host,
    const std::unique_ptr<ProtocolCallback>& callback) {
  if (!host || !host->GetParent())
    return true;
  callback->sendFailure(
      Response::ServerError(kCommandIsOnlyAvailableAtTopTarget));
  return false;
}

}  // namespace

PageHandler::PageHandler(
    EmulationHandler* emulation_handler,
    BrowserHandler* browser_handler,
    bool allow_unsafe_operations,
    absl::optional<url::Origin> navigation_initiator_origin)
    : DevToolsDomainHandler(Page::Metainfo::domainName),
      allow_unsafe_operations_(allow_unsafe_operations),
      navigation_initiator_origin_(navigation_initiator_origin),
      enabled_(false),
      screencast_enabled_(false),
      screencast_quality_(kDefaultScreenshotQuality),
      screencast_max_width_(-1),
      screencast_max_height_(-1),
      capture_every_nth_frame_(1),
      capture_retry_count_(0),
      session_id_(0),
      frame_counter_(0),
      frames_in_flight_(0),
      video_consumer_(nullptr),
      last_surface_size_(gfx::Size()),
      host_(nullptr),
      emulation_handler_(emulation_handler),
      browser_handler_(browser_handler) {
  bool create_video_consumer = true;
#if BUILDFLAG(IS_ANDROID)
  constexpr auto kScreencastPixelFormat = media::PIXEL_FORMAT_I420;
  // Video capture doesn't work on Android WebView. Use CopyFromSurface instead.
  if (!CompositorImpl::IsInitialized())
    create_video_consumer = false;
#else
  constexpr auto kScreencastPixelFormat = media::PIXEL_FORMAT_ARGB;
#endif
  if (create_video_consumer) {
    video_consumer_ = std::make_unique<DevToolsVideoConsumer>(
        base::BindRepeating(&PageHandler::OnFrameFromVideoConsumer,
                            weak_factory_.GetWeakPtr()));
    video_consumer_->SetFormat(kScreencastPixelFormat);
  }
  DCHECK(emulation_handler_);
}

PageHandler::~PageHandler() = default;

// static
std::vector<PageHandler*> PageHandler::EnabledForWebContents(
    WebContentsImpl* contents) {
  if (!DevToolsAgentHost::HasFor(contents))
    return std::vector<PageHandler*>();
  std::vector<PageHandler*> result;
  for (auto* handler :
       PageHandler::ForAgentHost(static_cast<DevToolsAgentHostImpl*>(
           DevToolsAgentHost::GetOrCreateFor(contents).get()))) {
    if (handler->enabled_)
      result.push_back(handler);
  }
  return result;
}

// static
std::vector<PageHandler*> PageHandler::ForAgentHost(
    DevToolsAgentHostImpl* host) {
  return host->HandlersByName<PageHandler>(Page::Metainfo::domainName);
}

void PageHandler::SetRenderer(int process_host_id,
                              RenderFrameHostImpl* frame_host) {
  if (host_ == frame_host)
    return;

  RenderWidgetHostImpl* widget_host =
      host_ ? host_->GetRenderWidgetHost() : nullptr;
  if (widget_host && observation_.IsObservingSource(widget_host))
    observation_.Reset();

  host_ = frame_host;
  widget_host = host_ ? host_->GetRenderWidgetHost() : nullptr;

  if (widget_host)
    observation_.Observe(widget_host);

  if (video_consumer_ && frame_host) {
    video_consumer_->SetFrameSinkId(
        frame_host->GetRenderWidgetHost()->GetFrameSinkId());
  }
}

void PageHandler::Wire(UberDispatcher* dispatcher) {
  frontend_ = std::make_unique<Page::Frontend>(dispatcher->channel());
  Page::Dispatcher::wire(dispatcher, this);
}

void PageHandler::OnSynchronousSwapCompositorFrame(
    const cc::RenderFrameMetadata& frame_metadata) {
  // Cache |frame_metadata_| as InnerSwapCompositorFrame may also be called on
  // screencast start.
  frame_metadata_ = frame_metadata;
  if (screencast_enabled_)
    InnerSwapCompositorFrame();
}

void PageHandler::RenderWidgetHostVisibilityChanged(
    RenderWidgetHost* widget_host,
    bool became_visible) {
  if (!screencast_enabled_)
    return;
  NotifyScreencastVisibility(became_visible);
}

void PageHandler::RenderWidgetHostDestroyed(RenderWidgetHost* widget_host) {
  DCHECK(observation_.IsObservingSource(widget_host));
  observation_.Reset();
}

void PageHandler::DidAttachInterstitialPage() {
  if (!enabled_)
    return;
  frontend_->InterstitialShown();
}

void PageHandler::DidDetachInterstitialPage() {
  if (!enabled_)
    return;
  frontend_->InterstitialHidden();
}

void PageHandler::DidRunJavaScriptDialog(const GURL& url,
                                         const std::u16string& message,
                                         const std::u16string& default_prompt,
                                         JavaScriptDialogType dialog_type,
                                         bool has_non_devtools_handlers,
                                         JavaScriptDialogCallback callback) {
  if (!enabled_)
    return;
  DCHECK(pending_dialog_.is_null());
  pending_dialog_ = std::move(callback);
  std::string type = Page::DialogTypeEnum::Alert;
  if (dialog_type == JAVASCRIPT_DIALOG_TYPE_CONFIRM)
    type = Page::DialogTypeEnum::Confirm;
  if (dialog_type == JAVASCRIPT_DIALOG_TYPE_PROMPT)
    type = Page::DialogTypeEnum::Prompt;
  frontend_->JavascriptDialogOpening(url.spec(), base::UTF16ToUTF8(message),
                                     type, has_non_devtools_handlers,
                                     base::UTF16ToUTF8(default_prompt));
}

void PageHandler::DidRunBeforeUnloadConfirm(const GURL& url,
                                            bool has_non_devtools_handlers,
                                            JavaScriptDialogCallback callback) {
  if (!enabled_)
    return;
  DCHECK(pending_dialog_.is_null());
  pending_dialog_ = std::move(callback);
  frontend_->JavascriptDialogOpening(url.spec(), std::string(),
                                     Page::DialogTypeEnum::Beforeunload,
                                     has_non_devtools_handlers, std::string());
}

void PageHandler::DidCloseJavaScriptDialog(bool success,
                                           const std::u16string& user_input) {
  if (!enabled_)
    return;
  pending_dialog_.Reset();
  frontend_->JavascriptDialogClosed(success, base::UTF16ToUTF8(user_input));
}

Response PageHandler::Enable() {
  enabled_ = true;
  return Response::FallThrough();
}

Response PageHandler::Disable() {
  enabled_ = false;
  screencast_enabled_ = false;
  bypass_csp_ = false;

  if (video_consumer_)
    video_consumer_->StopCapture();

  if (!pending_dialog_.is_null()) {
    WebContentsImpl* web_contents = GetWebContents();
    // Leave dialog hanging if there is a manager that can take care of it,
    // cancel and send ack otherwise.
    bool has_dialog_manager =
        web_contents && web_contents->GetDelegate() &&
        web_contents->GetDelegate()->GetJavaScriptDialogManager(web_contents);
    if (!has_dialog_manager)
      std::move(pending_dialog_).Run(false, std::u16string());
    pending_dialog_.Reset();
  }

  for (auto* item : pending_downloads_)
    item->RemoveObserver(this);
  pending_downloads_.clear();
  navigate_callbacks_.clear();
  return Response::FallThrough();
}

Response PageHandler::Crash() {
  WebContents* web_contents = WebContents::FromRenderFrameHost(host_);
  if (!web_contents)
    return Response::ServerError("Not attached to a page");
  if (web_contents->IsCrashed())
    return Response::ServerError("The target has already crashed");
  if (host_->frame_tree_node()->navigation_request())
    return Response::ServerError("Page has pending navigations, not killing");
  return Response::FallThrough();
}

Response PageHandler::Close() {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::ServerError("Not attached to a page");
  web_contents->DispatchBeforeUnload(false /* auto_cancel */);
  return Response::Success();
}

void PageHandler::Reload(Maybe<bool> bypassCache,
                         Maybe<std::string> script_to_evaluate_on_load,
                         std::unique_ptr<ReloadCallback> callback) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents) {
    callback->sendFailure(Response::InternalError());
    return;
  }

  // In the case of inspecting a GuestView (e.g. a PDF), we should reload
  // the outer web contents (embedder), since otherwise reloading the guest by
  // itself will fail.
  if (web_contents->GetOuterWebContents())
    web_contents = web_contents->GetOuterWebContents();

  // It is important to fallback before triggering reload, so that
  // renderer could prepare beforehand.
  callback->fallThrough();
  web_contents->GetController().Reload(bypassCache.fromMaybe(false)
                                           ? ReloadType::BYPASSING_CACHE
                                           : ReloadType::NORMAL,
                                       false);
}

static network::mojom::ReferrerPolicy ParsePolicyFromString(
    const std::string& policy) {
  if (policy == Page::ReferrerPolicyEnum::NoReferrer)
    return network::mojom::ReferrerPolicy::kNever;
  if (policy == Page::ReferrerPolicyEnum::NoReferrerWhenDowngrade)
    return network::mojom::ReferrerPolicy::kNoReferrerWhenDowngrade;
  if (policy == Page::ReferrerPolicyEnum::Origin)
    return network::mojom::ReferrerPolicy::kOrigin;
  if (policy == Page::ReferrerPolicyEnum::OriginWhenCrossOrigin)
    return network::mojom::ReferrerPolicy::kOriginWhenCrossOrigin;
  if (policy == Page::ReferrerPolicyEnum::SameOrigin)
    return network::mojom::ReferrerPolicy::kSameOrigin;
  if (policy == Page::ReferrerPolicyEnum::StrictOrigin)
    return network::mojom::ReferrerPolicy::kStrictOrigin;
  if (policy == Page::ReferrerPolicyEnum::StrictOriginWhenCrossOrigin) {
    return network::mojom::ReferrerPolicy::kStrictOriginWhenCrossOrigin;
  }
  if (policy == Page::ReferrerPolicyEnum::UnsafeUrl)
    return network::mojom::ReferrerPolicy::kAlways;

  DCHECK(policy.empty());
  return network::mojom::ReferrerPolicy::kDefault;
}

void PageHandler::Navigate(const std::string& url,
                           Maybe<std::string> referrer,
                           Maybe<std::string> maybe_transition_type,
                           Maybe<std::string> frame_id,
                           Maybe<std::string> referrer_policy,
                           std::unique_ptr<NavigateCallback> callback) {
  GURL gurl(url);
  if (!gurl.is_valid()) {
    callback->sendFailure(
        Response::ServerError("Cannot navigate to invalid URL"));
    return;
  }

  if (!host_) {
    callback->sendFailure(Response::InternalError());
    return;
  }

  ui::PageTransition type;
  std::string transition_type =
      maybe_transition_type.fromMaybe(Page::TransitionTypeEnum::Typed);
  if (transition_type == Page::TransitionTypeEnum::Link)
    type = ui::PAGE_TRANSITION_LINK;
  else if (transition_type == Page::TransitionTypeEnum::Typed)
    type = ui::PAGE_TRANSITION_TYPED;
  else if (transition_type == Page::TransitionTypeEnum::Address_bar)
    type = ui::PAGE_TRANSITION_FROM_ADDRESS_BAR;
  else if (transition_type == Page::TransitionTypeEnum::Auto_bookmark)
    type = ui::PAGE_TRANSITION_AUTO_BOOKMARK;
  else if (transition_type == Page::TransitionTypeEnum::Auto_subframe)
    type = ui::PAGE_TRANSITION_AUTO_SUBFRAME;
  else if (transition_type == Page::TransitionTypeEnum::Manual_subframe)
    type = ui::PAGE_TRANSITION_MANUAL_SUBFRAME;
  else if (transition_type == Page::TransitionTypeEnum::Generated)
    type = ui::PAGE_TRANSITION_GENERATED;
  else if (transition_type == Page::TransitionTypeEnum::Auto_toplevel)
    type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  else if (transition_type == Page::TransitionTypeEnum::Form_submit)
    type = ui::PAGE_TRANSITION_FORM_SUBMIT;
  else if (transition_type == Page::TransitionTypeEnum::Reload)
    type = ui::PAGE_TRANSITION_RELOAD;
  else if (transition_type == Page::TransitionTypeEnum::Keyword)
    type = ui::PAGE_TRANSITION_KEYWORD;
  else if (transition_type == Page::TransitionTypeEnum::Keyword_generated)
    type = ui::PAGE_TRANSITION_KEYWORD_GENERATED;
  else
    type = ui::PAGE_TRANSITION_TYPED;

  std::string out_frame_id = frame_id.fromMaybe(
      host_->frame_tree_node()->devtools_frame_token().ToString());
  FrameTreeNode* frame_tree_node = FrameTreeNodeFromDevToolsFrameToken(
      host_->frame_tree_node(), out_frame_id);

  if (!frame_tree_node) {
    callback->sendFailure(
        Response::ServerError("No frame with given id found"));
    return;
  }

  NavigationController::LoadURLParams params(gurl);
  network::mojom::ReferrerPolicy policy =
      ParsePolicyFromString(referrer_policy.fromMaybe(""));
  params.referrer = Referrer(GURL(referrer.fromMaybe("")), policy);
  params.transition_type = type;
  params.frame_tree_node_id = frame_tree_node->frame_tree_node_id();
  if (navigation_initiator_origin_.has_value()) {
    // When this agent has an initiator origin defined, ensure that its
    // navigations are considered renderer-initiated by that origin, such that
    // URL spoof defenses are in effect. (crbug.com/1192417)
    params.is_renderer_initiated = true;
    params.initiator_origin = *navigation_initiator_origin_;
    params.source_site_instance = SiteInstance::CreateForURL(
        host_->GetBrowserContext(), navigation_initiator_origin_->GetURL());
  }
  // Handler may be destroyed while navigating if the session
  // gets disconnected as a result of access checks.
  base::WeakPtr<PageHandler> weak_self = weak_factory_.GetWeakPtr();
  frame_tree_node->navigator().controller().LoadURLWithParams(params);
  if (!weak_self)
    return;
  if (frame_tree_node->navigation_request()) {
    navigate_callbacks_[frame_tree_node->navigation_request()
                            ->devtools_navigation_token()] =
        std::move(callback);
  } else {
    callback->sendSuccess(out_frame_id, Maybe<std::string>(),
                          Maybe<std::string>());
  }
}

void PageHandler::NavigationReset(NavigationRequest* navigation_request) {
  auto navigate_callback =
      navigate_callbacks_.find(navigation_request->devtools_navigation_token());
  if (navigate_callback == navigate_callbacks_.end())
    return;
  std::string frame_id =
      navigation_request->frame_tree_node()->devtools_frame_token().ToString();
  // A new NavigationRequest may have been created before |navigation_request|
  // started, in which case it is not marked as aborted. We report this as an
  // abort to DevTools anyway.
  if (!navigation_request->IsNavigationStarted()) {
    navigate_callback->second->sendSuccess(
        frame_id, Maybe<std::string>(),
        Maybe<std::string>(net::ErrorToString(net::ERR_ABORTED)));
  } else {
    bool success = navigation_request->GetNetErrorCode() == net::OK;
    std::string error_string =
        net::ErrorToString(navigation_request->GetNetErrorCode());
    navigate_callback->second->sendSuccess(
        frame_id,
        Maybe<std::string>(
            navigation_request->devtools_navigation_token().ToString()),
        success ? Maybe<std::string>() : Maybe<std::string>(error_string));
  }
  navigate_callbacks_.erase(navigate_callback);
}

void PageHandler::DownloadWillBegin(FrameTreeNode* ftn,
                                    download::DownloadItem* item) {
  if (!enabled_)
    return;

  // The filename the end user sees may differ. This is an attempt to eagerly
  // determine the filename at the beginning of the download; see
  // DownloadTargetDeterminer:DownloadTargetDeterminer::Result
  // and DownloadTargetDeterminer::GenerateFileName in
  // chrome/browser/download/download_target_determiner.cc
  // for the more comprehensive logic.
  const std::u16string likely_filename = net::GetSuggestedFilename(
      item->GetURL(), item->GetContentDisposition(), std::string(),
      item->GetSuggestedFilename(), item->GetMimeType(), "download");

  frontend_->DownloadWillBegin(ftn->devtools_frame_token().ToString(),
                               item->GetGuid(), item->GetURL().spec(),
                               base::UTF16ToUTF8(likely_filename));

  item->AddObserver(this);
  pending_downloads_.insert(item);
}

void PageHandler::OnDownloadDestroyed(download::DownloadItem* item) {
  pending_downloads_.erase(item);
}

void PageHandler::OnDownloadUpdated(download::DownloadItem* item) {
  if (!enabled_)
    return;
  std::string state;
  switch (item->GetState()) {
    case download::DownloadItem::IN_PROGRESS:
      state = Page::DownloadProgress::StateEnum::InProgress;
      break;
    case download::DownloadItem::COMPLETE:
      state = Page::DownloadProgress::StateEnum::Completed;
      break;
    case download::DownloadItem::CANCELLED:
    case download::DownloadItem::INTERRUPTED:
      state = Page::DownloadProgress::StateEnum::Canceled;
      break;
    case download::DownloadItem::MAX_DOWNLOAD_STATE:
      NOTREACHED();
  }
  frontend_->DownloadProgress(item->GetGuid(), item->GetTotalBytes(),
                              item->GetReceivedBytes(), state);
  if (state != Page::DownloadProgress::StateEnum::InProgress) {
    item->RemoveObserver(this);
    pending_downloads_.erase(item);
  }
}

static const char* TransitionTypeName(ui::PageTransition type) {
  int32_t t = type & ~ui::PAGE_TRANSITION_QUALIFIER_MASK;
  switch (t) {
    case ui::PAGE_TRANSITION_LINK:
      return Page::TransitionTypeEnum::Link;
    case ui::PAGE_TRANSITION_TYPED:
      return Page::TransitionTypeEnum::Typed;
    case ui::PAGE_TRANSITION_AUTO_BOOKMARK:
      return Page::TransitionTypeEnum::Auto_bookmark;
    case ui::PAGE_TRANSITION_AUTO_SUBFRAME:
      return Page::TransitionTypeEnum::Auto_subframe;
    case ui::PAGE_TRANSITION_MANUAL_SUBFRAME:
      return Page::TransitionTypeEnum::Manual_subframe;
    case ui::PAGE_TRANSITION_GENERATED:
      return Page::TransitionTypeEnum::Generated;
    case ui::PAGE_TRANSITION_AUTO_TOPLEVEL:
      return Page::TransitionTypeEnum::Auto_toplevel;
    case ui::PAGE_TRANSITION_FORM_SUBMIT:
      return Page::TransitionTypeEnum::Form_submit;
    case ui::PAGE_TRANSITION_RELOAD:
      return Page::TransitionTypeEnum::Reload;
    case ui::PAGE_TRANSITION_KEYWORD:
      return Page::TransitionTypeEnum::Keyword;
    case ui::PAGE_TRANSITION_KEYWORD_GENERATED:
      return Page::TransitionTypeEnum::Keyword_generated;
    default:
      return Page::TransitionTypeEnum::Other;
  }
}

Response PageHandler::GetNavigationHistory(
    int* current_index,
    std::unique_ptr<NavigationEntries>* entries) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();

  NavigationController& controller = web_contents->GetController();
  *current_index = controller.GetCurrentEntryIndex();
  *entries = std::make_unique<NavigationEntries>();
  for (int i = 0; i != controller.GetEntryCount(); ++i) {
    auto* entry = controller.GetEntryAtIndex(i);
    (*entries)->emplace_back(
        Page::NavigationEntry::Create()
            .SetId(entry->GetUniqueID())
            .SetUrl(entry->GetURL().spec())
            .SetUserTypedURL(entry->GetUserTypedURL().spec())
            .SetTitle(base::UTF16ToUTF8(entry->GetTitle()))
            .SetTransitionType(TransitionTypeName(entry->GetTransitionType()))
            .Build());
  }
  return Response::Success();
}

Response PageHandler::NavigateToHistoryEntry(int entry_id) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();

  NavigationController& controller = web_contents->GetController();
  for (int i = 0; i != controller.GetEntryCount(); ++i) {
    if (controller.GetEntryAtIndex(i)->GetUniqueID() == entry_id) {
      controller.GoToIndex(i);
      return Response::Success();
    }
  }

  return Response::InvalidParams("No entry with passed id");
}

static bool ReturnTrue(NavigationEntry* entry) {
  return true;
}

Response PageHandler::ResetNavigationHistory() {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();

  NavigationController& controller = web_contents->GetController();
  controller.DeleteNavigationEntries(base::BindRepeating(&ReturnTrue));
  return Response::Success();
}

void PageHandler::CaptureSnapshot(
    Maybe<std::string> format,
    std::unique_ptr<CaptureSnapshotCallback> callback) {
  if (!CanExecuteGlobalCommands(host_, callback))
    return;
  std::string snapshot_format = format.fromMaybe(kMhtml);
  if (snapshot_format != kMhtml) {
    callback->sendFailure(Response::ServerError("Unsupported snapshot format"));
    return;
  }
  DevToolsMHTMLHelper::Capture(weak_factory_.GetWeakPtr(), std::move(callback));
}

void PageHandler::CaptureScreenshot(
    Maybe<std::string> format,
    Maybe<int> quality,
    Maybe<Page::Viewport> clip,
    Maybe<bool> from_surface,
    Maybe<bool> capture_beyond_viewport,
    std::unique_ptr<CaptureScreenshotCallback> callback) {
  if (!host_ || !host_->GetRenderWidgetHost() ||
      !host_->GetRenderWidgetHost()->GetView()) {
    callback->sendFailure(Response::InternalError());
    return;
  }
  if (!CanExecuteGlobalCommands(host_, callback))
    return;
  if (clip.isJust()) {
    if (clip.fromJust()->GetWidth() == 0) {
      callback->sendFailure(
          Response::ServerError("Cannot take screenshot with 0 width."));
      return;
    }
    if (clip.fromJust()->GetHeight() == 0) {
      callback->sendFailure(
          Response::ServerError("Cannot take screenshot with 0 height."));
      return;
    }
  }

  RenderWidgetHostImpl* widget_host = host_->GetRenderWidgetHost();
  std::string screenshot_format =
      format.fromMaybe(Page::CaptureScreenshot::FormatEnum::Png);
  int screenshot_quality = quality.fromMaybe(kDefaultScreenshotQuality);

  // We don't support clip/emulation when capturing from window, bail out.
  if (!from_surface.fromMaybe(true)) {
    widget_host->GetSnapshotFromBrowser(
        base::BindOnce(&PageHandler::ScreenshotCaptured,
                       weak_factory_.GetWeakPtr(), std::move(callback),
                       screenshot_format, screenshot_quality, gfx::Size(),
                       gfx::Size(), blink::DeviceEmulationParams(),
                       absl::nullopt),
        false);
    return;
  }

  // Welcome to the neural net of capturing screenshot while emulating device
  // metrics!
  bool emulation_enabled = emulation_handler_->device_emulation_enabled();
  blink::DeviceEmulationParams original_params =
      emulation_handler_->GetDeviceEmulationParams();
  blink::DeviceEmulationParams modified_params = original_params;

  // Capture original view size if we know we are going to destroy it. We use
  // it in ScreenshotCaptured to restore.
  gfx::Size original_view_size =
      emulation_enabled || clip.isJust()
          ? widget_host->GetView()->GetViewBounds().size()
          : gfx::Size();
  gfx::Size emulated_view_size = modified_params.view_size;

  double dpfactor = 1;
  float widget_host_device_scale_factor = widget_host->GetDeviceScaleFactor();
  if (emulation_enabled) {
    // When emulating, emulate again and scale to make resulting image match
    // physical DP resolution. If view_size is not overriden, use actual view
    // size.
    float original_scale =
        original_params.scale > 0 ? original_params.scale : 1;
    if (!modified_params.view_size.width()) {
      emulated_view_size.set_width(
          ceil(original_view_size.width() / original_scale));
    }
    if (!modified_params.view_size.height()) {
      emulated_view_size.set_height(
          ceil(original_view_size.height() / original_scale));
    }

    dpfactor = modified_params.device_scale_factor
                   ? modified_params.device_scale_factor /
                         widget_host_device_scale_factor
                   : 1;
    // When clip is specified, we scale viewport via clip, otherwise we use
    // scale.
    modified_params.scale = clip.isJust() ? 1 : dpfactor;
    modified_params.view_size = emulated_view_size;
  } else if (clip.isJust()) {
    // When not emulating, still need to emulate the page size.
    modified_params.view_size = original_view_size;
    modified_params.screen_size = gfx::Size();
    modified_params.device_scale_factor = 0;
    modified_params.scale = 1;
  }

  // Set up viewport in renderer.
  if (clip.isJust()) {
    modified_params.viewport_offset.SetPoint(clip.fromJust()->GetX(),
                                             clip.fromJust()->GetY());
    modified_params.viewport_scale = clip.fromJust()->GetScale() * dpfactor;
    modified_params.viewport_offset.Scale(widget_host_device_scale_factor);
  }

  absl::optional<blink::web_pref::WebPreferences> maybe_original_web_prefs;
  if (capture_beyond_viewport.fromMaybe(false)) {
    blink::web_pref::WebPreferences original_web_prefs =
        host_->render_view_host()->GetDelegate()->GetOrCreateWebPreferences();
    maybe_original_web_prefs = original_web_prefs;

    blink::web_pref::WebPreferences modified_web_prefs = original_web_prefs;
    // Hiding scrollbar is needed to avoid scrollbar artefacts on the
    // screenshot. Details: https://crbug.com/1003629.
    modified_web_prefs.hide_scrollbars = true;
    modified_web_prefs.record_whole_document = true;
    host_->render_view_host()->GetDelegate()->SetWebPreferences(
        modified_web_prefs);

    {
      // TODO(crbug.com/1141835): Remove the bug is fixed.
      // Walkaround for the bug. Emulated `view_size` has to be set twice,
      // otherwise the scrollbar will be on the screenshot present.
      blink::DeviceEmulationParams tmp_params = modified_params;
      tmp_params.view_size = gfx::Size(1, 1);
      emulation_handler_->SetDeviceEmulationParams(tmp_params);
    }
  }

  // We use DeviceEmulationParams to either emulate, set viewport or both.
  emulation_handler_->SetDeviceEmulationParams(modified_params);

  // Set view size for the screenshot right after emulating.
  if (clip.isJust()) {
    double scale = dpfactor * clip.fromJust()->GetScale();
    widget_host->GetView()->SetSize(
        gfx::Size(base::ClampRound(clip.fromJust()->GetWidth() * scale),
                  base::ClampRound(clip.fromJust()->GetHeight() * scale)));
  } else if (emulation_enabled) {
    widget_host->GetView()->SetSize(
        gfx::ScaleToFlooredSize(emulated_view_size, dpfactor));
  }
  gfx::Size requested_image_size = gfx::Size();
  if (emulation_enabled || clip.isJust()) {
    if (clip.isJust()) {
      requested_image_size =
          gfx::Size(clip.fromJust()->GetWidth(), clip.fromJust()->GetHeight());
    } else {
      requested_image_size = emulated_view_size;
    }
    double scale = widget_host_device_scale_factor * dpfactor;
    if (clip.isJust())
      scale *= clip.fromJust()->GetScale();
    requested_image_size = gfx::ScaleToRoundedSize(requested_image_size, scale);
  }

  widget_host->GetSnapshotFromBrowser(
      base::BindOnce(&PageHandler::ScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     screenshot_format, screenshot_quality, original_view_size,
                     requested_image_size, original_params,
                     maybe_original_web_prefs),
      true);
}

Response PageHandler::StartScreencast(Maybe<std::string> format,
                                      Maybe<int> quality,
                                      Maybe<int> max_width,
                                      Maybe<int> max_height,
                                      Maybe<int> every_nth_frame) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();
  RenderWidgetHostImpl* widget_host =
      host_ ? host_->GetRenderWidgetHost() : nullptr;
  if (!widget_host)
    return Response::InternalError();

  screencast_enabled_ = true;
  screencast_format_ =
      format.fromMaybe(Page::CaptureScreenshot::FormatEnum::Png);
  screencast_quality_ = quality.fromMaybe(kDefaultScreenshotQuality);
  if (screencast_quality_ < 0 || screencast_quality_ > 100)
    screencast_quality_ = kDefaultScreenshotQuality;
  screencast_max_width_ = max_width.fromMaybe(-1);
  screencast_max_height_ = max_height.fromMaybe(-1);
  ++session_id_;
  frame_counter_ = 0;
  frames_in_flight_ = 0;
  capture_every_nth_frame_ = every_nth_frame.fromMaybe(1);
  bool visible = !widget_host->is_hidden();
  NotifyScreencastVisibility(visible);

  if (video_consumer_) {
    gfx::Size surface_size = gfx::Size();
    RenderWidgetHostViewBase* const view =
        static_cast<RenderWidgetHostViewBase*>(host_->GetView());
    if (view) {
      surface_size = view->GetCompositorViewportPixelSize();
      last_surface_size_ = surface_size;
    }

    gfx::Size snapshot_size = DetermineSnapshotSize(
        surface_size, screencast_max_width_, screencast_max_height_);
    if (!snapshot_size.IsEmpty())
      video_consumer_->SetMinAndMaxFrameSize(snapshot_size, snapshot_size);

    video_consumer_->StartCapture();
    return Response::FallThrough();
  }

  if (!visible)
    return Response::FallThrough();

  if (frame_metadata_) {
    InnerSwapCompositorFrame();
  } else {
    widget_host->RequestForceRedraw(0);
  }
  return Response::FallThrough();
}

Response PageHandler::StopScreencast() {
  screencast_enabled_ = false;
  if (video_consumer_)
    video_consumer_->StopCapture();
  return Response::FallThrough();
}

Response PageHandler::ScreencastFrameAck(int session_id) {
  if (session_id == session_id_)
    --frames_in_flight_;
  return Response::Success();
}

Response PageHandler::HandleJavaScriptDialog(bool accept,
                                             Maybe<std::string> prompt_text) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();

  if (pending_dialog_.is_null())
    return Response::InvalidParams("No dialog is showing");

  std::u16string prompt_override;
  if (prompt_text.isJust())
    prompt_override = base::UTF8ToUTF16(prompt_text.fromJust());
  std::move(pending_dialog_).Run(accept, prompt_override);

  // Clean up the dialog UI if any.
  if (web_contents->GetDelegate()) {
    JavaScriptDialogManager* manager =
        web_contents->GetDelegate()->GetJavaScriptDialogManager(web_contents);
    if (manager) {
      manager->HandleJavaScriptDialog(
          web_contents, accept,
          prompt_text.isJust() ? &prompt_override : nullptr);
    }
  }

  return Response::Success();
}

Response PageHandler::BringToFront() {
  WebContentsImpl* wc = GetWebContents();
  if (wc) {
    wc->Activate();
    wc->Focus();
    return Response::Success();
  }
  return Response::InternalError();
}

Response PageHandler::SetDownloadBehavior(const std::string& behavior,
                                          Maybe<std::string> download_path) {
  BrowserContext* browser_context =
      host_ ? host_->GetProcess()->GetBrowserContext() : nullptr;
  if (!browser_context)
    return Response::ServerError("Could not fetch browser context");
  if (host_ && host_->GetParent())
    return Response::ServerError(kCommandIsOnlyAvailableAtTopTarget);
  return browser_handler_->DoSetDownloadBehavior(behavior, browser_context,
                                                 std::move(download_path));
}

void PageHandler::GetAppManifest(
    std::unique_ptr<GetAppManifestCallback> callback) {
  if (!host_) {
    callback->sendFailure(Response::ServerError("Cannot retrieve manifest"));
    return;
  }
  if (!CanExecuteGlobalCommands(host_, callback))
    return;
  ManifestManagerHost::GetOrCreateForPage(host_->GetPage())
      ->RequestManifestDebugInfo(base::BindOnce(&PageHandler::GotManifest,
                                                weak_factory_.GetWeakPtr(),
                                                std::move(callback)));
}

WebContentsImpl* PageHandler::GetWebContents() {
  return host_ && !host_->frame_tree_node()->parent()
             ? static_cast<WebContentsImpl*>(
                   WebContents::FromRenderFrameHost(host_))
             : nullptr;
}

void PageHandler::NotifyScreencastVisibility(bool visible) {
  if (visible)
    capture_retry_count_ = kCaptureRetryLimit;
  frontend_->ScreencastVisibilityChanged(visible);
}

bool PageHandler::ShouldCaptureNextScreencastFrame() {
  return frames_in_flight_ <= kMaxScreencastFramesInFlight &&
         !(++frame_counter_ % capture_every_nth_frame_);
}

void PageHandler::InnerSwapCompositorFrame() {
  if (!host_)
    return;

  if (!ShouldCaptureNextScreencastFrame())
    return;

  RenderWidgetHostViewBase* const view =
      static_cast<RenderWidgetHostViewBase*>(host_->GetView());
  if (!view || !view->IsSurfaceAvailableForCopy())
    return;

  const gfx::Size surface_size = view->GetCompositorViewportPixelSize();
  if (surface_size.IsEmpty())
    return;

  const gfx::Size snapshot_size = DetermineSnapshotSize(
      surface_size, screencast_max_width_, screencast_max_height_);
  if (snapshot_size.IsEmpty())
    return;

  double top_controls_visible_height =
      frame_metadata_->top_controls_height *
      frame_metadata_->top_controls_shown_ratio;

  std::unique_ptr<Page::ScreencastFrameMetadata> page_metadata =
      BuildScreencastFrameMetadata(
          surface_size, frame_metadata_->device_scale_factor,
          frame_metadata_->page_scale_factor,
          frame_metadata_->root_scroll_offset.value_or(gfx::PointF()),
          top_controls_visible_height);
  if (!page_metadata)
    return;

  // Request a copy of the surface as a scaled SkBitmap.
  view->CopyFromSurface(
      gfx::Rect(), snapshot_size,
      base::BindOnce(&PageHandler::ScreencastFrameCaptured,
                     weak_factory_.GetWeakPtr(), std::move(page_metadata)));
  frames_in_flight_++;
}

void PageHandler::OnFrameFromVideoConsumer(
    scoped_refptr<media::VideoFrame> frame) {
  if (!host_)
    return;

  if (!ShouldCaptureNextScreencastFrame())
    return;

  RenderWidgetHostViewBase* const view =
      static_cast<RenderWidgetHostViewBase*>(host_->GetView());
  if (!view)
    return;

  const gfx::Size surface_size = view->GetCompositorViewportPixelSize();
  if (surface_size.IsEmpty())
    return;

  // If window has been resized, set the new dimensions.
  if (surface_size != last_surface_size_) {
    last_surface_size_ = surface_size;
    gfx::Size snapshot_size = DetermineSnapshotSize(
        surface_size, screencast_max_width_, screencast_max_height_);
    if (!snapshot_size.IsEmpty())
      video_consumer_->SetMinAndMaxFrameSize(snapshot_size, snapshot_size);
    return;
  }

  double device_scale_factor, page_scale_factor;
  double top_controls_visible_height;
  gfx::PointF root_scroll_offset;
  GetMetadataFromFrame(*frame, &device_scale_factor, &page_scale_factor,
                       &root_scroll_offset, &top_controls_visible_height);
  std::unique_ptr<Page::ScreencastFrameMetadata> page_metadata =
      BuildScreencastFrameMetadata(surface_size, device_scale_factor,
                                   page_scale_factor, root_scroll_offset,
                                   top_controls_visible_height);
  if (!page_metadata)
    return;

  frames_in_flight_++;
  ScreencastFrameCaptured(std::move(page_metadata),
                          DevToolsVideoConsumer::GetSkBitmapFromFrame(frame));
}

void PageHandler::ScreencastFrameCaptured(
    std::unique_ptr<Page::ScreencastFrameMetadata> page_metadata,
    const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    if (capture_retry_count_) {
      --capture_retry_count_;
      base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&PageHandler::InnerSwapCompositorFrame,
                         weak_factory_.GetWeakPtr()),
          base::Milliseconds(kFrameRetryDelayMs));
    }
    --frames_in_flight_;
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&EncodeSkBitmap, bitmap, screencast_format_,
                     screencast_quality_),
      base::BindOnce(&PageHandler::ScreencastFrameEncoded,
                     weak_factory_.GetWeakPtr(), std::move(page_metadata)));
}

void PageHandler::ScreencastFrameEncoded(
    std::unique_ptr<Page::ScreencastFrameMetadata> page_metadata,
    const protocol::Binary& data) {
  if (data.size() == 0) {
    --frames_in_flight_;
    return;  // Encode failed.
  }

  frontend_->ScreencastFrame(data, std::move(page_metadata), session_id_);
}

void PageHandler::ScreenshotCaptured(
    std::unique_ptr<CaptureScreenshotCallback> callback,
    const std::string& format,
    int quality,
    const gfx::Size& original_view_size,
    const gfx::Size& requested_image_size,
    const blink::DeviceEmulationParams& original_emulation_params,
    const absl::optional<blink::web_pref::WebPreferences>&
        maybe_original_web_prefs,
    const gfx::Image& image) {
  if (original_view_size.width()) {
    RenderWidgetHostImpl* widget_host = host_->GetRenderWidgetHost();
    widget_host->GetView()->SetSize(original_view_size);
    emulation_handler_->SetDeviceEmulationParams(original_emulation_params);
  }

  if (maybe_original_web_prefs) {
    host_->render_view_host()->GetDelegate()->SetWebPreferences(
        maybe_original_web_prefs.value());
  }

  if (image.IsEmpty()) {
    callback->sendFailure(
        Response::ServerError("Unable to capture screenshot"));
    return;
  }

  if (!requested_image_size.IsEmpty() &&
      (image.Width() != requested_image_size.width() ||
       image.Height() != requested_image_size.height())) {
    const SkBitmap* bitmap = image.ToSkBitmap();
    SkBitmap cropped = SkBitmapOperations::CreateTiledBitmap(
        *bitmap, 0, 0, requested_image_size.width(),
        requested_image_size.height());
    gfx::Image croppedImage = gfx::Image::CreateFrom1xBitmap(cropped);
    callback->sendSuccess(EncodeImage(croppedImage, format, quality));
  } else {
    callback->sendSuccess(EncodeImage(image, format, quality));
  }
}

void PageHandler::GotManifest(std::unique_ptr<GetAppManifestCallback> callback,
                              const GURL& manifest_url,
                              ::blink::mojom::ManifestPtr parsed_manifest,
                              blink::mojom::ManifestDebugInfoPtr debug_info) {
  auto errors = std::make_unique<protocol::Array<Page::AppManifestError>>();
  bool failed = true;
  if (debug_info) {
    failed = false;
    for (const auto& error : debug_info->errors) {
      errors->emplace_back(Page::AppManifestError::Create()
                               .SetMessage(error->message)
                               .SetCritical(error->critical)
                               .SetLine(error->line)
                               .SetColumn(error->column)
                               .Build());
      if (error->critical)
        failed = true;
    }
  }

  std::unique_ptr<Page::AppManifestParsedProperties> parsed;
  if (!blink::IsEmptyManifest(parsed_manifest)) {
    parsed = Page::AppManifestParsedProperties::Create()
                 .SetScope(parsed_manifest->scope.possibly_invalid_spec())
                 .Build();
  }

  callback->sendSuccess(
      manifest_url.possibly_invalid_spec(), std::move(errors),
      failed ? Maybe<std::string>() : debug_info->raw_manifest,
      std::move(parsed));
}

Response PageHandler::StopLoading() {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::InternalError();
  web_contents->Stop();
  return Response::Success();
}

Response PageHandler::SetWebLifecycleState(const std::string& state) {
  WebContentsImpl* web_contents = GetWebContents();
  if (!web_contents)
    return Response::ServerError("Not attached to a page");
  if (state == Page::SetWebLifecycleState::StateEnum::Frozen) {
    // TODO(fmeawad): Instead of forcing a visibility change, only allow
    // freezing a page if it was already hidden.
    web_contents->WasHidden();
    web_contents->SetPageFrozen(true);
    return Response::Success();
  }
  if (state == Page::SetWebLifecycleState::StateEnum::Active) {
    web_contents->SetPageFrozen(false);
    return Response::Success();
  }
  return Response::ServerError("Unidentified lifecycle state");
}

void PageHandler::GetInstallabilityErrors(
    std::unique_ptr<GetInstallabilityErrorsCallback> callback) {
  auto installability_errors =
      std::make_unique<protocol::Array<Page::InstallabilityError>>();
  // TODO: Use InstallableManager once it moves into content/.
  // Until then, this code is only used to return empty array in the tests.
  callback->sendSuccess(std::move(installability_errors));
}

void PageHandler::GetManifestIcons(
    std::unique_ptr<GetManifestIconsCallback> callback) {
  // TODO: Use InstallableManager once it moves into content/.
  // Until then, this code is only used to return no image data in the tests.
  callback->sendSuccess(Maybe<Binary>());
}

void PageHandler::GetAppId(std::unique_ptr<GetAppIdCallback> callback) {
  // TODO: Use InstallableManager once it moves into content/.
  // Until then, this code is only used to return no image data in the tests.
  callback->sendSuccess(protocol::Maybe<protocol::String>(),
                        protocol::Maybe<protocol::String>());
}

Response PageHandler::SetBypassCSP(bool enabled) {
  bypass_csp_ = enabled;
  return Response::FallThrough();
}

Page::BackForwardCacheNotRestoredReason NotRestoredReasonToProtocol(
    BackForwardCacheMetrics::NotRestoredReason reason) {
  using Reason = BackForwardCacheMetrics::NotRestoredReason;
  switch (reason) {
    case Reason::kNotPrimaryMainFrame:
      return Page::BackForwardCacheNotRestoredReasonEnum::NotPrimaryMainFrame;
    case Reason::kBackForwardCacheDisabled:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          BackForwardCacheDisabled;
    case Reason::kRelatedActiveContentsExist:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RelatedActiveContentsExist;
    case Reason::kHTTPStatusNotOK:
      return Page::BackForwardCacheNotRestoredReasonEnum::HTTPStatusNotOK;
    case Reason::kSchemeNotHTTPOrHTTPS:
      return Page::BackForwardCacheNotRestoredReasonEnum::SchemeNotHTTPOrHTTPS;
    case Reason::kLoading:
      return Page::BackForwardCacheNotRestoredReasonEnum::Loading;
    case Reason::kWasGrantedMediaAccess:
      return Page::BackForwardCacheNotRestoredReasonEnum::WasGrantedMediaAccess;
    case Reason::kDisableForRenderFrameHostCalled:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          DisableForRenderFrameHostCalled;
    case Reason::kDomainNotAllowed:
      return Page::BackForwardCacheNotRestoredReasonEnum::DomainNotAllowed;
    case Reason::kHTTPMethodNotGET:
      return Page::BackForwardCacheNotRestoredReasonEnum::HTTPMethodNotGET;
    case Reason::kSubframeIsNavigating:
      return Page::BackForwardCacheNotRestoredReasonEnum::SubframeIsNavigating;
    case Reason::kTimeout:
      return Page::BackForwardCacheNotRestoredReasonEnum::Timeout;
    case Reason::kCacheLimit:
      return Page::BackForwardCacheNotRestoredReasonEnum::CacheLimit;
    case Reason::kJavaScriptExecution:
      return Page::BackForwardCacheNotRestoredReasonEnum::JavaScriptExecution;
    case Reason::kRendererProcessKilled:
      return Page::BackForwardCacheNotRestoredReasonEnum::RendererProcessKilled;
    case Reason::kRendererProcessCrashed:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RendererProcessCrashed;
    case Reason::kGrantedMediaStreamAccess:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          GrantedMediaStreamAccess;
    case Reason::kSchedulerTrackedFeatureUsed:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          SchedulerTrackedFeatureUsed;
    case Reason::kConflictingBrowsingInstance:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          ConflictingBrowsingInstance;
    case Reason::kCacheFlushed:
      return Page::BackForwardCacheNotRestoredReasonEnum::CacheFlushed;
    case Reason::kServiceWorkerVersionActivation:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          ServiceWorkerVersionActivation;
    case Reason::kSessionRestored:
      return Page::BackForwardCacheNotRestoredReasonEnum::SessionRestored;
    case Reason::kServiceWorkerPostMessage:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          ServiceWorkerPostMessage;
    case Reason::kEnteredBackForwardCacheBeforeServiceWorkerHostAdded:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          EnteredBackForwardCacheBeforeServiceWorkerHostAdded;
    case Reason::kNotMostRecentNavigationEntry:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          NotMostRecentNavigationEntry;
    case Reason::kServiceWorkerClaim:
      return Page::BackForwardCacheNotRestoredReasonEnum::ServiceWorkerClaim;
    case Reason::kIgnoreEventAndEvict:
      return Page::BackForwardCacheNotRestoredReasonEnum::IgnoreEventAndEvict;
    case Reason::kHaveInnerContents:
      return Page::BackForwardCacheNotRestoredReasonEnum::HaveInnerContents;
    case Reason::kTimeoutPuttingInCache:
      return Page::BackForwardCacheNotRestoredReasonEnum::TimeoutPuttingInCache;
    case Reason::kBackForwardCacheDisabledByLowMemory:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          BackForwardCacheDisabledByLowMemory;
    case Reason::kBackForwardCacheDisabledByCommandLine:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          BackForwardCacheDisabledByCommandLine;
    case Reason::kNetworkRequestDatapipeDrainedAsBytesConsumer:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          NetworkRequestDatapipeDrainedAsBytesConsumer;
    case Reason::kNetworkRequestRedirected:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          NetworkRequestRedirected;
    case Reason::kNetworkRequestTimeout:
      return Page::BackForwardCacheNotRestoredReasonEnum::NetworkRequestTimeout;
    case Reason::kNetworkExceedsBufferLimit:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          NetworkExceedsBufferLimit;
    case Reason::kNavigationCancelledWhileRestoring:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          NavigationCancelledWhileRestoring;
    case Reason::kUserAgentOverrideDiffers:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          UserAgentOverrideDiffers;
    case Reason::kForegroundCacheLimit:
      return Page::BackForwardCacheNotRestoredReasonEnum::ForegroundCacheLimit;
    case Reason::kBrowsingInstanceNotSwapped:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          BrowsingInstanceNotSwapped;
    case Reason::kBackForwardCacheDisabledForDelegate:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          BackForwardCacheDisabledForDelegate;
    case Reason::kUnloadHandlerExistsInMainFrame:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          UnloadHandlerExistsInMainFrame;
    case Reason::kUnloadHandlerExistsInSubFrame:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          UnloadHandlerExistsInSubFrame;
    case Reason::kServiceWorkerUnregistration:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          ServiceWorkerUnregistration;
    case Reason::kCacheControlNoStore:
      return Page::BackForwardCacheNotRestoredReasonEnum::CacheControlNoStore;
    case Reason::kCacheControlNoStoreCookieModified:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          CacheControlNoStoreCookieModified;
    case Reason::kCacheControlNoStoreHTTPOnlyCookieModified:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          CacheControlNoStoreHTTPOnlyCookieModified;
    case Reason::kNoResponseHead:
      return Page::BackForwardCacheNotRestoredReasonEnum::NoResponseHead;
    case Reason::kErrorDocument:
      return Page::BackForwardCacheNotRestoredReasonEnum::ErrorDocument;
    case Reason::kFencedFramesEmbedder:
      return Page::BackForwardCacheNotRestoredReasonEnum::FencedFramesEmbedder;
    case Reason::kBlocklistedFeatures:
      // Blocklisted features should be handled separately and be broken down
      // into sub reasons.
      NOTREACHED();
      return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
    case Reason::kUnknown:
      return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
  }
}

using blink::scheduler::WebSchedulerTrackedFeature;
Page::BackForwardCacheNotRestoredReason BlocklistedFeatureToProtocol(
    WebSchedulerTrackedFeature feature) {
  switch (feature) {
    case WebSchedulerTrackedFeature::kWebSocket:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebSocket;
    case WebSchedulerTrackedFeature::kWebTransport:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebTransport;
    case WebSchedulerTrackedFeature::kWebRTC:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebRTC;
    case WebSchedulerTrackedFeature::kMainResourceHasCacheControlNoCache:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          MainResourceHasCacheControlNoCache;
    case WebSchedulerTrackedFeature::kMainResourceHasCacheControlNoStore:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          MainResourceHasCacheControlNoStore;
    case WebSchedulerTrackedFeature::kSubresourceHasCacheControlNoCache:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          SubresourceHasCacheControlNoCache;
    case WebSchedulerTrackedFeature::kSubresourceHasCacheControlNoStore:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          SubresourceHasCacheControlNoStore;
    case WebSchedulerTrackedFeature::kContainsPlugins:
      return Page::BackForwardCacheNotRestoredReasonEnum::ContainsPlugins;
    case WebSchedulerTrackedFeature::kDocumentLoaded:
      return Page::BackForwardCacheNotRestoredReasonEnum::DocumentLoaded;
    case WebSchedulerTrackedFeature::kDedicatedWorkerOrWorklet:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          DedicatedWorkerOrWorklet;
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestOthers:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          OutstandingNetworkRequestOthers;
    case WebSchedulerTrackedFeature::kOutstandingIndexedDBTransaction:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          OutstandingIndexedDBTransaction;
    case WebSchedulerTrackedFeature::kRequestedNotificationsPermission:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedNotificationsPermission;
    case WebSchedulerTrackedFeature::kRequestedMIDIPermission:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedMIDIPermission;
    case WebSchedulerTrackedFeature::kRequestedAudioCapturePermission:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedAudioCapturePermission;
    case WebSchedulerTrackedFeature::kRequestedVideoCapturePermission:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedVideoCapturePermission;
    case WebSchedulerTrackedFeature::kRequestedBackForwardCacheBlockedSensors:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedBackForwardCacheBlockedSensors;
    case WebSchedulerTrackedFeature::kRequestedBackgroundWorkPermission:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedBackgroundWorkPermission;
    case WebSchedulerTrackedFeature::kBroadcastChannel:
      return Page::BackForwardCacheNotRestoredReasonEnum::BroadcastChannel;
    case WebSchedulerTrackedFeature::kIndexedDBConnection:
      return Page::BackForwardCacheNotRestoredReasonEnum::IndexedDBConnection;
    case WebSchedulerTrackedFeature::kWebXR:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebXR;
    case WebSchedulerTrackedFeature::kSharedWorker:
      return Page::BackForwardCacheNotRestoredReasonEnum::SharedWorker;
    case WebSchedulerTrackedFeature::kWebLocks:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebLocks;
    case WebSchedulerTrackedFeature::kWebHID:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebHID;
    case WebSchedulerTrackedFeature::kWebShare:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebShare;
    case WebSchedulerTrackedFeature::kRequestedStorageAccessGrant:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          RequestedStorageAccessGrant;
    case WebSchedulerTrackedFeature::kWebNfc:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebNfc;
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestFetch:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          OutstandingNetworkRequestFetch;
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestXHR:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          OutstandingNetworkRequestXHR;
    case WebSchedulerTrackedFeature::kAppBanner:
      return Page::BackForwardCacheNotRestoredReasonEnum::AppBanner;
    case WebSchedulerTrackedFeature::kPrinting:
      return Page::BackForwardCacheNotRestoredReasonEnum::Printing;
    case WebSchedulerTrackedFeature::kWebDatabase:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebDatabase;
    case WebSchedulerTrackedFeature::kPictureInPicture:
      return Page::BackForwardCacheNotRestoredReasonEnum::PictureInPicture;
    case WebSchedulerTrackedFeature::kPortal:
      return Page::BackForwardCacheNotRestoredReasonEnum::Portal;
    case WebSchedulerTrackedFeature::kSpeechRecognizer:
      return Page::BackForwardCacheNotRestoredReasonEnum::SpeechRecognizer;
    case WebSchedulerTrackedFeature::kIdleManager:
      return Page::BackForwardCacheNotRestoredReasonEnum::IdleManager;
    case WebSchedulerTrackedFeature::kPaymentManager:
      return Page::BackForwardCacheNotRestoredReasonEnum::PaymentManager;
    case WebSchedulerTrackedFeature::kSpeechSynthesis:
      return Page::BackForwardCacheNotRestoredReasonEnum::SpeechSynthesis;
    case WebSchedulerTrackedFeature::kKeyboardLock:
      return Page::BackForwardCacheNotRestoredReasonEnum::KeyboardLock;
    case WebSchedulerTrackedFeature::kWebOTPService:
      return Page::BackForwardCacheNotRestoredReasonEnum::WebOTPService;
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestDirectSocket:
      return Page::BackForwardCacheNotRestoredReasonEnum::
          OutstandingNetworkRequestDirectSocket;
    case WebSchedulerTrackedFeature::kInjectedJavascript:
      return Page::BackForwardCacheNotRestoredReasonEnum::InjectedJavascript;
    case WebSchedulerTrackedFeature::kInjectedStyleSheet:
      return Page::BackForwardCacheNotRestoredReasonEnum::InjectedStyleSheet;
    case WebSchedulerTrackedFeature::kDummy:
      // This is a test only reason and should never be called.
      NOTREACHED();
      return Page::BackForwardCacheNotRestoredReasonEnum::Dummy;
  }
}

Page::BackForwardCacheNotRestoredReason
DisableForRenderFrameHostReasonToProtocol(
    BackForwardCache::DisabledReason reason) {
  switch (reason.source) {
    case BackForwardCache::DisabledSource::kLegacy:
      NOTREACHED();
      return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
    case BackForwardCache::DisabledSource::kTesting:
      NOTREACHED();
      return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
    case BackForwardCache::DisabledSource::kContent:
      switch (
          static_cast<BackForwardCacheDisable::DisabledReasonId>(reason.id)) {
        case BackForwardCacheDisable::DisabledReasonId::kUnknown:
          return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
        case BackForwardCacheDisable::DisabledReasonId::kSecurityHandler:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentSecurityHandler;
        case BackForwardCacheDisable::DisabledReasonId::kWebAuthenticationAPI:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentWebAuthenticationAPI;
        case BackForwardCacheDisable::DisabledReasonId::kFileChooser:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentFileChooser;
        case BackForwardCacheDisable::DisabledReasonId::kSerial:
          return Page::BackForwardCacheNotRestoredReasonEnum::ContentSerial;
        case BackForwardCacheDisable::DisabledReasonId::kFileSystemAccess:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentFileSystemAccess;
        case BackForwardCacheDisable::DisabledReasonId::
            kMediaDevicesDispatcherHost:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentMediaDevicesDispatcherHost;
        case BackForwardCacheDisable::DisabledReasonId::kWebBluetooth:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentWebBluetooth;
        case BackForwardCacheDisable::DisabledReasonId::kWebUSB:
          return Page::BackForwardCacheNotRestoredReasonEnum::ContentWebUSB;
        case BackForwardCacheDisable::DisabledReasonId::kMediaSession:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentMediaSession;
        case BackForwardCacheDisable::DisabledReasonId::kMediaSessionService:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentMediaSessionService;
        case BackForwardCacheDisable::DisabledReasonId::kScreenReader:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              ContentScreenReader;
      }
    case BackForwardCache::DisabledSource::kEmbedder:
      switch (static_cast<back_forward_cache::DisabledReasonId>(reason.id)) {
        case back_forward_cache::DisabledReasonId::kUnknown:
          return Page::BackForwardCacheNotRestoredReasonEnum::Unknown;
        case back_forward_cache::DisabledReasonId::kPopupBlockerTabHelper:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderPopupBlockerTabHelper;
        case back_forward_cache::DisabledReasonId::
            kSafeBrowsingTriggeredPopupBlocker:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderSafeBrowsingTriggeredPopupBlocker;
        case back_forward_cache::DisabledReasonId::kSafeBrowsingThreatDetails:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderSafeBrowsingThreatDetails;
        case back_forward_cache::DisabledReasonId::kAppBannerManager:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderAppBannerManager;
        case back_forward_cache::DisabledReasonId::kDomDistillerViewerSource:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderDomDistillerViewerSource;
        case back_forward_cache::DisabledReasonId::
            kDomDistiller_SelfDeletingRequestDelegate:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderDomDistillerSelfDeletingRequestDelegate;
        case back_forward_cache::DisabledReasonId::kOomInterventionTabHelper:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderOomInterventionTabHelper;
        case back_forward_cache::DisabledReasonId::kOfflinePage:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderOfflinePage;
        case back_forward_cache::DisabledReasonId::
            kChromePasswordManagerClient_BindCredentialManager:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderChromePasswordManagerClientBindCredentialManager;
        case back_forward_cache::DisabledReasonId::kPermissionRequestManager:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderPermissionRequestManager;
        case back_forward_cache::DisabledReasonId::kModalDialog:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderModalDialog;
        case back_forward_cache::DisabledReasonId::kExtensions:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderExtensions;
        case back_forward_cache::DisabledReasonId::kExtensionMessaging:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderExtensionMessaging;
        case back_forward_cache::DisabledReasonId::
            kExtensionMessagingForOpenPort:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderExtensionMessagingForOpenPort;
        case back_forward_cache::DisabledReasonId::
            kExtensionSentMessageToCachedFrame:
          return Page::BackForwardCacheNotRestoredReasonEnum::
              EmbedderExtensionSentMessageToCachedFrame;
      }
  }
}

Page::BackForwardCacheNotRestoredReasonType MapNotRestoredReasonToType(
    BackForwardCacheMetrics::NotRestoredReason reason) {
  using Reason = BackForwardCacheMetrics::NotRestoredReason;
  switch (reason) {
    case Reason::kNotPrimaryMainFrame:
    case Reason::kBackForwardCacheDisabled:
    case Reason::kRelatedActiveContentsExist:
    case Reason::kHTTPStatusNotOK:
    case Reason::kSchemeNotHTTPOrHTTPS:
    case Reason::kLoading:
    case Reason::kWasGrantedMediaAccess:
    case Reason::kDisableForRenderFrameHostCalled:
    case Reason::kDomainNotAllowed:
    case Reason::kHTTPMethodNotGET:
    case Reason::kSubframeIsNavigating:
    case Reason::kTimeout:
    case Reason::kCacheLimit:
    case Reason::kJavaScriptExecution:
    case Reason::kRendererProcessKilled:
    case Reason::kRendererProcessCrashed:
    case Reason::kGrantedMediaStreamAccess:
    case Reason::kSchedulerTrackedFeatureUsed:
    case Reason::kConflictingBrowsingInstance:
    case Reason::kCacheFlushed:
    case Reason::kServiceWorkerVersionActivation:
    case Reason::kSessionRestored:
    case Reason::kServiceWorkerPostMessage:
    case Reason::kEnteredBackForwardCacheBeforeServiceWorkerHostAdded:
    case Reason::kNotMostRecentNavigationEntry:
    case Reason::kServiceWorkerClaim:
    case Reason::kIgnoreEventAndEvict:
    case Reason::kHaveInnerContents:
    case Reason::kTimeoutPuttingInCache:
    case Reason::kBackForwardCacheDisabledByLowMemory:
    case Reason::kBackForwardCacheDisabledByCommandLine:
    case Reason::kNetworkRequestRedirected:
    case Reason::kNetworkRequestTimeout:
    case Reason::kNetworkExceedsBufferLimit:
    case Reason::kNavigationCancelledWhileRestoring:
    case Reason::kForegroundCacheLimit:
    case Reason::kUserAgentOverrideDiffers:
    case Reason::kBrowsingInstanceNotSwapped:
    case Reason::kBackForwardCacheDisabledForDelegate:
    case Reason::kServiceWorkerUnregistration:
    case Reason::kCacheControlNoStore:
    case Reason::kCacheControlNoStoreCookieModified:
    case Reason::kCacheControlNoStoreHTTPOnlyCookieModified:
    case Reason::kNoResponseHead:
    case Reason::kErrorDocument:
    case Reason::kFencedFramesEmbedder:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::Circumstantial;
    case Reason::kUnloadHandlerExistsInMainFrame:
    case Reason::kUnloadHandlerExistsInSubFrame:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::PageSupportNeeded;
    case Reason::kNetworkRequestDatapipeDrainedAsBytesConsumer:
    case Reason::kUnknown:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::SupportPending;
    case Reason::kBlocklistedFeatures:
      NOTREACHED();
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::PageSupportNeeded;
  }
}

Page::BackForwardCacheNotRestoredReasonType MapBlocklistedFeatureToType(
    WebSchedulerTrackedFeature feature) {
  switch (feature) {
    case WebSchedulerTrackedFeature::kWebRTC:
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestOthers:
    case WebSchedulerTrackedFeature::kOutstandingIndexedDBTransaction:
    case WebSchedulerTrackedFeature::kBroadcastChannel:
    case WebSchedulerTrackedFeature::kIndexedDBConnection:
    case WebSchedulerTrackedFeature::kWebXR:
    case WebSchedulerTrackedFeature::kSharedWorker:
    case WebSchedulerTrackedFeature::kWebHID:
    case WebSchedulerTrackedFeature::kWebShare:
    case WebSchedulerTrackedFeature::kWebDatabase:
    case WebSchedulerTrackedFeature::kPaymentManager:
    case WebSchedulerTrackedFeature::kKeyboardLock:
    case WebSchedulerTrackedFeature::kWebOTPService:
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestDirectSocket:
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestFetch:
    case WebSchedulerTrackedFeature::kOutstandingNetworkRequestXHR:
    case WebSchedulerTrackedFeature::kWebTransport:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::PageSupportNeeded;
    case WebSchedulerTrackedFeature::kPortal:
    case WebSchedulerTrackedFeature::kWebNfc:
    case WebSchedulerTrackedFeature::kRequestedStorageAccessGrant:
    case WebSchedulerTrackedFeature::kRequestedNotificationsPermission:
    case WebSchedulerTrackedFeature::kRequestedMIDIPermission:
    case WebSchedulerTrackedFeature::kRequestedAudioCapturePermission:
    case WebSchedulerTrackedFeature::kRequestedVideoCapturePermission:
    case WebSchedulerTrackedFeature::kRequestedBackForwardCacheBlockedSensors:
    case WebSchedulerTrackedFeature::kRequestedBackgroundWorkPermission:
    case WebSchedulerTrackedFeature::kContainsPlugins:
    case WebSchedulerTrackedFeature::kIdleManager:
    case WebSchedulerTrackedFeature::kSpeechRecognizer:
    case WebSchedulerTrackedFeature::kPrinting:
    case WebSchedulerTrackedFeature::kPictureInPicture:
    case WebSchedulerTrackedFeature::kWebLocks:
    case WebSchedulerTrackedFeature::kAppBanner:
    case WebSchedulerTrackedFeature::kWebSocket:
    case WebSchedulerTrackedFeature::kDedicatedWorkerOrWorklet:
    case WebSchedulerTrackedFeature::kSpeechSynthesis:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::SupportPending;
    case WebSchedulerTrackedFeature::kMainResourceHasCacheControlNoStore:
    case WebSchedulerTrackedFeature::kMainResourceHasCacheControlNoCache:
    case WebSchedulerTrackedFeature::kSubresourceHasCacheControlNoCache:
    case WebSchedulerTrackedFeature::kSubresourceHasCacheControlNoStore:
    case WebSchedulerTrackedFeature::kInjectedStyleSheet:
    case WebSchedulerTrackedFeature::kInjectedJavascript:
    case WebSchedulerTrackedFeature::kDocumentLoaded:
    case WebSchedulerTrackedFeature::kDummy:
      return Page::BackForwardCacheNotRestoredReasonTypeEnum::Circumstantial;
  }
}

Page::BackForwardCacheNotRestoredReasonType
MapDisableForRenderFrameHostReasonToType(
    BackForwardCache::DisabledReason reason) {
  return Page::BackForwardCacheNotRestoredReasonTypeEnum::SupportPending;
}

std::unique_ptr<protocol::Array<Page::BackForwardCacheNotRestoredExplanation>>
CreateNotRestoredExplanation(
    const BackForwardCacheCanStoreDocumentResult::NotStoredReasons
        not_stored_reasons,
    const blink::scheduler::WebSchedulerTrackedFeatures blocklisted_features,
    const std::set<BackForwardCache::DisabledReason>& disabled_reasons) {
  auto reasons = std::make_unique<
      protocol::Array<Page::BackForwardCacheNotRestoredExplanation>>();

  for (BackForwardCacheMetrics::NotRestoredReason reason : not_stored_reasons) {
    if (reason ==
        BackForwardCacheMetrics::NotRestoredReason::kBlocklistedFeatures) {
      DCHECK(!blocklisted_features.Empty());
      for (blink::scheduler::WebSchedulerTrackedFeature feature :
           blocklisted_features) {
        reasons->emplace_back(
            Page::BackForwardCacheNotRestoredExplanation::Create()
                .SetType(MapBlocklistedFeatureToType(feature))
                .SetReason(BlocklistedFeatureToProtocol(feature))
                .Build());
      }
    } else if (reason == BackForwardCacheMetrics::NotRestoredReason::
                             kDisableForRenderFrameHostCalled) {
      for (auto disabled_reason : disabled_reasons) {
        auto reason =
            Page::BackForwardCacheNotRestoredExplanation::Create()
                .SetType(
                    MapDisableForRenderFrameHostReasonToType(disabled_reason))
                .SetReason(
                    DisableForRenderFrameHostReasonToProtocol(disabled_reason))
                .Build();
        if (!disabled_reason.context.empty())
          reason->SetContext(disabled_reason.context);
        reasons->emplace_back(std::move(reason));
      }
    } else {
      reasons->emplace_back(
          Page::BackForwardCacheNotRestoredExplanation::Create()
              .SetType(MapNotRestoredReasonToType(reason))
              .SetReason(NotRestoredReasonToProtocol(reason))
              .Build());
    }
  }
  return reasons;
}

std::unique_ptr<Page::BackForwardCacheNotRestoredExplanationTree>
CreateNotRestoredExplanationTree(
    const BackForwardCacheCanStoreTreeResult& tree_result) {
  auto explanation = CreateNotRestoredExplanation(
      tree_result.GetDocumentResult().not_stored_reasons(),
      tree_result.GetDocumentResult().blocklisted_features(),
      tree_result.GetDocumentResult().disabled_reasons());

  auto children_array = std::make_unique<
      protocol::Array<Page::BackForwardCacheNotRestoredExplanationTree>>();
  for (auto& child : tree_result.GetChildren()) {
    children_array->emplace_back(
        CreateNotRestoredExplanationTree(*(child.get())));
  }
  return Page::BackForwardCacheNotRestoredExplanationTree::Create()
      .SetUrl(tree_result.GetUrl().spec())
      .SetExplanations(std::move(explanation))
      .SetChildren(std::move(children_array))
      .Build();
}

Response PageHandler::AddCompilationCache(const std::string& url,
                                          const Binary& data) {
  // We're just checking a permission here, the real business happens
  // in the renderer, if we fall through.
  if (allow_unsafe_operations_)
    return Response::FallThrough();
  return Response::ServerError("Permission denied");
}

void PageHandler::BackForwardCacheNotUsed(
    const NavigationRequest* navigation,
    const BackForwardCacheCanStoreDocumentResult* result,
    const BackForwardCacheCanStoreTreeResult* tree_result) {
  if (!enabled_)
    return;

  FrameTreeNode* ftn = navigation->frame_tree_node();
  std::string devtools_navigation_token =
      navigation->devtools_navigation_token().ToString();
  std::string frame_id = ftn->devtools_frame_token().ToString();

  auto explanation = CreateNotRestoredExplanation(
      result->not_stored_reasons(), result->blocklisted_features(),
      result->disabled_reasons());

  // TODO(crbug.com/1281855): |tree_result| should not be nullptr when |result|
  // has the reasons.
  std::unique_ptr<Page::BackForwardCacheNotRestoredExplanationTree>
      explanation_tree =
          tree_result ? CreateNotRestoredExplanationTree(*tree_result)
                      : nullptr;
  frontend_->BackForwardCacheNotUsed(devtools_navigation_token, frame_id,
                                     std::move(explanation),
                                     std::move(explanation_tree));
}

void PageHandler::DidActivatePrerender(const NavigationRequest& nav_request) {
  if (!enabled_)
    return;
  FrameTreeNode* ftn = nav_request.frame_tree_node();
  std::string initiating_frame_id = ftn->devtools_frame_token().ToString();
  const GURL& prerendering_url = nav_request.common_params().url;
  frontend_->PrerenderAttemptCompleted(
      initiating_frame_id, prerendering_url.spec(),
      Page::PrerenderFinalStatusEnum::Activated);
}

bool PageHandler::ShouldBypassCSP() {
  return enabled_ && bypass_csp_;
}

}  // namespace protocol
}  // namespace content
