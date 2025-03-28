// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/navigator.h"

#include <utility>

#include "base/check_op.h"
#include "base/debug/dump_without_crashing.h"
#include "base/metrics/histogram_macros.h"
#include "base/notreached.h"
#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/process_lock.h"
#include "content/browser/renderer_host/debug_urls.h"
#include "content/browser/renderer_host/frame_tree.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/navigation_controller_impl.h"
#include "content/browser/renderer_host/navigation_entry_impl.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/navigation_request_info.h"
#include "content/browser/renderer_host/navigator_delegate.h"
#include "content/browser/renderer_host/render_frame_host_delegate.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/site_info.h"
#include "content/browser/site_instance_impl.h"
#include "content/browser/web_package/prefetched_signed_exchange_cache.h"
#include "content/browser/web_package/web_bundle_handle_tracker.h"
#include "content/browser/webui/web_ui_controller_factory_registry.h"
#include "content/browser/webui/web_ui_impl.h"
#include "content/common/navigation_params_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/global_request_id.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/restore_type.h"
#include "content/public/common/bindings_policy.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/url_utils.h"
#include "net/base/net_errors.h"
#include "services/metrics/public/cpp/ukm_builders.h"
#include "services/metrics/public/cpp/ukm_recorder.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "third_party/blink/public/common/loader/inter_process_time_ticks_converter.h"
#include "third_party/blink/public/common/navigation/navigation_policy.h"
#include "third_party/blink/public/mojom/navigation/navigation_params.mojom.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace nw {
  typedef bool(*RphGuestFilterURLHookFn)(content::RenderProcessHost* rph, const GURL* url);
  extern RphGuestFilterURLHookFn gRphGuestFilterURLHook;
}

namespace content {

namespace {

using WebFeature = blink::mojom::WebFeature;
using CrossOriginOpenerPolicyValue =
    network::mojom::CrossOriginOpenerPolicyValue;
using CrossOriginEmbedderPolicyValue =
    network::mojom::CrossOriginEmbedderPolicyValue;

// Map Cross-Origin-Opener-Policy header value to its corresponding WebFeature.
absl::optional<WebFeature> FeatureCoop(CrossOriginOpenerPolicyValue value) {
  switch (value) {
    case CrossOriginOpenerPolicyValue::kUnsafeNone:
      return absl::nullopt;
    case CrossOriginOpenerPolicyValue::kSameOrigin:
      return WebFeature::kCrossOriginOpenerPolicySameOrigin;
    case CrossOriginOpenerPolicyValue::kSameOriginAllowPopups:
      return WebFeature::kCrossOriginOpenerPolicySameOriginAllowPopups;
    case CrossOriginOpenerPolicyValue::kSameOriginPlusCoep:
    case CrossOriginOpenerPolicyValue::kSameOriginAllowPopupsPlusCoep:
      return WebFeature::kCoopAndCoepIsolated;
  }
}

// Map Cross-Origin-Opener-Policy-Report-Only header value to its corresponding
// WebFeature.
absl::optional<WebFeature> FeatureCoopRO(CrossOriginOpenerPolicyValue value) {
  switch (value) {
    case CrossOriginOpenerPolicyValue::kUnsafeNone:
      return absl::nullopt;
    case CrossOriginOpenerPolicyValue::kSameOrigin:
      return WebFeature::kCrossOriginOpenerPolicySameOriginReportOnly;
    case CrossOriginOpenerPolicyValue::kSameOriginAllowPopups:
      return WebFeature::
          kCrossOriginOpenerPolicySameOriginAllowPopupsReportOnly;
    case CrossOriginOpenerPolicyValue::kSameOriginPlusCoep:
    case CrossOriginOpenerPolicyValue::kSameOriginAllowPopupsPlusCoep:
      return WebFeature::kCoopAndCoepIsolatedReportOnly;
  }
}

// Map Cross-Origin-Embedder-Policy header value to its
// corresponding WebFeature.
absl::optional<WebFeature> FeatureCoep(CrossOriginEmbedderPolicyValue value) {
  switch (value) {
    case CrossOriginEmbedderPolicyValue::kNone:
      return absl::nullopt;
    case CrossOriginEmbedderPolicyValue::kCredentialless:
      return WebFeature::kCrossOriginEmbedderPolicyCredentialless;
    case CrossOriginEmbedderPolicyValue::kRequireCorp:
      return WebFeature::kCrossOriginEmbedderPolicyRequireCorp;
  }
}

// Map Cross-Origin-Embedder-Policy-Report-Only header value to its
// corresponding WebFeature.
absl::optional<WebFeature> FeatureCoepRO(CrossOriginEmbedderPolicyValue value) {
  switch (value) {
    case CrossOriginEmbedderPolicyValue::kNone:
      return absl::nullopt;
    case CrossOriginEmbedderPolicyValue::kCredentialless:
      return WebFeature::kCrossOriginEmbedderPolicyCredentiallessReportOnly;
    case CrossOriginEmbedderPolicyValue::kRequireCorp:
      return WebFeature::kCrossOriginEmbedderPolicyRequireCorpReportOnly;
  }
}

// TODO(titouan): Move the feature computation logic into `NavigationRequest`,
// and use `NavigationRequest::TakeWebFeatureToLog()` to record them later.
void RecordWebPlatformSecurityMetrics(RenderFrameHostImpl* rfh,
                                      bool has_embedding_control,
                                      bool is_error_page) {
  ContentBrowserClient* client = GetContentClient()->browser();

  auto log = [&](absl::optional<WebFeature> feature) {
    if (feature)
      client->LogWebFeatureForCurrentPage(rfh, feature.value());
  };

  // [COOP]
  if (rfh->IsInPrimaryMainFrame()) {
    log(FeatureCoop(rfh->cross_origin_opener_policy().value));
    log(FeatureCoopRO(rfh->cross_origin_opener_policy().report_only_value));

    if (rfh->cross_origin_opener_policy().reporting_endpoint ||
        rfh->cross_origin_opener_policy().report_only_reporting_endpoint) {
      log(WebFeature::kCrossOriginOpenerPolicyReporting);
    }
  }

  // [COEP]
  log(FeatureCoep(rfh->cross_origin_embedder_policy().value));
  log(FeatureCoepRO(rfh->cross_origin_embedder_policy().report_only_value));

  // Record iframes embedded in cross-origin contexts without a CSP
  // frame-ancestor directive.
  bool is_embedded_in_cross_origin_context = false;
  RenderFrameHostImpl* parent = rfh->GetParent();
  while (parent) {
    if (!parent->GetLastCommittedOrigin().IsSameOriginWith(
            rfh->GetLastCommittedOrigin())) {
      is_embedded_in_cross_origin_context = true;
      break;
    }
    parent = parent->GetParent();
  }

  if (is_embedded_in_cross_origin_context && !has_embedding_control &&
      !is_error_page) {
    log(WebFeature::kCrossOriginSubframeWithoutEmbeddingControl);
    RenderFrameHostImpl* main_frame = rfh->GetMainFrame();
    ukm::builders::CrossOriginSubframeWithoutEmbeddingControl(
        main_frame->GetPageUkmSourceId())
        .SetSubframeEmbedded(1)
        .Record(ukm::UkmRecorder::Get());
  }

  // Webview tag guests do not follow regular process model decisions. They
  // always stay in their original SiteInstance, regardless of COOP. Assumption
  // made below about COOP:same-origin and unsafe-none never being in the same
  // BrowsingInstance does not hold. See https://crbug.com/1243711.
  if (rfh->GetSiteInstance()->IsGuest())
    return;

  // Check if the navigation resulted in having same-origin documents in pages
  // with different COOP status inside the browsing context group.
  RenderFrameHostImpl* top_level_document =
      rfh->frame_tree_node()->frame_tree()->GetMainFrame();
  network::mojom::CrossOriginOpenerPolicyValue page_coop =
      top_level_document->cross_origin_opener_policy().value;
  for (RenderFrameHostImpl* other_tld :
       rfh->delegate()->GetActiveTopLevelDocumentsInBrowsingContextGroup(rfh)) {
    network::mojom::CrossOriginOpenerPolicyValue other_page_coop =
        other_tld->cross_origin_opener_policy().value;
    if (page_coop == other_page_coop)
      continue;

    using CoopValue = network::mojom::CrossOriginOpenerPolicyValue;
    DCHECK((page_coop == CoopValue::kSameOriginAllowPopups &&
            other_page_coop == CoopValue::kUnsafeNone) ||
           (page_coop == CoopValue::kUnsafeNone &&
            other_page_coop == CoopValue::kSameOriginAllowPopups));
    for (FrameTreeNode* frame_tree_node :
         other_tld->frame_tree_node()->frame_tree()->Nodes()) {
      RenderFrameHostImpl* other_rfh = frame_tree_node->current_frame_host();
      if (other_rfh->lifecycle_state() ==
              RenderFrameHostImpl::LifecycleStateImpl::kActive &&
          rfh->GetLastCommittedOrigin().IsSameOriginWith(
              other_rfh->GetLastCommittedOrigin())) {
        // Always log the feature on the COOP same-origin-allow-popups page,
        // since this is the spec we are trying to change.
        RenderFrameHostImpl* rfh_to_log =
            page_coop == CoopValue::kSameOriginAllowPopups ? rfh : other_rfh;
        client->LogWebFeatureForCurrentPage(
            rfh_to_log, blink::mojom::WebFeature::
                            kSameOriginDocumentsWithDifferentCOOPStatus);
      }
    }
  }
}

// Records the fact that `rfh` made use of `web_features`.
void RecordMetrics(RenderFrameHostImpl& rfh,
                   const std::vector<blink::mojom::WebFeature>& web_features) {
  ContentBrowserClient& client = *GetContentClient()->browser();
  for (const auto feature : web_features) {
    client.LogWebFeatureForCurrentPage(&rfh, feature);
  }
}

bool HasEmbeddingControl(NavigationRequest* navigation_request) {
  if (!navigation_request->response())
    // This navigation did not result in a network request. The embedding of
    // the frame is not controlled by network headers.
    return true;

  // Check if the request has a CSP frame-ancestor directive.
  for (const auto& csp : navigation_request->response()
                             ->parsed_headers->content_security_policy) {
    if (csp->header->type ==
            network::mojom::ContentSecurityPolicyType::kEnforce &&
        csp->directives.contains(
            network::mojom::CSPDirectiveName::FrameAncestors)) {
      return true;
    }
  }

  return false;
}

bool ValidateUnfencedTopNavigation(RenderFrameHostImpl* render_frame_host,
                                   const GURL& url,
                                   int initiator_process_id,
                                   bool user_gesture) {
  // It should only be possible to send this IPC with this flag from an
  // MPArch opaque-ads fenced frame. Opaque-ads fenced frames should always
  // have the sandbox flag `allow-top-navigation-by-user-activation`.
  if (!render_frame_host->IsInFencedFrameTree() ||
      !blink::features::IsFencedFramesMPArchBased() ||
      (render_frame_host->GetMainFrame()
           ->frame_tree_node()
           ->GetFencedFrameMode() !=
       blink::mojom::FencedFrameMode::kOpaqueAds) ||
      render_frame_host->IsSandboxed(
          network::mojom::WebSandboxFlags::kTopNavigationByUserActivation)) {
    // If we get the IPC elsewhere, assume the renderer is compromised.
    bad_message::ReceivedBadMessage(
        initiator_process_id,
        bad_message::RFHI_UNFENCED_TOP_IPC_OUTSIDE_FENCED_FRAME);
    return false;
  }

  // Perform checks that normally would be performed in
  // `blink::CanNavigateHelper` but that we skipped because the target
  // frame wasn't available in the renderer.
  // TODO(crbug.com/1123606): Clean this up (make it use a common code path
  // for maintainability) after OT if possible.

  // Javascript URLs are not allowed, because they can be used to
  // communicate from the fenced frame to the embedder.
  // TODO(crbug.com/1315802): It does not seem possible to reach this code
  // with an uncompromised renderer, because javascript URLs don't reach
  // the same IPC; instead they run inside the fenced frame as _self.
  // It also seems that Javascript URLs would be caught earlier in this
  // particular code path by VerifyOpenURLParams().
  // In this code's final IPC resting place after the factor, make sure
  // to check whether this code is redundant.
  if (url.SchemeIs(url::kJavaScriptScheme)) {
    render_frame_host->AddMessageToConsole(
        blink::mojom::ConsoleMessageLevel::kError,
        "The frame attempting navigation must be in the same fenced "
        "frame tree as the target if navigating to a javascript: url");
    return false;
  }

  // User activation is required, because fenced frames use the sandbox
  // flag `allow-top-navigation-by-user-activation`.
  // It would be better to instead check
  // `render_frame_host->frame_tree_node()->HasTransientUserActivation()`,
  // but it has already been consumed at this point.
  // TODO(crbug.com/848778): use the browser's source of truth for user
  // activation here (and elsewhere in this file) rather than trust the
  // renderer.
  if (!user_gesture) {
    render_frame_host->AddMessageToConsole(
        blink::mojom::ConsoleMessageLevel::kError,
        "The frame attempting navigation of the top-level window is "
        "sandboxed with the 'allow-top-navigation-by-user-activation' "
        "flag, but has no user activation (aka gesture). See "
        "https://www.chromestatus.com/feature/5629582019395584.");
    return false;
  }

  return true;
}

}  // namespace

struct Navigator::NavigationMetricsData {
  NavigationMetricsData(base::TimeTicks start_time,
                        GURL url,
                        ukm::SourceId ukm_source_id,
                        bool is_browser_initiated_before_unload)
      : start_time_(start_time),
        url_(url),
        ukm_source_id_(ukm_source_id),
        is_browser_initiated_before_unload_(
            is_browser_initiated_before_unload) {}

  base::TimeTicks start_time_;
  GURL url_;
  ukm::SourceId ukm_source_id_;
  bool is_browser_initiated_before_unload_;

  // Timestamps before_unload_(start|end)_ give the time it took to run
  // beforeunloads dispatched from the browser process. For browser-initiated
  // navigations this includes all frames (all beforeunload handlers on a page).
  // For renderer-initiated navigations this just includes OOPIFs since local
  // beforeunloads will have been run in the renderer before dispatching the
  // navigation IPC.
  absl::optional<base::TimeTicks> before_unload_start_;
  absl::optional<base::TimeTicks> before_unload_end_;

  // Time at which the browser process received a navigation request and
  // dispatched beforeunloads to the renderer.
  absl::optional<base::TimeTicks> before_unload_sent_;

  // Timestamps renderer_before_unload_(start|end)_ give the time it took to run
  // beforeunloads for local frames in a renderer-initiated navigation, prior to
  // notifying the browser process about the navigation.
  absl::optional<base::TimeTicks> renderer_before_unload_start_;
  absl::optional<base::TimeTicks> renderer_before_unload_end_;

  // Time at which the browser process dispatched the CommitNavigation to the
  // renderer.
  absl::optional<base::TimeTicks> commit_navigation_sent_;
};

Navigator::Navigator(
    BrowserContext* browser_context,
    FrameTree& frame_tree,
    NavigatorDelegate* delegate,
    NavigationControllerDelegate* navigation_controller_delegate)
    : controller_(browser_context, frame_tree, navigation_controller_delegate),
      delegate_(delegate) {}

Navigator::~Navigator() = default;

// static
bool Navigator::CheckWebUIRendererDoesNotDisplayNormalURL(
    RenderFrameHostImpl* render_frame_host,
    const UrlInfo& url_info,
    bool is_renderer_initiated_check) {
  const GURL& url = url_info.url;
  // In single process mode, everything runs in the same process, so the checks
  // below are irrelevant.
  if (RenderProcessHost::run_renderer_in_process())
    return true;

  // In the case of error page process, any URL is allowed to commit.
  ProcessLock process_lock = render_frame_host->GetProcess()->GetProcessLock();
  if (process_lock.is_error_page())
    return true;

  bool frame_has_bindings = ((render_frame_host->GetEnabledBindings() &
                              kWebUIBindingsPolicyMask) != 0);
  bool is_allowed_in_web_ui_renderer =
      WebUIControllerFactoryRegistry::GetInstance()->IsURLAcceptableForWebUI(
          render_frame_host->GetProcess()->GetBrowserContext(), url);

  // Embedders might disable locking for WebUI URLs, which is bad idea, however
  // this method should take this into account.
  SiteInstanceImpl* site_instance = render_frame_host->GetSiteInstance();
  SiteInfo site_info = site_instance->DeriveSiteInfo(url_info);
  bool should_lock_process =
      site_info.ShouldLockProcessToSite(site_instance->GetIsolationContext());

  // If the |render_frame_host| has any WebUI bindings, disallow URLs that are
  // not allowed in a WebUI renderer process.
  if (frame_has_bindings) {
    // The process itself must have WebUI bit in the security policy.
    // Otherwise it indicates that there is a bug in browser process logic and
    // the browser process must be terminated.
    // TODO(nasko): Convert to CHECK() once it is confirmed this is not
    // violated in reality.
    if (!ChildProcessSecurityPolicyImpl::GetInstance()->HasWebUIBindings(
            render_frame_host->GetProcess()->GetID())) {
      base::debug::DumpWithoutCrashing();
    }

    // Check whether the process must be locked and if so that the process lock
    // is indeed in place.
    if (should_lock_process && !process_lock.is_locked_to_site())
      return false;

    // There must be a WebUI on the frame.
    if (!render_frame_host->web_ui())
      return false;

    // The |url| must be allowed in a WebUI process if the frame has WebUI.
    if (!is_allowed_in_web_ui_renderer) {
      // If this method is called in response to IPC message from the renderer
      // process, it should be terminated, otherwise it is a bug in the
      // navigation logic and the browser process should be terminated to avoid
      // exposing users to security issues.
      if (is_renderer_initiated_check)
        return false;

      CHECK(false);
    }
  }

  // If `url` is one that is allowed in WebUI renderer process, ensure that its
  // origin is either opaque or its process lock matches the RFH process lock.
  if (is_allowed_in_web_ui_renderer) {
    url::Origin url_origin =
        url::Origin::Create(url.DeprecatedGetOriginAsURL());

    // Verify `site_info`'s process lock matches the RFH's process lock, if one
    // is in place.
    if (should_lock_process) {
      if (!url_origin.opaque() &&
          process_lock != ProcessLock::FromSiteInfo(site_info)) {
        return false;
      }
    }
  }

  return true;
}

// A renderer-initiated navigation should be ignored iff a) there is an ongoing
// request b) which is browser initiated or a history traversal and c) the
// renderer request is not user-initiated.
// Renderer-initiated history traversals cause navigations to be ignored for
// compatibility reasons - this behavior is asserted by several web platform
// tests.
// static
bool Navigator::ShouldIgnoreIncomingRendererRequest(
    const NavigationRequest* ongoing_navigation_request,
    bool has_user_gesture) {
  return ongoing_navigation_request &&
         (ongoing_navigation_request->browser_initiated() ||
          NavigationTypeUtils::IsHistory(
              ongoing_navigation_request->common_params().navigation_type)) &&
         !has_user_gesture;
}

NavigatorDelegate* Navigator::GetDelegate() {
  return delegate_;
}

void Navigator::DidFailLoadWithError(RenderFrameHostImpl* render_frame_host,
                                     const GURL& url,
                                     int error_code) {
  delegate_->DidFailLoadWithError(render_frame_host, url, error_code);
}

bool Navigator::StartHistoryNavigationInNewSubframe(
    RenderFrameHostImpl* render_frame_host,
    mojo::PendingAssociatedRemote<mojom::NavigationClient>* navigation_client) {
  return controller_.StartHistoryNavigationInNewSubframe(render_frame_host,
                                                         navigation_client);
}

void Navigator::DidNavigate(
    RenderFrameHostImpl* render_frame_host,
    const mojom::DidCommitProvisionalLoadParams& params,
    std::unique_ptr<NavigationRequest> navigation_request,
    bool was_within_same_document) {
  DCHECK(navigation_request);
  FrameTreeNode* frame_tree_node = render_frame_host->frame_tree_node();
  FrameTree* frame_tree = frame_tree_node->frame_tree();
  DCHECK_EQ(frame_tree, &controller_.frame_tree());
  base::WeakPtr<RenderFrameHostImpl> old_frame_host =
      frame_tree_node->render_manager()->current_frame_host()->GetWeakPtr();

  // At this point we have already chosen a SiteInstance for this navigation, so
  // set OriginIsolationRequest to kNone in the conversion to UrlInfo below:
  // this is done implicitly in the UrlInfoInit constructor.
  const UrlInfo url_info(UrlInfoInit(params.url));

  if (auto& old_page_info = navigation_request->commit_params().old_page_info) {
    // This is a same-site main-frame navigation where we did a proactive
    // BrowsingInstance swap but we're reusing the old page's process, and we
    // have dispatched the pagehide and visibilitychange handlers of the old
    // page when we committed the new page.
    auto* page_lifecycle_state_manager =
        old_frame_host->render_view_host()->GetPageLifecycleStateManager();
    page_lifecycle_state_manager->DidSetPagehideDispatchDuringNewPageCommit(
        std::move(old_page_info->new_lifecycle_state_for_old_page));
  }

  // If a frame claims the navigation was same-document, it must be the current
  // frame, not a pending one.
  if (was_within_same_document && render_frame_host != old_frame_host.get()) {
    was_within_same_document = false;
  }

  if (ui::PageTransitionIsMainFrame(params.transition)) {
    // Run tasks that must execute just before the commit.
    delegate_->DidNavigateMainFramePreCommit(frame_tree_node,
                                             was_within_same_document);
  }

  // DidNavigateFrame() must be called before replicating the new origin and
  // other properties to proxies.  This is because it destroys the subframes of
  // the frame we're navigating from, which might trigger those subframes to
  // run unload handlers.  Those unload handlers should still see the old
  // frame's origin.  See https://crbug.com/825283.
  frame_tree_node->render_manager()->DidNavigateFrame(
      render_frame_host, navigation_request->common_params().has_user_gesture,
      was_within_same_document,
      navigation_request->coop_status()
          .require_browsing_instance_swap() /* clear_proxies_on_commit */,
      navigation_request->commit_params().frame_policy);

  // Save the new page's origin and other properties, and replicate them to
  // proxies, including the proxy created in DidNavigateFrame() to replace the
  // old frame in cross-process navigation cases.
  render_frame_host->browsing_context_state()->SetCurrentOrigin(
      params.origin, params.has_potentially_trustworthy_unique_origin);
  render_frame_host->browsing_context_state()->SetInsecureRequestPolicy(
      params.insecure_request_policy);
  render_frame_host->browsing_context_state()->SetInsecureNavigationsSet(
      params.insecure_navigations_set);

  // Save the activation status of the previous page here before it gets reset
  // in FrameTreeNode::ResetForNavigation.
  bool previous_document_was_activated =
      frame_tree->root()->HasStickyUserActivation();

  if (!was_within_same_document) {
    // Navigating to a new location means a new, fresh set of http headers
    // and/or <meta> elements - we need to reset Permissions Policy.
    frame_tree_node->ResetForNavigation();
  }

  // Update the site of the SiteInstance if it doesn't have one yet, unless
  // assigning a site is not necessary for this URL. In that case, the
  // SiteInstance can still be considered unused until a navigation to a real
  // page.
  SiteInstanceImpl* site_instance = render_frame_host->GetSiteInstance();
  if (!site_instance->HasSite() &&
      SiteInstanceImpl::ShouldAssignSiteForURL(url_info.url)) {
    site_instance->ConvertToDefaultOrSetSite(url_info);
  }

  // Need to update MIME type here because it's referred to in
  // UpdateNavigationCommands() called by RendererDidNavigate() to
  // determine whether or not to enable the encoding menu.
  // It's updated only for the main frame. For a subframe,
  // RenderView::UpdateURL does not set params.contents_mime_type.
  // (see http://code.google.com/p/chromium/issues/detail?id=2929 )
  // TODO(jungshik): Add a test for the encoding menu to avoid
  // regressing it again.
  // TODO(nasko): Verify the correctness of the above comment, since some of the
  // code doesn't exist anymore. Also, move this code in the
  // PageTransitionIsMainFrame code block above.
  if (ui::PageTransitionIsMainFrame(params.transition)) {
    render_frame_host->GetPage().SetContentsMimeType(params.contents_mime_type);
  }

  // RenderFrameHostImpl::DidNavigate will update the url, and may cause the
  // node to consider itself no longer on the initial empty document. Record
  // whether we're leaving the initial empty document before that.
  bool was_on_initial_empty_document =
      frame_tree_node->is_on_initial_empty_document();

  render_frame_host->DidNavigate(params, navigation_request.get(),
                                 was_within_same_document);

  int old_entry_count = controller_.GetEntryCount();
  LoadCommittedDetails details;
  base::TimeTicks start = base::TimeTicks::Now();
  bool did_navigate = controller_.RendererDidNavigate(
      render_frame_host, params, &details, was_within_same_document,
      was_on_initial_empty_document, previous_document_was_activated,
      navigation_request.get());
  if (!was_within_same_document) {
    base::UmaHistogramTimes(
        base::StrCat(
            {"Navigation.RendererDidNavigateTime.",
             render_frame_host->GetParent() ? "Subframe" : "MainFrame"}),
        base::TimeTicks::Now() - start);
  }

  // Update the RenderFrameHost's last committed FrameNavigationEntry, to have a
  // record of it in rare cases where the last committed NavigationEntry may not
  // agree. Always update this even if the FrameNavigationEntry is null after
  // RendererDidNavigate, to ensure that a stale copy is not kept around.
  // TODO(https://crbug.com/608402): Eliminate cases where the
  // FrameNavigationEntry can be null after RendererDidNavigate.
  // TODO(https://crbug.com/1304466): Merge this with
  // RenderFrameHostImpl::DidNavigate if that can be moved after
  // RendererDidNavigate, allowing us to avoid duplicating the URL and origin in
  // RenderFrameHost.
  FrameNavigationEntry* frame_entry = nullptr;
  if (controller_.GetLastCommittedEntry()) {
    frame_entry =
        controller_.GetLastCommittedEntry()->GetFrameEntry(frame_tree_node);
  }
  render_frame_host->set_last_committed_frame_entry(frame_entry);

  // If the history length and/or offset changed, update other renderers in the
  // FrameTree.
  if (old_entry_count != controller_.GetEntryCount() ||
      details.previous_entry_index !=
          controller_.GetLastCommittedEntryIndex()) {
    frame_tree->root()->render_manager()->ExecutePageBroadcastMethod(
        base::BindRepeating(
            [](int history_offset, int history_count, RenderViewHostImpl* rvh) {
              if (auto& broadcast = rvh->GetAssociatedPageBroadcast())
                broadcast->SetHistoryOffsetAndLength(history_offset,
                                                     history_count);
            },
            controller_.GetLastCommittedEntryIndex(),
            controller_.GetEntryCount()),
        site_instance);
  }

  // Store some information for recording WebPlatform security metrics. These
  // metrics depends on information present in the NavigationRequest. However
  // they must be recorded after the NavigationRequest has been destroyed and
  // DidFinishNavigation was called, otherwise they will not be properly
  // attributed to the right page.
  bool has_embedding_control = HasEmbeddingControl(navigation_request.get());
  bool is_error_page = navigation_request->IsErrorPage();
  const GURL original_request_url = navigation_request->GetOriginalRequestURL();

  // Get the list of web features that the navigating document made use of
  // before it could commit. We attribute these to the new document once it
  // has committed.
  std::vector<blink::mojom::WebFeature> web_features =
      navigation_request->TakeWebFeaturesToLog();

  // Navigations that activate an existing bfcached or prerendered document do
  // not create a new document.
  bool did_create_new_document =
      !navigation_request->IsPageActivation() && !was_within_same_document;

  // Send notification about committed provisional loads. This notification is
  // different from the NAV_ENTRY_COMMITTED notification which doesn't include
  // the actual URL navigated to and isn't sent for AUTO_SUBFRAME navigations.
  if (details.type != NAVIGATION_TYPE_NAV_IGNORE) {
    DCHECK(delegate_);
    DCHECK_EQ(!render_frame_host->GetParent(),
              did_navigate ? details.is_main_frame : false);
    navigation_request->DidCommitNavigation(
        params, did_navigate, details.did_replace_entry,
        details.previous_main_frame_url, details.type);

    // Dispatch PrimaryPageChanged notification when a main frame
    // non-same-document navigation changes the current Page in the FrameTree.
    //
    // We do this here to ensure that this navigation has updated all relevant
    // properties of RenderFrameHost / Page / Navigation Controller / Navigation
    // Request (e.g. `RenderFrameHost::GetLastCommittedURL`,
    // `NavigationRequest::GetHttpStatusCode`) before notifying the observers.
    // TODO(crbug.com/1275933): Don't dispatch PrimaryPageChanged for initial
    // empty document navigations.
    if (!was_within_same_document && render_frame_host->is_main_frame()) {
      navigation_request->frame_tree_node()
          ->frame_tree()
          ->delegate()
          ->NotifyPageChanged(render_frame_host->GetPage());

      // Finally reset the `navigation_request` after navigation commit and all
      // NavigationRequest usages.
      navigation_request.reset();
    }
  }

  if (did_create_new_document) {
    RecordWebPlatformSecurityMetrics(render_frame_host, has_embedding_control,
                                     is_error_page);
    RecordMetrics(*render_frame_host, web_features);
  }

  if (!did_navigate)
    return;  // No navigation happened.

  // DO NOT ADD MORE STUFF TO THIS FUNCTION! Your component should either listen
  // for the appropriate notification (best) or you can add it to
  // DidNavigateMainFramePostCommit / DidNavigateAnyFramePostCommit (only if
  // necessary, please).

  // TODO(carlosk): Move this out.
  RecordNavigationMetrics(details, params, site_instance, original_request_url);

  // Now that something has committed, we don't need to track whether the
  // initial page has been accessed.
  frame_tree->ResetHasAccessedInitialMainDocument();

  // Run post-commit tasks.
  if (details.is_main_frame)
    delegate_->DidNavigateMainFramePostCommit(render_frame_host, details);

  delegate_->DidNavigateAnyFramePostCommit(render_frame_host, details);
}

void Navigator::Navigate(std::unique_ptr<NavigationRequest> request,
                         ReloadType reload_type) {
  TRACE_EVENT0("browser,navigation", "Navigator::Navigate");
  TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(
      "navigation,rail", "NavigationTiming navigationStart",
      TRACE_EVENT_SCOPE_GLOBAL, request->common_params().navigation_start);

  FrameTreeNode* frame_tree_node = request->frame_tree_node();
  DCHECK_EQ(frame_tree_node->frame_tree(), &controller_.frame_tree());

  navigation_data_ = std::make_unique<NavigationMetricsData>(
      request->common_params().navigation_start, request->common_params().url,
      frame_tree_node->current_frame_host()->GetPageUkmSourceId(),
      true /* is_browser_initiated_before_unload */);

  // Check if the BeforeUnload event needs to execute before assigning the
  // NavigationRequest to the FrameTreeNode. Assigning it to the FrameTreeNode
  // has the side effect of initializing the current RenderFrameHost, which will
  // return that it should execute the BeforeUnload event (even though we don't
  // need to wait for it in the case of a brand new RenderFrameHost).
  //
  // We don't want to dispatch a beforeunload handler if
  // is_history_navigation_in_new_child is true. This indicates a newly created
  // child frame which does not have a beforeunload handler.
  bool should_dispatch_beforeunload =
      !NavigationTypeUtils::IsSameDocument(
          request->common_params().navigation_type) &&
      !request->common_params().is_history_navigation_in_new_child_frame &&
      frame_tree_node->current_frame_host()->ShouldDispatchBeforeUnload(
          false /* check_subframes_only */);

  int nav_entry_id = request->nav_entry_id();
  bool is_pending_entry =
      controller_.GetPendingEntry() &&
      (nav_entry_id == controller_.GetPendingEntry()->GetUniqueID());
  frame_tree_node->CreatedNavigationRequest(std::move(request));
  DCHECK(frame_tree_node->navigation_request());

  // Have the current renderer execute its beforeunload event if needed. If it
  // is not needed then NavigationRequest::BeginNavigation should be directly
  // called instead.
  if (should_dispatch_beforeunload) {
    frame_tree_node->navigation_request()->SetWaitingForRendererResponse();
    frame_tree_node->current_frame_host()->DispatchBeforeUnload(
        RenderFrameHostImpl::BeforeUnloadType::BROWSER_INITIATED_NAVIGATION,
        reload_type != ReloadType::NONE);
  } else {
    frame_tree_node->navigation_request()->BeginNavigation();
    // WARNING: The NavigationRequest might have been destroyed in
    // BeginNavigation(). Do not use |frame_tree_node->navigation_request()|
    // after this point without null checking it first.
  }

  // Make sure no code called via RFH::Navigate clears the pending entry.
  if (is_pending_entry)
    CHECK_EQ(nav_entry_id, controller_.GetPendingEntry()->GetUniqueID());
}

void Navigator::RequestOpenURL(
    RenderFrameHostImpl* render_frame_host,
    const GURL& url,
    const blink::LocalFrameToken* initiator_frame_token,
    int initiator_process_id,
    const absl::optional<url::Origin>& initiator_origin,
    const scoped_refptr<network::ResourceRequestBody>& post_body,
    const std::string& extra_headers,
    const Referrer& referrer,
    WindowOpenDisposition disposition,
    bool should_replace_current_entry,
    bool user_gesture,
    bool is_unfenced_top_navigation,
    blink::mojom::TriggeringEventInfo triggering_event_info,
    const std::string& href_translate,
    scoped_refptr<network::SharedURLLoaderFactory> blob_url_loader_factory,
    const absl::optional<blink::Impression>& impression) {
  // Note: This can be called for subframes (even when OOPIFs are not possible)
  // if the disposition calls for a different window.

  // Only the current RenderFrameHost should be sending an OpenURL request.
  // Pending RenderFrameHost should know where it is navigating and pending
  // deletion RenderFrameHost shouldn't be trying to navigate.
  if (render_frame_host !=
      render_frame_host->frame_tree_node()->current_frame_host()) {
    return;
  }

  SiteInstance* current_site_instance = render_frame_host->GetSiteInstance();

  // TODO(creis): Pass the redirect_chain into this method to support client
  // redirects.  http://crbug.com/311721.
  std::vector<GURL> redirect_chain;

  int frame_tree_node_id = FrameTreeNode::kFrameTreeNodeInvalidId;

  // Send the navigation to the current FrameTreeNode if it's destined for a
  // subframe in the current tab.  We'll assume it's for the main frame
  // (possibly of a new or different WebContents) otherwise.
  if (disposition == WindowOpenDisposition::CURRENT_TAB &&
      render_frame_host->GetParentOrOuterDocument()) {
    if (is_unfenced_top_navigation) {
      // If the flag `is_unfenced_top_navigation` is set, this is a special code
      // path for MPArch fenced frames. The target frame doesn't have a handle
      // inside the MPArch renderer process, so we need to set it here.
      // TODO(crbug.com/1315802): Refactor _unfencedTop handling.

      // Check that the IPC parameters are valid and that the navigation
      // is allowed.
      if (!ValidateUnfencedTopNavigation(render_frame_host, url,
                                         initiator_process_id, user_gesture)) {
        return;
      }

      // Change the navigation target to the outermost frame.
      // This escapes Portals but not GuestViews.
      // - We don't especially care about Portals, because the frame won't be
      //   user activated until it's no longer in a portal
      // - We don't want _unfencedTop navigations to escape a GuestView
      //   (<webview>) and affect their embedder.
      // You can represent the primary NavigationController's FrameTree root
      // with`RenderFrameHost::kNoFrameTreeNodeId`, but we will pick out the
      // exact node to be safe.
      frame_tree_node_id = render_frame_host->GetOutermostMainFrame()
                               ->frame_tree_node()
                               ->frame_tree_node_id();

      // Fenced frames are enforced to have a history of length 1. Because the
      // renderer thinks this navigation is to the fenced frame root, it sets
      // `should_replace_current_entry` to true, but we do not want this
      // restriction for navigations outside the fenced frame.
      // TODO(crbug.com/1315802): Make sure that the browser doesn't rely on
      // whether the renderer says we should replace the current entry, i.e.
      // make sure there are no situations where we should actually replace the
      // current entry but don't, due to this line.
      should_replace_current_entry = false;
    } else {
      // Otherwise, proceed normally.
      frame_tree_node_id =
          render_frame_host->frame_tree_node()->frame_tree_node_id();
    }
  }

  // Prerendering frames need to have an FTN id set, so OpenURL() can find
  // the correct frame tree for the navigation. Due to the above logic, that
  // means this function currently can't be called for prerendering main frames.
  DCHECK(render_frame_host->lifecycle_state() !=
             RenderFrameHostImpl::LifecycleStateImpl::kPrerendering ||
         frame_tree_node_id != FrameTreeNode::kFrameTreeNodeInvalidId);

  OpenURLParams params(url, referrer, frame_tree_node_id, disposition,
                       ui::PAGE_TRANSITION_LINK,
                       true /* is_renderer_initiated */);
  params.post_data = post_body;
  params.extra_headers = extra_headers;
  if (redirect_chain.size() > 0)
    params.redirect_chain = redirect_chain;
  params.should_replace_current_entry = should_replace_current_entry;
  params.user_gesture = user_gesture;
  params.triggering_event_info = triggering_event_info;
  params.initiator_origin = initiator_origin;
  params.initiator_frame_token = base::OptionalFromPtr(initiator_frame_token);
  params.initiator_process_id = initiator_process_id;

  // RequestOpenURL is used only for local frames, so we can get here only if
  // the navigation is initiated by a frame in the same SiteInstance as this
  // frame.  Note that navigations on RenderFrameProxies do not use
  // RequestOpenURL and go through NavigateFromFrameProxy instead.
  params.source_site_instance = current_site_instance;

  params.source_render_frame_id = render_frame_host->GetRoutingID();
  params.source_render_process_id = render_frame_host->GetProcess()->GetID();

  if (render_frame_host->web_ui()) {
    // Note that we hide the referrer for Web UI pages. We don't really want
    // web sites to see a referrer of "chrome://blah" (and some chrome: URLs
    // might have search terms or other stuff we don't want to send to the
    // site), so we send no referrer.
    params.referrer = Referrer();

    // Navigations in Web UI pages count as browser-initiated navigations.
    params.is_renderer_initiated = false;
  }

  params.blob_url_loader_factory = std::move(blob_url_loader_factory);
  params.href_translate = href_translate;
  params.impression = impression;

  delegate_->OpenURL(params);
}

void Navigator::NavigateFromFrameProxy(
    RenderFrameHostImpl* render_frame_host,
    const GURL& url,
    const blink::LocalFrameToken* initiator_frame_token,
    int initiator_process_id,
    const url::Origin& initiator_origin,
    SiteInstance* source_site_instance,
    const Referrer& referrer,
    ui::PageTransition page_transition,
    bool should_replace_current_entry,
    blink::NavigationDownloadPolicy download_policy,
    const std::string& method,
    scoped_refptr<network::ResourceRequestBody> post_body,
    const std::string& extra_headers,
    scoped_refptr<network::SharedURLLoaderFactory> blob_url_loader_factory,
    network::mojom::SourceLocationPtr source_location,
    bool has_user_gesture,
    const absl::optional<blink::Impression>& impression,
    base::TimeTicks navigation_start_time,
    absl::optional<bool> is_fenced_frame_opaque_url) {
  // |method != "POST"| should imply absence of |post_body|.
  if (method != "POST" && post_body) {
    NOTREACHED();
    post_body = nullptr;
  }

  // Allow the delegate to cancel the cross-process navigation.
  // TODO(crbug.com/1316388): With MPArch there may be multiple main frames and
  // so is_main_frame should not be used to identify outermost main frames.
  // Follow up to confirm correctness.
  if (!delegate_->ShouldAllowRendererInitiatedCrossProcessNavigation(
          render_frame_host->IsOutermostMainFrame()))
    return;

  // TODO(creis): Determine if this transfer started as a browser-initiated
  // navigation.  See https://crbug.com/495161.
  bool is_renderer_initiated = true;
  Referrer referrer_to_use(referrer);
  if (render_frame_host->web_ui()) {
    // Note that we hide the referrer for Web UI pages. We don't really want
    // web sites to see a referrer of "chrome://blah" (and some chrome: URLs
    // might have search terms or other stuff we don't want to send to the
    // site), so we send no referrer.
    referrer_to_use = Referrer();

    // Navigations in Web UI pages count as browser-initiated navigations.
    is_renderer_initiated = false;
  }

  if (is_renderer_initiated &&
      ShouldIgnoreIncomingRendererRequest(
          render_frame_host->frame_tree_node()->navigation_request(),
          has_user_gesture)) {
    return;
  }

  controller_.NavigateFromFrameProxy(
      render_frame_host, url, initiator_frame_token, initiator_process_id,
      initiator_origin, is_renderer_initiated, source_site_instance,
      referrer_to_use, page_transition, should_replace_current_entry,
      download_policy, method, post_body, extra_headers,
      std::move(source_location), std::move(blob_url_loader_factory),
      impression, navigation_start_time, is_fenced_frame_opaque_url);
}

void Navigator::BeforeUnloadCompleted(FrameTreeNode* frame_tree_node,
                                      bool proceed,
                                      const base::TimeTicks& proceed_time) {
  DCHECK(frame_tree_node);

  NavigationRequest* navigation_request = frame_tree_node->navigation_request();

  // The NavigationRequest may have been canceled while the renderer was
  // executing the BeforeUnload event.
  if (!navigation_request)
    return;

  // If the user chose not to proceed, cancel the ongoing navigation.
  // Note: it might be a new navigation, and not the one that triggered the
  // sending of the BeforeUnload IPC in the first place. However, the
  // BeforeUnload where the user asked not to proceed will have taken place
  // after the navigation started. The last user input should be respected, and
  // the navigation cancelled anyway.
  if (!proceed) {
    CancelNavigation(frame_tree_node);
    return;
  }

  // The browser-initiated NavigationRequest that triggered the sending of the
  // BeforeUnload IPC might have been replaced by a renderer-initiated one while
  // the BeforeUnload event executed in the renderer. In that case, the request
  // will already have begun, so there is no need to start it again.
  if (navigation_request->state() >
      NavigationRequest::WAITING_FOR_RENDERER_RESPONSE) {
    DCHECK(navigation_request->from_begin_navigation());
    return;
  }

  // Update the navigation start: it should be when it was determined that the
  // navigation will proceed.
  navigation_request->set_navigation_start_time(proceed_time);

  DCHECK_EQ(NavigationRequest::WAITING_FOR_RENDERER_RESPONSE,
            navigation_request->state());

  // Send the request to the IO thread.
  navigation_request->BeginNavigation();
  // DO NOT USE |navigation_request| BEYOND THIS POINT. It might have been
  // destroyed in BeginNavigation().
  // See https://crbug.com/770157.
}

void Navigator::OnBeginNavigation(
    FrameTreeNode* frame_tree_node,
    blink::mojom::CommonNavigationParamsPtr common_params,
    blink::mojom::BeginNavigationParamsPtr begin_params,
    scoped_refptr<network::SharedURLLoaderFactory> blob_url_loader_factory,
    mojo::PendingAssociatedRemote<mojom::NavigationClient> navigation_client,
    scoped_refptr<PrefetchedSignedExchangeCache>
        prefetched_signed_exchange_cache,
    std::unique_ptr<WebBundleHandleTracker> web_bundle_handle_tracker) {
  TRACE_EVENT0("navigation", "Navigator::OnBeginNavigation");
  // TODO(clamy): the url sent by the renderer should be validated with
  // FilterURL.
  // This is a renderer-initiated navigation.
  DCHECK(frame_tree_node);

  if (common_params->is_history_navigation_in_new_child_frame) {
    // Try to find a FrameNavigationEntry that matches this frame instead, based
    // on the frame's unique name.  If this can't be found, fall back to the
    // default path below.
    if (frame_tree_node->navigator().StartHistoryNavigationInNewSubframe(
            frame_tree_node->current_frame_host(), &navigation_client)) {
      return;
    }
  }

  NavigationRequest* ongoing_navigation_request =
      frame_tree_node->navigation_request();

  // Client redirects during the initial history navigation of a child frame
  // should take precedence over the history navigation (despite being renderer-
  // initiated).  See https://crbug.com/348447 and https://crbug.com/691168.
  if (ongoing_navigation_request &&
      ongoing_navigation_request->common_params()
          .is_history_navigation_in_new_child_frame) {
    // Preemptively clear this local pointer before deleting the request.
    ongoing_navigation_request = nullptr;
    frame_tree_node->ResetNavigationRequest(false);
  }

  // Verify this navigation has precedence.
  if (ShouldIgnoreIncomingRendererRequest(ongoing_navigation_request,
                                          common_params->has_user_gesture)) {
    return;
  }

  NavigationEntryImpl* navigation_entry =
      GetNavigationEntryForRendererInitiatedNavigation(*common_params,
                                                       frame_tree_node);
  const bool override_user_agent =
      delegate_->ShouldOverrideUserAgentForRendererInitiatedNavigation();
  if (navigation_entry)
    navigation_entry->SetIsOverridingUserAgent(override_user_agent);

  frame_tree_node->CreatedNavigationRequest(
      NavigationRequest::CreateRendererInitiated(
          frame_tree_node, navigation_entry, std::move(common_params),
          std::move(begin_params), controller_.GetLastCommittedEntryIndex(),
          controller_.GetEntryCount(), override_user_agent,
          std::move(blob_url_loader_factory), std::move(navigation_client),
          std::move(prefetched_signed_exchange_cache),
          std::move(web_bundle_handle_tracker)));
  NavigationRequest* navigation_request = frame_tree_node->navigation_request();

  navigation_data_ = std::make_unique<NavigationMetricsData>(
      navigation_request->common_params().navigation_start,
      navigation_request->common_params().url,
      frame_tree_node->current_frame_host()->GetPageUkmSourceId(),
      false /* is_browser_initiated_before_unload */);

  LogRendererInitiatedBeforeUnloadTime(
      navigation_request->begin_params().before_unload_start,
      navigation_request->begin_params().before_unload_end);

  // This frame has already run beforeunload before it sent this IPC.  See if
  // any of its cross-process subframes also need to run beforeunload.  If so,
  // delay the navigation until beforeunload completion callbacks are invoked on
  // those frames.
  DCHECK(!NavigationTypeUtils::IsSameDocument(
      navigation_request->common_params().navigation_type));
  bool should_dispatch_beforeunload =
      frame_tree_node->current_frame_host()->ShouldDispatchBeforeUnload(
          true /* check_subframes_only */);
  if (should_dispatch_beforeunload) {
    frame_tree_node->navigation_request()->SetWaitingForRendererResponse();
    frame_tree_node->current_frame_host()->DispatchBeforeUnload(
        RenderFrameHostImpl::BeforeUnloadType::RENDERER_INITIATED_NAVIGATION,
        NavigationTypeUtils::IsReload(
            navigation_request->common_params().navigation_type));
    return;
  }

  // For main frames, NavigationHandle will be created after the call to
  // |DidStartMainFrameNavigation|, so it receives the most up to date pending
  // entry from the NavigationController.
  navigation_request->BeginNavigation();
  // DO NOT USE |navigation_request| BEYOND THIS POINT. It might have been
  // destroyed in BeginNavigation().
  // See https://crbug.com/770157.
}

void Navigator::RestartNavigationAsCrossDocument(
    std::unique_ptr<NavigationRequest> navigation_request) {
  FrameTreeNode* frame_tree_node = navigation_request->frame_tree_node();
  // Don't restart the navigation if there is already another ongoing navigation
  // in the FrameTreeNode.
  if (frame_tree_node->navigation_request())
    return;

  navigation_request->ResetForCrossDocumentRestart();
  frame_tree_node->CreatedNavigationRequest(std::move(navigation_request));
  frame_tree_node->navigation_request()->BeginNavigation();
  // DO NOT USE THE NAVIGATION REQUEST BEYOND THIS POINT. It might have been
  // destroyed in BeginNavigation().
  // See https://crbug.com/770157.
}

void Navigator::CancelNavigation(FrameTreeNode* frame_tree_node) {
  if (frame_tree_node->navigation_request())
    frame_tree_node->navigation_request()->set_net_error(net::ERR_ABORTED);
  frame_tree_node->ResetNavigationRequest(false);
  if (frame_tree_node->IsMainFrame())
    navigation_data_.reset();
}

void Navigator::LogCommitNavigationSent() {
  if (!navigation_data_)
    return;

  navigation_data_->commit_navigation_sent_ = base::TimeTicks::Now();
}

void Navigator::LogBeforeUnloadTime(
    base::TimeTicks renderer_before_unload_start_time,
    base::TimeTicks renderer_before_unload_end_time,
    base::TimeTicks before_unload_sent_time,
    bool for_legacy) {
  if (!navigation_data_)
    return;

  // LogBeforeUnloadTime is called once for each cross-process frame. Once all
  // beforeunloads complete, the timestamps in navigation_data will be the
  // timestamps of the beforeunload that blocked the navigation the longest.
  // `for_legacy` indicates this is being called as the result of a PostTask(),
  // which did not go to the renderer so that the times do not need to be
  // adjusted.
  if (!base::TimeTicks::IsConsistentAcrossProcesses() && !for_legacy) {
    // These timestamps come directly from the renderer so they might need to be
    // converted to local time stamps.
    blink::InterProcessTimeTicksConverter converter(
        blink::LocalTimeTicks::FromTimeTicks(before_unload_sent_time),
        blink::LocalTimeTicks::FromTimeTicks(base::TimeTicks::Now()),
        blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_start_time),
        blink::RemoteTimeTicks::FromTimeTicks(renderer_before_unload_end_time));
    blink::LocalTimeTicks converted_renderer_before_unload_start =
        converter.ToLocalTimeTicks(blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_start_time));
    blink::LocalTimeTicks converted_renderer_before_unload_end =
        converter.ToLocalTimeTicks(blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_end_time));
    navigation_data_->before_unload_start_ =
        converted_renderer_before_unload_start.ToTimeTicks();
    navigation_data_->before_unload_end_ =
        converted_renderer_before_unload_end.ToTimeTicks();
  } else {
    navigation_data_->before_unload_start_ = renderer_before_unload_start_time;
    navigation_data_->before_unload_end_ = renderer_before_unload_end_time;
  }
  navigation_data_->before_unload_sent_ = before_unload_sent_time;
}

void Navigator::LogRendererInitiatedBeforeUnloadTime(
    base::TimeTicks renderer_before_unload_start_time,
    base::TimeTicks renderer_before_unload_end_time) {
  DCHECK(navigation_data_);

  if (renderer_before_unload_start_time == base::TimeTicks() ||
      renderer_before_unload_end_time == base::TimeTicks())
    return;

  if (!base::TimeTicks::IsConsistentAcrossProcesses()) {
    // These timestamps come directly from the renderer so they might need to be
    // converted to local time stamps.
    blink::InterProcessTimeTicksConverter converter(
        blink::LocalTimeTicks::FromTimeTicks(base::TimeTicks()),
        blink::LocalTimeTicks::FromTimeTicks(base::TimeTicks::Now()),
        blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_start_time),
        blink::RemoteTimeTicks::FromTimeTicks(renderer_before_unload_end_time));
    blink::LocalTimeTicks converted_renderer_before_unload_start =
        converter.ToLocalTimeTicks(blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_start_time));
    blink::LocalTimeTicks converted_renderer_before_unload_end =
        converter.ToLocalTimeTicks(blink::RemoteTimeTicks::FromTimeTicks(
            renderer_before_unload_end_time));
    navigation_data_->renderer_before_unload_start_ =
        converted_renderer_before_unload_start.ToTimeTicks();
    navigation_data_->renderer_before_unload_end_ =
        converted_renderer_before_unload_end.ToTimeTicks();
  } else {
    navigation_data_->renderer_before_unload_start_ =
        renderer_before_unload_start_time;
    navigation_data_->renderer_before_unload_end_ =
        renderer_before_unload_end_time;
  }
}

void Navigator::RecordNavigationMetrics(
    const LoadCommittedDetails& details,
    const mojom::DidCommitProvisionalLoadParams& params,
    SiteInstance* site_instance,
    const GURL& original_request_url) {
  DCHECK(site_instance->HasProcess());

  if (!details.is_main_frame || !navigation_data_ ||
      navigation_data_->url_ != original_request_url) {
    return;
  }

  ukm::builders::Unload builder(navigation_data_->ukm_source_id_);
  base::TimeTicks first_before_unload_start_time;

  if (navigation_data_->is_browser_initiated_before_unload_) {
    if (navigation_data_->before_unload_start_ &&
        navigation_data_->before_unload_end_) {
      first_before_unload_start_time =
          navigation_data_->before_unload_start_.value();
      builder.SetBeforeUnloadDuration(
          (navigation_data_->before_unload_end_.value() -
           navigation_data_->before_unload_start_.value())
              .InMilliseconds());
    }
  } else {
    if (navigation_data_->renderer_before_unload_start_ &&
        navigation_data_->renderer_before_unload_end_) {
      first_before_unload_start_time =
          navigation_data_->renderer_before_unload_start_.value();
      base::TimeDelta before_unload_duration =
          navigation_data_->renderer_before_unload_end_.value() -
          navigation_data_->renderer_before_unload_start_.value();

      // If we had to dispatch beforeunload handlers for OOPIFs from the
      // browser, add those into the beforeunload duration as they contributed
      // to the total beforeunload latency.
      if (navigation_data_->before_unload_sent_) {
        before_unload_duration +=
            navigation_data_->before_unload_end_.value() -
            navigation_data_->before_unload_start_.value();
      }
      builder.SetBeforeUnloadDuration(before_unload_duration.InMilliseconds());
    }
  }

  // Records the queuing duration of the beforeunload sent from the browser to
  // the frame that blocked the navigation the longest. This can happen in a
  // renderer or browser initiated navigation and could mean a long queuing time
  // blocked the navigation or a long beforeunload. Records nothing if none were
  // sent.
  if (navigation_data_->before_unload_sent_) {
    builder.SetBeforeUnloadQueueingDuration(
        (navigation_data_->before_unload_start_.value() -
         navigation_data_->before_unload_sent_.value())
            .InMilliseconds());
  }

  // If this is a same-process navigation and we have timestamps for unload
  // durations, fill those metrics out as well.
  if (params.unload_start && params.unload_end &&
      params.commit_navigation_end &&
      navigation_data_->commit_navigation_sent_) {
    base::TimeTicks unload_start = params.unload_start.value();
    base::TimeTicks unload_end = params.unload_end.value();
    base::TimeTicks commit_navigation_end =
        params.commit_navigation_end.value();

    if (!base::TimeTicks::IsConsistentAcrossProcesses()) {
      // These timestamps come directly from the renderer so they might need
      // to be converted to local time stamps.
      blink::InterProcessTimeTicksConverter converter(
          blink::LocalTimeTicks::FromTimeTicks(first_before_unload_start_time),
          blink::LocalTimeTicks::FromTimeTicks(base::TimeTicks::Now()),
          blink::RemoteTimeTicks::FromTimeTicks(unload_start),
          blink::RemoteTimeTicks::FromTimeTicks(unload_end));
      blink::LocalTimeTicks converted_unload_start = converter.ToLocalTimeTicks(
          blink::RemoteTimeTicks::FromTimeTicks(unload_start));
      blink::LocalTimeTicks converted_unload_end = converter.ToLocalTimeTicks(
          blink::RemoteTimeTicks::FromTimeTicks(unload_end));
      blink::LocalTimeTicks converted_commit_navigation_end =
          converter.ToLocalTimeTicks(
              blink::RemoteTimeTicks::FromTimeTicks(commit_navigation_end));
      unload_start = converted_unload_start.ToTimeTicks();
      unload_end = converted_unload_end.ToTimeTicks();
      commit_navigation_end = converted_commit_navigation_end.ToTimeTicks();
    }
    builder.SetUnloadDuration((unload_end - unload_start).InMilliseconds());
    builder.SetUnloadQueueingDuration(
        (unload_start - navigation_data_->commit_navigation_sent_.value())
            .InMilliseconds());
    if (navigation_data_->commit_navigation_sent_) {
      builder.SetBeforeUnloadToCommit_SameProcess(
          (commit_navigation_end - first_before_unload_start_time)
              .InMilliseconds());
    }
  } else if (navigation_data_->commit_navigation_sent_) {
    // The navigation is cross-process and we don't have unload timings as they
    // are run in the old process and don't block the navigation.
    builder.SetBeforeUnloadToCommit_CrossProcess(
        (navigation_data_->commit_navigation_sent_.value() -
         first_before_unload_start_time)
            .InMilliseconds());
  }

  builder.Record(ukm::UkmRecorder::Get());
  navigation_data_.reset();
}

NavigationEntryImpl*
Navigator::GetNavigationEntryForRendererInitiatedNavigation(
    const blink::mojom::CommonNavigationParams& common_params,
    FrameTreeNode* frame_tree_node) {
  if (!frame_tree_node->IsMainFrame())
    return nullptr;

  // If there is no browser-initiated pending entry for this navigation and it
  // is not for the error URL, create a pending entry and ensure the address bar
  // updates accordingly.  We don't know the referrer or extra headers at this
  // point, but the referrer will be set properly upon commit.  This does not
  // set the SiteInstance for the pending entry, because it may change
  // before the URL commits.
  NavigationEntryImpl* pending_entry = controller_.GetPendingEntry();
  bool has_browser_initiated_pending_entry =
      pending_entry && !pending_entry->is_renderer_initiated();
  if (has_browser_initiated_pending_entry)
    return nullptr;

  // A pending navigation entry is created in OnBeginNavigation(). The renderer
  // sends a provisional load notification after that. We don't want to create
  // a duplicate navigation entry here.
  bool renderer_provisional_load_to_pending_url =
      pending_entry && pending_entry->is_renderer_initiated() &&
      (pending_entry->GetURL() == common_params.url);
  if (renderer_provisional_load_to_pending_url)
    return nullptr;

  // Since GetNavigationEntryForRendererInitiatedNavigation is called from
  // OnBeginNavigation, we can assume that no frame proxies are involved and
  // therefore that |current_site_instance| is also the |source_site_instance|.
  SiteInstance* current_site_instance =
      frame_tree_node->current_frame_host()->GetSiteInstance();
  SiteInstance* source_site_instance = current_site_instance;

  std::unique_ptr<NavigationEntryImpl> entry =
      NavigationEntryImpl::FromNavigationEntry(
          NavigationControllerImpl::CreateNavigationEntry(
              common_params.url, content::Referrer(),
              common_params.initiator_origin, source_site_instance,
              ui::PAGE_TRANSITION_LINK, true /* is_renderer_initiated */,
              std::string() /* extra_headers */,
              controller_.GetBrowserContext(),
              nullptr /* blob_url_loader_factory */));

  entry->set_reload_type(NavigationRequest::NavigationTypeToReloadType(
      common_params.navigation_type));

  controller_.SetPendingEntry(std::move(entry));
  delegate_->NotifyChangedNavigationState(content::INVALIDATE_TYPE_URL);

  return controller_.GetPendingEntry();
}

}  // namespace content
