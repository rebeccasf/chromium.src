// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/render_frame_host_manager.h"

#include <stddef.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/command_line.h"
#include "base/containers/adapters.h"
#include "base/containers/contains.h"
#include "base/debug/crash_logging.h"
#include "base/debug/dump_without_crashing.h"
#include "base/memory/ptr_util.h"
#include "base/notreached.h"
#include "base/trace_event/base_tracing.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/typed_macros.h"
#include "build/build_config.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/net/cross_origin_opener_policy_reporter.h"
#include "content/browser/process_lock.h"
#include "content/browser/renderer_host/agent_scheduling_group_host.h"
#include "content/browser/renderer_host/debug_urls.h"
#include "content/browser/renderer_host/frame_navigation_entry.h"
#include "content/browser/renderer_host/frame_tree.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/navigation_controller_impl.h"
#include "content/browser/renderer_host/navigation_entry_impl.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/navigator.h"
#include "content/browser/renderer_host/render_frame_host_delegate.h"
#include "content/browser/renderer_host/render_frame_host_factory.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/renderer_host/render_frame_proxy_host.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/renderer_host/render_view_host_factory.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/browser/renderer_host/render_widget_host_view_child_frame.h"
#include "content/browser/site_info.h"
#include "content/browser/site_instance_impl.h"
#include "content/browser/webui/web_ui_controller_factory_registry.h"
#include "content/common/content_navigation_policy.h"
#include "content/common/navigation_params_utils.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/disallow_activation_reason.h"
#include "content/public/browser/render_process_host_observer.h"
#include "content/public/browser/render_widget_host_iterator.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/site_isolation_policy.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/common/url_utils.h"
#include "services/network/public/cpp/features.h"
#include "third_party/blink/public/common/chrome_debug_urls.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/frame/event_page_show_persisted.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/frame/frame_owner_properties.mojom.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom.h"
#include "third_party/blink/public/mojom/frame/user_activation_update_types.mojom.h"
#include "third_party/blink/public/mojom/security_context/insecure_request_policy.mojom.h"

#if BUILDFLAG(IS_MAC)
#include "ui/gfx/mac/scoped_cocoa_disable_screen_updates.h"
#endif  // BUILDFLAG(IS_MAC)

namespace nw {
typedef bool(*GuestSwapProcessHookFn)(content::BrowserContext*, const GURL& url);
CONTENT_EXPORT GuestSwapProcessHookFn gGuestSwapProcessHook = nullptr;
}

namespace content {

using LifecycleStateImpl = RenderFrameHostImpl::LifecycleStateImpl;
using perfetto::protos::pbzero::ChromeTrackEvent;

namespace {

bool IsDataOrAbout(const GURL& url) {
  return url.IsAboutSrcdoc() || url.IsAboutBlank() ||
         url.scheme() == url::kDataScheme;
}

// Helper function to determine whether a navigation from `current_rfh` to
// `destination_effective_url_info` should swap BrowsingInstances to ensure that
// `destination_effective_url_info` ends up in a dedicated process.  This is the
// case when `destination_effective_url` has an origin that was just isolated
// dynamically, where leaving the navigation in the current BrowsingInstance
// would leave `destination_effective_url_info` without a dedicated process,
// since dynamic origin isolation applies only to future BrowsingInstances.  In
// the common case where `current_rfh` is a main frame, and there are no
// scripting references to it from other windows, it is safe to swap
// BrowsingInstances to ensure the new isolated origin takes effect.  Note that
// this applies even to same-site navigations, as well as to renderer-initiated
// navigations.
bool ShouldSwapBrowsingInstancesForDynamicIsolation(
    RenderFrameHostImpl* current_rfh,
    const UrlInfo& destination_effective_url_info) {
  // Only main frames are eligible to swap BrowsingInstances.
  if (!current_rfh->is_main_frame())
    return false;

  // Skip cases when there are other windows that might script this one.
  SiteInstanceImpl* current_instance = current_rfh->GetSiteInstance();
  if (current_instance->GetRelatedActiveContentsCount() > 1u)
    return false;

  // Check whether `destination_effective_url_info` would require a dedicated
  // process if we left it in the current BrowsingInstance.  If so, there's no
  // need to swap BrowsingInstances.
  auto& current_isolation_context = current_instance->GetIsolationContext();
  auto current_site_info = SiteInfo::Create(current_isolation_context,
                                            destination_effective_url_info);
  if (current_site_info.RequiresDedicatedProcess(current_isolation_context))
    return false;

  // Finally, check whether `destination_effective_url_info` would require a
  // dedicated process if we were to swap to a fresh BrowsingInstance.  To check
  // this, use a new IsolationContext, rather than
  // current_instance->GetIsolationContext().
  IsolationContext future_isolation_context(
      current_instance->GetBrowserContext());
  auto future_site_info = SiteInfo::Create(future_isolation_context,
                                           destination_effective_url_info);
  return future_site_info.RequiresDedicatedProcess(future_isolation_context);
}

// Helper function to determine whether |dest_url_info| should be loaded in the
// same StoragePartition that |current_instance| is currently using.
bool DoesNavigationChangeStoragePartition(SiteInstanceImpl* current_instance,
                                          const UrlInfo& dest_url_info) {
  // Derive a new SiteInfo from |current_instance|, but don't treat the
  // navigation as related to avoid StoragePartition propagation logic.
  StoragePartitionConfig dest_partition_config =
      current_instance->DeriveSiteInfo(dest_url_info, /*is_related=*/false)
          .storage_partition_config();
  StoragePartitionConfig current_partition_config =
      current_instance->GetSiteInfo().storage_partition_config();
  return current_partition_config != dest_partition_config;
}

bool IsSiteInstanceCompatibleWithErrorIsolation(
    SiteInstance* site_instance,
    const FrameTreeNode& frame_tree_node,
    bool is_failure) {
  // With no error isolation all SiteInstances are compatible with any
  // |is_failure|.
  if (!frame_tree_node.IsErrorPageIsolationEnabled())
    return true;

  // When error page isolation is enabled, don't reuse |site_instance| if it's
  // an error page SiteInstance, but the navigation is not a failure.
  // Similarly, don't reuse |site_instance| if it's not an error page
  // SiteInstance but the navigation will fail and actually need an error page
  // SiteInstance.
  bool is_site_instance_for_failures =
      static_cast<SiteInstanceImpl*>(site_instance)
          ->GetSiteInfo()
          .is_error_page();
  return is_site_instance_for_failures == is_failure;
}

// Simple wrapper around WebExposedIsolationInfo::AreCompatible for easier use
// within the process model.
bool IsSiteInstanceCompatibleWithWebExposedIsolation(
    SiteInstance* site_instance,
    const absl::optional<WebExposedIsolationInfo>& web_exposed_isolation_info) {
  SiteInstanceImpl* site_instance_impl =
      static_cast<SiteInstanceImpl*>(site_instance);

  return WebExposedIsolationInfo::AreCompatible(
      site_instance_impl->GetWebExposedIsolationInfo(),
      web_exposed_isolation_info);
}

// Helper for appending more information to the optional |reason| parameter
// that some of the RenderFrameHostManager's methods expose for debugging /
// diagnostic purposes.
void AppendReason(std::string* reason, const char* value) {
  if (!reason)
    return;

  if (!reason->empty())
    reason->append("; ");
  reason->append(value);

  DCHECK_LT(reason->size(),
            static_cast<size_t>(base::debug::CrashKeySize::Size256));
}

perfetto::protos::pbzero::ShouldSwapBrowsingInstance
ShouldSwapBrowsingInstanceToProto(ShouldSwapBrowsingInstance result) {
  using ProtoLevel = perfetto::protos::pbzero::ShouldSwapBrowsingInstance;
  switch (result) {
    case ShouldSwapBrowsingInstance::kYes_ForceSwap:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_YES_FORCE_SWAP;
    case ShouldSwapBrowsingInstance::kYes_CrossSiteProactiveSwap:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_YES_CROSS_SITE_PROACTIVE_SWAP;
    case ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_YES_SAME_SITE_PROACTIVE_SWAP;
    case ShouldSwapBrowsingInstance::kNo_ProactiveSwapDisabled:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_PROACTIVE_SWAP_DISABLED;
    case ShouldSwapBrowsingInstance::kNo_NotMainFrame:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_NOT_MAIN_FRAME;
    case ShouldSwapBrowsingInstance::kNo_HasRelatedActiveContents:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_HAS_RELATED_ACTIVE_CONTENTS;
    case ShouldSwapBrowsingInstance::kNo_DoesNotHaveSite:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_DOES_NOT_HAVE_SITE;
    case ShouldSwapBrowsingInstance::kNo_SourceURLSchemeIsNotHTTPOrHTTPS:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_SOURCE_URL_SCHEME_NOT_HTTP_OR_HTTPS;
    case ShouldSwapBrowsingInstance::kNo_SameSiteNavigation:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_SAME_SITE_NAVIGATION;
    case ShouldSwapBrowsingInstance::kNo_AlreadyHasMatchingBrowsingInstance:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_ALREADY_HAS_MATCHING_BROWSING_INSTANCE;
    case ShouldSwapBrowsingInstance::kNo_RendererDebugURL:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_RENDERER_DEBUG_URL;
    case ShouldSwapBrowsingInstance::kNo_NotNeededForBackForwardCache:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_NOT_NEEDED_FOR_BACK_FORWARD_CACHE;
    case ShouldSwapBrowsingInstance::kNo_SameDocumentNavigation:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_SAME_DOCUMENT_NAVIGATION;
    case ShouldSwapBrowsingInstance::kNo_SameUrlNavigation:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_SAME_URL_NAVIGATION;
    case ShouldSwapBrowsingInstance::kNo_WillReplaceEntry:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_WILL_REPLACE_ENTRY;
    case ShouldSwapBrowsingInstance::kNo_Reload:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_RELOAD;
    case ShouldSwapBrowsingInstance::kNo_Guest:
      return ProtoLevel::SHOULD_SWAP_BROWSING_INSTANCE_NO_GUEST;
    case ShouldSwapBrowsingInstance::kNo_HasNotComittedAnyNavigation:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_HAS_NOT_COMMITTED_ANY_NAVIGATION;
    case ShouldSwapBrowsingInstance::
        kNo_UnloadHandlerExistsOnSameSiteNavigation:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_UNLOAD_HANDLER_EXISTS_ON_SAME_SITE_NAVIGATION;
    case ShouldSwapBrowsingInstance::kNo_NotPrimaryMainFrame:
      return ProtoLevel::
          SHOULD_SWAP_BROWSING_INSTANCE_NO_NOT_PRIMARY_MAIN_FRAME;
  }
}

void TraceShouldSwapBrowsingInstanceResult(int frame_tree_node_id,
                                           ShouldSwapBrowsingInstance result) {
  TRACE_EVENT_INSTANT(
      "navigation",
      "RenderFrameHostManager::GetSiteInstanceForNavigation_ShouldSwapResult",
      [&](perfetto::EventContext ctx) {
        auto* event = ctx.event<ChromeTrackEvent>();
        auto* data = event->set_should_swap_browsing_instances_result();
        data->set_frame_tree_node_id(frame_tree_node_id);
        data->set_result(ShouldSwapBrowsingInstanceToProto(result));
      });
}

}  // namespace

RenderFrameHostManager::RenderFrameHostManager(FrameTreeNode* frame_tree_node,
                                               Delegate* delegate)
    : frame_tree_node_(frame_tree_node), delegate_(delegate) {
  DCHECK(frame_tree_node_);
}

RenderFrameHostManager::~RenderFrameHostManager() {
  DCHECK(!speculative_render_frame_host_);
  // If the current RenderFrameHost doesn't exist, then there is no need to
  // destroy proxies, as they are only accessible via RenderFrameHost. This
  // only occurs in MPArch activation, as frame trees are destroyed even when
  // the root has no associated RenderFrameHost, specifically when
  // RenderFrameHost has been moved during activation and the source
  // FrameTreeNode is being destroyed.
  if (!render_frame_host_) {
    return;
  }

  // Delete any RenderFrameProxyHosts. It is important to delete those prior to
  // deleting the current RenderFrameHost, since the CrossProcessFrameConnector
  // (owned by RenderFrameProxyHost) points to the RenderWidgetHostView
  // associated with the current RenderFrameHost and uses it during its
  // destructor.
  render_frame_host_->browsing_context_state()->ResetProxyHosts();

  SetRenderFrameHost(nullptr);
}

void RenderFrameHostManager::InitRoot(
    SiteInstance* site_instance,
    bool renderer_initiated_creation,
    blink::FramePolicy initial_main_frame_policy,
    const std::string& name) {
  scoped_refptr<BrowsingContextState> browsing_context_state =
      base::MakeRefCounted<BrowsingContextState>(
          blink::mojom::FrameReplicationState::New(
              url::Origin(), name, "", blink::ParsedPermissionsPolicy(),
              network::mojom::WebSandboxFlags::kNone, initial_main_frame_policy,
              // should enforce strict mixed content checking
              blink::mojom::InsecureRequestPolicy::kLeaveInsecureRequestsAlone,
              // hashes of hosts for insecure request upgrades
              std::vector<uint32_t>(),
              false /* has_potentially_trustworthy_unique_origin */,
              false /* has_active_user_gesture */,
              false /* has_received_user_gesture_before_nav */,
              false /* is_ad_subframe */),
          frame_tree_node_->parent(),
          features::GetBrowsingContextMode() ==
                  features::BrowsingContextStateImplementationType::
                      kLegacyOneToOneWithFrameTreeNode
              ? static_cast<absl::optional<BrowsingInstanceId>>(absl::nullopt)
              : site_instance->GetBrowsingInstanceId());
  browsing_context_state->CommitFramePolicy(initial_main_frame_policy);
  browsing_context_state->SetFrameName(name, "");
  SetRenderFrameHost(CreateRenderFrameHost(
      CreateFrameCase::kInitRoot, site_instance,
      /*frame_routing_id=*/MSG_ROUTING_NONE,
      mojo::PendingAssociatedRemote<mojom::Frame>(), blink::LocalFrameToken(),
      renderer_initiated_creation, browsing_context_state));

  // Creating a main RenderFrameHost also creates a new Page, so notify the
  // delegate about this.
  frame_tree_node_->frame_tree()->delegate()->NotifyPageChanged(
      render_frame_host_->GetPage());
}

void RenderFrameHostManager::InitChild(
    SiteInstance* site_instance,
    int32_t frame_routing_id,
    mojo::PendingAssociatedRemote<mojom::Frame> frame_remote,
    const blink::LocalFrameToken& frame_token,
    blink::FramePolicy frame_policy,
    std::string frame_name,
    std::string frame_unique_name) {
  scoped_refptr<BrowsingContextState> browsing_context_state =
      base::MakeRefCounted<BrowsingContextState>(
          blink::mojom::FrameReplicationState::New(
              url::Origin(), frame_name, frame_unique_name,
              blink::ParsedPermissionsPolicy(),
              network::mojom::WebSandboxFlags::kNone, frame_policy,
              // should enforce strict mixed content checking
              blink::mojom::InsecureRequestPolicy::kLeaveInsecureRequestsAlone,
              // hashes of hosts for insecure request upgrades
              std::vector<uint32_t>(),
              false /* has_potentially_trustworthy_unique_origin */,
              false /* has_active_user_gesture */,
              false /* has_received_user_gesture_before_nav */,
              false /* is_ad_subframe */),
          frame_tree_node_->parent(),
          features::GetBrowsingContextMode() ==
                  features::BrowsingContextStateImplementationType::
                      kLegacyOneToOneWithFrameTreeNode
              ? static_cast<absl::optional<BrowsingInstanceId>>(absl::nullopt)
              : site_instance->GetBrowsingInstanceId());
  browsing_context_state->CommitFramePolicy(frame_policy);
  SetRenderFrameHost(CreateRenderFrameHost(
      CreateFrameCase::kInitChild, site_instance, frame_routing_id,
      std::move(frame_remote), frame_token,
      /*renderer_initiated_creation=*/false, browsing_context_state));
}

RenderWidgetHostViewBase* RenderFrameHostManager::GetRenderWidgetHostView()
    const {
  if (render_frame_host_)
    return static_cast<RenderWidgetHostViewBase*>(
        render_frame_host_->GetView());
  return nullptr;
}

bool RenderFrameHostManager::IsMainFrameForInnerDelegate() {
  return frame_tree_node_->IsMainFrame() &&
         frame_tree_node_->frame_tree()
                 ->delegate()
                 ->GetOuterDelegateFrameTreeNodeId() !=
             FrameTreeNode::kFrameTreeNodeInvalidId;
}

FrameTreeNode* RenderFrameHostManager::GetOuterDelegateNode() {
  int outer_contents_frame_tree_node_id =
      frame_tree_node_->frame_tree()
          ->delegate()
          ->GetOuterDelegateFrameTreeNodeId();
  return FrameTreeNode::GloballyFindByID(outer_contents_frame_tree_node_id);
}

RenderFrameProxyHost* RenderFrameHostManager::GetProxyToParent() {
  if (frame_tree_node_->IsMainFrame())
    return nullptr;

  return frame_tree_node_->GetBrowsingContextStateForSubframe()
      ->GetRenderFrameProxyHost(
          frame_tree_node_->parent()->GetSiteInstance()->group());
}

RenderFrameProxyHost* RenderFrameHostManager::GetProxyToOuterDelegate() {
  // Only the main frame should be able to reach the outer WebContents.
  DCHECK(frame_tree_node_->IsMainFrame());
  FrameTreeNode* outer_contents_frame_tree_node = GetOuterDelegateNode();
  if (!outer_contents_frame_tree_node ||
      !outer_contents_frame_tree_node->parent()) {
    return nullptr;
  }

  // We will create an outer delegate proxy in each BrowsingContextState in this
  // frame so it doesn't matter which BCS is used here.
  return render_frame_host_->browsing_context_state()->GetRenderFrameProxyHost(
      outer_contents_frame_tree_node->parent()->GetSiteInstance()->group(),
      BrowsingContextState::ProxyAccessMode::kAllowOuterDelegate);
}

RenderFrameProxyHost*
RenderFrameHostManager::GetProxyToParentOrOuterDelegate() {
  return IsMainFrameForInnerDelegate() ? GetProxyToOuterDelegate()
                                       : GetProxyToParent();
}

void RenderFrameHostManager::RemoveOuterDelegateFrame() {
  // Removing the outer delegate frame will destroy the inner WebContents. This
  // should only be called on the main frame.
  DCHECK(frame_tree_node_->IsMainFrame());
  FrameTreeNode* outer_delegate_frame_tree_node = GetOuterDelegateNode();
  DCHECK(outer_delegate_frame_tree_node->parent());
  outer_delegate_frame_tree_node->frame_tree()->RemoveFrame(
      outer_delegate_frame_tree_node);
}

void RenderFrameHostManager::Stop() {
  render_frame_host_->Stop();

  // A loading speculative RenderFrameHost should also stop.
  if (speculative_render_frame_host_ &&
      speculative_render_frame_host_->is_loading()) {
    speculative_render_frame_host_->GetAssociatedLocalFrame()->StopLoading();
  }
}

void RenderFrameHostManager::SetIsLoading(bool is_loading) {
  render_frame_host_->render_view_host()->GetWidget()->SetIsLoading(is_loading);
}

void RenderFrameHostManager::BeforeUnloadCompleted(
    bool proceed,
    const base::TimeTicks& proceed_time) {
  // If beforeunload was dispatched as part of preparing this frame for
  // attaching an inner delegate, continue attaching now.
  if (is_attaching_inner_delegate()) {
    DCHECK(frame_tree_node_->parent());
    if (proceed) {
      CreateNewFrameForInnerDelegateAttachIfNecessary();
    } else {
      NotifyPrepareForInnerDelegateAttachComplete(false /* success */);
    }
    return;
  }

  bool proceed_to_fire_unload = false;
  delegate_->BeforeUnloadFiredFromRenderManager(proceed, proceed_time,
                                                &proceed_to_fire_unload);
  if (proceed_to_fire_unload) {
    // If we're about to close the tab and there's a speculative RFH, cancel it.
    // Otherwise, if the navigation in the speculative RFH completes before the
    // close in the current RFH, we'll lose the tab close.
    if (speculative_render_frame_host_)
      CleanUpNavigation();

    render_frame_host_->render_view_host()->ClosePage();
  }
}

void RenderFrameHostManager::DidNavigateFrame(
    RenderFrameHostImpl* render_frame_host,
    bool was_caused_by_user_gesture,
    bool is_same_document_navigation,
    bool clear_proxies_on_commit,
    const blink::FramePolicy& frame_policy) {
  CommitPendingIfNecessary(render_frame_host, was_caused_by_user_gesture,
                           is_same_document_navigation,
                           clear_proxies_on_commit);

  // Make sure any dynamic changes to this frame's sandbox flags and permissions
  // policy that were made prior to navigation take effect.  This should only
  // happen for cross-document navigations.
  if (!is_same_document_navigation) {
    if (!render_frame_host->browsing_context_state()->CommitFramePolicy(
            frame_policy)) {
      // The frame policy didn't change, no need to send updates to proxies.
      return;
    }

    // There should be no children of this frame; any policy changes should only
    // happen on navigation commit which will delete any child frames.
    DCHECK(!frame_tree_node_->child_count());

    if (!frame_tree_node_->parent()) {
      // Policy updates for root node happens only when the frame is a fenced
      // frame root.
      // Note: SendFramePolicyUpdatesToProxies doesn't need to be invoked for
      // MPArch fenced frames, because the root fenced frame must use a static
      // policy not to introduce a communication channel.
      CHECK(frame_tree_node_->IsFencedFrameRoot());
    } else {
      render_frame_host_->browsing_context_state()
          ->SendFramePolicyUpdatesToProxies(
              frame_tree_node_->parent()->GetSiteInstance()->group(),
              frame_policy);
    }
  }
}

void RenderFrameHostManager::CommitPendingIfNecessary(
    RenderFrameHostImpl* render_frame_host,
    bool was_caused_by_user_gesture,
    bool is_same_document_navigation,
    bool clear_proxies_on_commit) {
  if (!speculative_render_frame_host_) {
    // There's no speculative RenderFrameHost so it must be that the current
    // renderer process completed a navigation.
    // TODO(danakj): Make this a CHECK and stop handling it. Then make it a
    // DCHECK when we're sure.
    DCHECK_EQ(render_frame_host_.get(), render_frame_host);
    if (render_frame_host != render_frame_host_.get())
      return;
  }

  if (render_frame_host == speculative_render_frame_host_.get()) {
    // A cross-process navigation completed, so show the new renderer. If a
    // same-process navigation is also ongoing, it will be canceled when the
    // speculative RenderFrameHost replaces the current one in the commit call
    // below.
    CommitPending(std::move(speculative_render_frame_host_),
                  std::move(stored_page_to_restore_), clear_proxies_on_commit);
    frame_tree_node_->ResetNavigationRequest(false);
    return;
  }

  // A same-process navigation committed. A cross-process navigation may also
  // be ongoing.

  // A navigation in the original process has taken place, while a
  // cross-process navigation is ongoing.  This should cancel the ongoing
  // cross-process navigation if the commit is cross-document and has a user
  // gesture (since the user might have clicked on a new link while waiting for
  // a slow navigation), but it should not cancel it for same-document
  // navigations (which might happen as bookkeeping) or when there is no user
  // gesture (which might abusively try to prevent the user from leaving).
  // See https://crbug.com/825677 and https://crbug.com/75195 for examples.
  if (speculative_render_frame_host_ && !is_same_document_navigation &&
      was_caused_by_user_gesture) {
    frame_tree_node_->ResetNavigationRequest(false);
    CleanUpNavigation();
  }

  if (render_frame_host_->is_local_root() && render_frame_host_->GetView()) {
    // RenderFrames are created with a hidden RenderWidgetHost. When
    // navigation finishes, we show it if the delegate is shown. CommitPending()
    // takes care of this in the cross-process case, as well as other cases
    // where a RenderFrameHost is swapped in.
    if (!frame_tree_node_->frame_tree()->IsHidden())
      render_frame_host_->GetView()->Show();
  }
}

void RenderFrameHostManager::DidChangeOpener(
    const absl::optional<blink::LocalFrameToken>& opener_frame_token,
    SiteInstanceGroup* source_site_instance_group) {
  FrameTreeNode* opener = nullptr;
  if (opener_frame_token) {
    RenderFrameHostImpl* opener_rfhi = RenderFrameHostImpl::FromFrameToken(
        source_site_instance_group->process()->GetID(), *opener_frame_token);
    // If |opener_rfhi| is null, the opener RFH has already disappeared.  In
    // this case, clear the opener rather than keeping the old opener around.
    if (opener_rfhi)
      opener = opener_rfhi->frame_tree_node();
  }

  if (frame_tree_node_->opener() == opener)
    return;

  frame_tree_node_->SetOpener(opener);

  render_frame_host_->browsing_context_state()->UpdateOpener(
      source_site_instance_group);

  if (render_frame_host_->GetSiteInstance()->group() !=
      source_site_instance_group) {
    render_frame_host_->UpdateOpener();
  }

  // Notify the speculative RenderFrameHosts as well.  This is necessary in case
  // a process swap has started while the message was in flight.
  if (speculative_render_frame_host_ &&
      speculative_render_frame_host_->GetSiteInstance()->group() !=
          source_site_instance_group) {
    speculative_render_frame_host_->UpdateOpener();
  }
}

std::unique_ptr<StoredPage> RenderFrameHostManager::TakePrerenderedPage() {
  DCHECK(frame_tree_node_->IsMainFrame());
  auto main_render_frame_host = SetRenderFrameHost(nullptr);
  return CollectPage(std::move(main_render_frame_host));
}

void RenderFrameHostManager::PrepareForCollectingPage(
    RenderFrameHostImpl* main_render_frame_host,
    std::set<RenderViewHostImpl*>* render_view_hosts,
    BrowsingContextState::RenderFrameProxyHostMap* proxy_hosts) {
  TRACE_EVENT("navigation", "RenderFrameHostManager::PrepareForCollectingPage");

  // Prepare the main frame.
  (*render_view_hosts).insert(main_render_frame_host->render_view_host());
  // Prepare the proxies.
  SiteInstance* instance = main_render_frame_host->GetSiteInstance();

  for (auto& it :
       main_render_frame_host->browsing_context_state()->proxy_hosts()) {
    // This avoids including the proxy created when starting a
    // new cross-process, cross-BrowsingInstance navigation, as well as any
    // restored proxies which are also in a different BrowsingInstance.
    if (instance->IsRelatedSiteInstance(it.second->GetSiteInstance())) {
      (*render_view_hosts).insert(it.second->GetRenderViewHost());
      // When BrowsingContextState is decoupled from the FrameTreeNode and
      // RenderFrameHostManager (legacy mode is disabled), proxies and
      // replication state will be stored in a separate BrowsingContextState,
      // which won't need any updates. However, RenderViewHosts are still stored
      // in FrameTree (which, for example, is shared between the new page and
      // the page entering BFCache), so they have to be collected explicitly.
      if (features::GetBrowsingContextMode() ==
          features::BrowsingContextStateImplementationType::
              kLegacyOneToOneWithFrameTreeNode) {
        (*proxy_hosts)[it.first] = std::move(it.second);
      }
    }
  }

  // Since proxies are not collected, we can return early here.
  if (features::GetBrowsingContextMode() ==
      features::BrowsingContextStateImplementationType::
          kSwapForCrossBrowsingInstanceNavigations) {
    return;
  }

  // Remove the previously extracted proxies from the
  // RenderFrameHostManager, which also removes their respective
  // SiteInstanceGroup::Observer.
  for (auto& it : *proxy_hosts) {
    main_render_frame_host->browsing_context_state()
        ->DeleteRenderFrameProxyHost(it.second->site_instance_group());
  }
}

std::unique_ptr<StoredPage> RenderFrameHostManager::CollectPage(
    std::unique_ptr<RenderFrameHostImpl> main_render_frame_host) {
  DCHECK(main_render_frame_host->is_main_frame());

  std::set<RenderViewHostImpl*> render_view_hosts;
  BrowsingContextState::RenderFrameProxyHostMap proxy_hosts;

  PrepareForCollectingPage(main_render_frame_host.get(), &render_view_hosts,
                           &proxy_hosts);

  auto stored_page = std::make_unique<StoredPage>(
      std::move(main_render_frame_host), std::move(proxy_hosts),
      std::move(render_view_hosts));
  return stored_page;
}

void RenderFrameHostManager::UnloadOldFrame(
    std::unique_ptr<RenderFrameHostImpl> old_render_frame_host) {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::UnloadOldFrame",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());

  // Now close any modal dialogs that would prevent us from unloading the frame.
  // This must be done separately from Unload(), so that the
  // ScopedPageLoadDeferrer is no longer on the stack when we send the
  // mojo::FrameNavigationControl::Unload message. Prerendering pages cannot
  // create modal dialogs and unloading a prerendering RFH should not cause
  // existing dialogs to close.
  // To prevent the cancellation be used as a channel from fenced frames to
  // the primary main frame, we won't cancel modal dialogs for fenced frame
  // navigations.
  // TODO(crbug.com/1249466): Update CancelModalDialogsForRenderManager
  // to take a RFH/RPH and only clear relevant dialogs instead of all dialogs in
  // the WebContents.
  if (current_frame_host()->GetLifecycleState() !=
          RenderFrameHost::LifecycleState::kPrerendering &&
      !current_frame_host()->IsNestedWithinFencedFrame()) {
    delegate_->CancelModalDialogsForRenderManager();
  }

  // If the old RFH is not live, just return as there is no further work to do.
  // It will be deleted and there will be no proxy created.
  if (!old_render_frame_host->IsRenderFrameLive())
    return;

  // Reset any NavigationRequest in the RenderFrameHost. An unloaded
  // RenderFrameHost should not be trying to commit a navigation.
  old_render_frame_host->ResetNavigationRequests();

  NavigationEntryImpl* last_committed_entry =
      GetNavigationController().GetLastCommittedEntry();
  BackForwardCacheMetrics* old_page_back_forward_cache_metrics =
      (!old_render_frame_host->GetParent() && last_committed_entry)
          ? last_committed_entry->back_forward_cache_metrics()
          : nullptr;

  // Record the metrics about the state of the old main frame at the moment when
  // we navigate away from it as it matters for whether the page is eligible for
  // being put into back-forward cache.
  //
  // This covers the cross-process navigation case and the same-process case is
  // handled in RenderFrameHostImpl::CommitNavigation, so the subframe state
  // can be captured before the frame navigates away.
  //
  // TODO(altimin, crbug.com/933147): Remove this logic after we are done with
  // implementing back-forward cache.
  if (old_page_back_forward_cache_metrics) {
    old_page_back_forward_cache_metrics->RecordFeatureUsage(
        old_render_frame_host.get());
  }

  // BackForwardCache:
  //
  // If the old RenderFrameHost can be stored in the BackForwardCache, return
  // early without unloading and running unload handlers, as the document may
  // be restored later.
  {
    BackForwardCacheImpl& back_forward_cache =
        GetNavigationController().GetBackForwardCache();

    BackForwardCacheCanStoreDocumentResultWithTree can_store =
        back_forward_cache.CanStorePageNow(old_render_frame_host.get());
    TRACE_EVENT("navigation", "BackForwardCache_MaybeStorePage",
                "old_render_frame_host", old_render_frame_host, "can_store",
                can_store.flattened_reasons.ToString());
    if (can_store) {
      auto stored_page = CollectPage(std::move(old_render_frame_host));
      auto entry =
          std::make_unique<BackForwardCacheImpl::Entry>(std::move(stored_page));
      // Ensures RenderViewHosts are not reused while they are in the cache.
      for (RenderViewHostImpl* rvh : entry->render_view_hosts()) {
        rvh->EnterBackForwardCache();
      }
      back_forward_cache.StoreEntry(std::move(entry));
      return;
    }

    if (old_page_back_forward_cache_metrics) {
      old_page_back_forward_cache_metrics->FinalizeNotRestoredReasons(
          can_store.flattened_reasons, std::move(can_store.tree_reasons));
    }
  }

  // Create a replacement proxy for the old RenderFrameHost when we're switching
  // SiteInstance. There should not be one yet. This is done even if there are
  // no active frames besides this one to simplify cleanup logic on the renderer
  // side. See https://crbug.com/568836 for motivation.
  RenderFrameProxyHost* proxy = nullptr;
  if (render_frame_host_->GetSiteInstance() !=
      old_render_frame_host->GetSiteInstance()) {
    proxy =
        old_render_frame_host->browsing_context_state()
            ->CreateRenderFrameProxyHost(
                old_render_frame_host->GetSiteInstance(),
                old_render_frame_host->render_view_host(), frame_tree_node_);
  }

  // |old_render_frame_host| will be deleted when its unload ACK is received,
  // or when the timer times out, or when the RFHM itself is deleted (whichever
  // comes first).
  auto insertion =
      pending_delete_hosts_.insert(std::move(old_render_frame_host));
  // Tell the old RenderFrameHost to swap out and be replaced by the proxy.
  (*insertion.first)->Unload(proxy, true);
}

void RenderFrameHostManager::DiscardUnusedFrame(
    std::unique_ptr<RenderFrameHostImpl> render_frame_host) {
  // RenderDocument: In the case of a local<->local RenderFrameHost Swap, just
  // discard the RenderFrameHost. There are no other proxies associated.
  if (render_frame_host->GetSiteInstance() ==
      render_frame_host_->GetSiteInstance()) {
    return;  // |render_frame_host| is released here.
  }

  // TODO(carlosk): this code is very similar to what can be found in
  // UnloadOldFrame and we should see that these are unified at some point.

  // If the SiteInstanceGroup for the pending RFH is being used by others,
  // ensure that the pending RenderFrameHost is replaced by a
  // RenderFrameProxyHost to allow other frames to communicate to this frame.
  SiteInstanceImpl* site_instance = render_frame_host->GetSiteInstance();
  RenderFrameProxyHost* proxy = nullptr;
  if (site_instance->HasSite() &&
      site_instance->group()->active_frame_count() > 1) {
    // A proxy already exists for the SiteInstanceGroup that |site_instance|
    // belongs to, so just reuse it. There is no need to call Unload() on the
    // |render_frame_host|, as this method is only called to discard a pending
    // or speculative RenderFrameHost, i.e. one that has never hosted an actual
    // document.
    proxy =
        render_frame_host->browsing_context_state()->GetRenderFrameProxyHost(
            site_instance->group());
    CHECK(proxy);
  }

  render_frame_host.reset();

  // If the old proxy isn't live, create the RenderFrameProxy in the renderer,
  // so that other frames can still communicate with this frame.  See
  // https://crbug.com/653746.
  if (proxy && !proxy->is_render_frame_proxy_live())
    proxy->InitRenderFrameProxy();
}

bool RenderFrameHostManager::DeleteFromPendingList(
    RenderFrameHostImpl* render_frame_host) {
  auto it = pending_delete_hosts_.find(render_frame_host);
  if (it == pending_delete_hosts_.end())
    return false;
  pending_delete_hosts_.erase(it);
  return true;
}

// Prerender navigations match a prerender after calling
// GetFrameHostForNavigation, which means we might create a speculative RFH and
// then try to replace it with the prerendered RFH during activation. We can not
// just reset this RFH in RestorePage as the RFH would be in an invalid state
// for destruction. We need to properly clean up first. Hence this method.
// TODO(https://crbug.com/1190197): We should refactor prerender matching flow
// to ensure that we do not create speculative RFHs for prerender activation.
void RenderFrameHostManager::ActivatePrerender(
    std::unique_ptr<StoredPage> stored_page) {
  DCHECK(blink::features::IsPrerender2Enabled());
  if (speculative_render_frame_host_)
    DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());

  // Reset the swap result of BrowsingInstance as prerender activation always
  // swaps BrowsingInstance.
  BackForwardCacheMetrics* back_forward_cache_metrics =
      render_frame_host_->GetBackForwardCacheMetrics();
  if (back_forward_cache_metrics)
    back_forward_cache_metrics->SetBrowsingInstanceSwapResult(absl::nullopt);

  RestorePage(std::move(stored_page));
}

void RenderFrameHostManager::RestorePage(
    std::unique_ptr<StoredPage> stored_page) {
  TRACE_EVENT("navigation", "RenderFrameHostManager::RestorePage",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  // Matched in CommitPending().
  stored_page->render_frame_host->GetProcess()->AddPendingView();

  // speculative_render_frame_host_ and stored_page_to_restore_ will be
  // consumed during CommitPendingIfNecessary.
  // TODO(ahemery): This is awkward to leave the entry in a half consumed state
  // and it would be clearer if we could not reuse speculative_render_frame_host
  // in the long run. For now, and to avoid complex edge cases, we simply reuse
  // it to preserve the understood logic in CommitPending.

  // There should be no speculative RFH at this point. With BackForwardCache, it
  // should have never been created, and with prerender activation, it should
  // have been cleared out earlier.
  DCHECK(!speculative_render_frame_host_);
  speculative_render_frame_host_ = std::move(stored_page->render_frame_host);
  // Now |stored_page| is destroyed and thus does not monitor cookie changes any
  // more. This is okay as eviction would not happen from this point.
  stored_page_to_restore_ = std::move(stored_page);
}

void RenderFrameHostManager::ClearRFHsPendingShutdown() {
  pending_delete_hosts_.clear();
}

void RenderFrameHostManager::ClearWebUIInstances() {
  current_frame_host()->ClearWebUI();
  if (speculative_render_frame_host_)
    speculative_render_frame_host_->ClearWebUI();
}

bool RenderFrameHostManager::HasPendingCommitForCrossDocumentNavigation()
    const {
  if (render_frame_host_->HasPendingCommitForCrossDocumentNavigation())
    return true;
  if (speculative_render_frame_host_) {
    return speculative_render_frame_host_
               ->HasPendingCommitForCrossDocumentNavigation();
  }
  return false;
}

void RenderFrameHostManager::DidCreateNavigationRequest(
    NavigationRequest* request) {
  TRACE_EVENT("navigation",
              "RenderFrameHostManager::DidCreateNavigationRequest",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  const bool force_use_current_render_frame_host =
      // Since the frame from the back-forward cache is being committed to the
      // SiteInstance we already have, it is treated as current.
      request->IsServedFromBackForwardCache() ||
      // Avoid calling GetFrameHostForNavigation() for same-document navigations
      // since they should always occur in the current document, which means
      // also in the current SiteInstance.
      // State may have changed in the browser that would cause us to put the
      // document in a different SiteInstance if it was loaded again now, but we
      // do not want to load the document again, see https://crbug.com/1125106.
      request->IsSameDocument();

  if (force_use_current_render_frame_host) {
    // This method should generally be calling GetFrameHostForNavigation() in
    // order to choose the correct RenderFrameHost, and choose a speculative
    // RenderFrameHost when the navigation can not be performed in the current
    // frame. Getting this wrong has security consequences as it could allow a
    // document from a different security context to be loaded in the current
    // frame and gain access to things in-process that it should not.
    // However, there are some situations where we know that we want to perform
    // the navigation in the current frame. In that case we must be sure that
    // the renderer is not *controlling* the navigation. The BeginNavigation()
    // path allows the renderer to specify all the parameters of the
    // NavigationRequest, so we should never allow it to specify that the
    // navigation be performed in the current RenderFrameHost.
    CHECK(!request->from_begin_navigation());

    // Cleanup existing pending RenderFrameHost. This corresponds to what is
    // done inside GetFrameHostForNavigation(request), but we avoid calling that
    // method for navigations which will be forced into the current document.
    CleanUpNavigation();
    request->set_associated_site_instance_type(
        NavigationRequest::AssociatedSiteInstanceType::CURRENT);
  } else {
    RenderFrameHostImpl* dest_rfh = GetFrameHostForNavigation(request);
    DCHECK(dest_rfh);
    request->set_associated_site_instance_type(
        dest_rfh == render_frame_host_.get()
            ? NavigationRequest::AssociatedSiteInstanceType::CURRENT
            : NavigationRequest::AssociatedSiteInstanceType::SPECULATIVE);
  }
}

RenderFrameHostImpl* RenderFrameHostManager::GetFrameHostForNavigation(
    NavigationRequest* request,
    std::string* reason) {
  TRACE_EVENT("navigation", "RenderFrameHostManager::GetFrameHostForNavigation",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);

  DCHECK(!request->common_params().url.SchemeIs(url::kJavaScriptScheme))
      << "Don't call this method for JavaScript URLs as those create a "
         "temporary  NavigationRequest and we don't want to reset an ongoing "
         "navigation's speculative RFH.";
  // Same-document navigations should be committed in the current document
  // (and current RenderFrameHost), so we should not come here and ask where
  // we would load that document. The resulting SiteInstance may have changed
  // since we did load the current document, but we don't want to reload it if
  // that is the case. See crbug.com/1125106.
  DCHECK(!request->IsSameDocument());
  // TODO(crbug.com/1188513): Verify that we're not resetting the document
  // sequence number in a same-document navigation. This method will reset it
  // if the site instance changed. But this method should not be called for a
  // same document history navigation. Change back to a DCHECK() once this is
  // resolved.
  if (request->IsSameDocument())
    base::debug::DumpWithoutCrashing();

  // Even though prerendering is considered an inactive state (i.e., not allowed
  // to show any UI changes) it is still allowed to navigate, fetch, load and
  // run documents in the background.
  if ((current_frame_host()->lifecycle_state() !=
       LifecycleStateImpl::kPrerendering)) {
    // Inactive frames should never be navigated. If this happens, log a
    // DumpWithoutCrashing to understand the root cause. See
    // https://crbug.com/926820 and https://crbug.com/927705.
    if (current_frame_host()->IsInactiveAndDisallowActivation(
            DisallowActivationReasonId::kNavigatingInInactiveFrame)) {
      NOTREACHED() << "Navigation in an inactive frame";
      DEBUG_ALIAS_FOR_GURL(url, request->common_params().url);
      base::debug::DumpWithoutCrashing();
    }
  }

  // Speculative RFHs are deleted immediately.
  if (speculative_render_frame_host_)
    DCHECK(!speculative_render_frame_host_->must_be_replaced());

  // The appropriate RenderFrameHost to commit the navigation.
  RenderFrameHostImpl* navigation_rfh = nullptr;

  // First compute the SiteInstance to use for the navigation.
  SiteInstance* current_site_instance = render_frame_host_->GetSiteInstance();
  BrowserContext* browser_context = current_site_instance->GetBrowserContext();
  scoped_refptr<SiteInstance> dest_site_instance =
      GetSiteInstanceForNavigationRequest(request, reason);

  // A subframe should always be in the same BrowsingInstance as the parent
  // (see also https://crbug.com/1107269).
  RenderFrameHostImpl* parent = frame_tree_node_->parent();
  DCHECK(!parent ||
         dest_site_instance->IsRelatedSiteInstance(parent->GetSiteInstance()));

  // The SiteInstance determines whether to switch RenderFrameHost or not.
  bool use_current_rfh = current_site_instance == dest_site_instance;

  bool is_same_site =
      render_frame_host_->IsNavigationSameSite(request->GetUrlInfo());
  if (frame_tree_node_->IsMainFrame()) {
    // Same-site navigations could swap BrowsingInstance as well. But we only
    // want to clear window.name on cross-site cross-BrowsingInstance main frame
    // navigations.
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#resetBCName.
    request->set_is_cross_site_cross_browsing_context_group(
        !is_same_site &&
        !dest_site_instance->IsRelatedSiteInstance(current_site_instance));
  }

  // If a crashed RenderFrameHost must not be reused, replace it by a
  // new one immediately.
  if (render_frame_host_->must_be_replaced())
    use_current_rfh = false;

  // Force using a different RenderFrameHost when RenderDocument is enabled.
  // TODO(arthursonzogni, fergal): Add support for the main frame.
  if (ShouldCreateNewHostForSameSiteSubframe() &&
      !frame_tree_node_->IsMainFrame() &&
      render_frame_host_->has_committed_any_navigation()) {
    use_current_rfh = false;
  }

  bool notify_webui_of_rf_creation = false;
  // We only do this if the policy allows it and are recovering a crashed frame.
  bool recovering_without_early_commit =
      ShouldSkipEarlyCommitPendingForCrashedFrame() &&
      render_frame_host_->must_be_replaced();
  if (use_current_rfh) {
    // GetFrameHostForNavigation will be called more than once during a
    // navigation (currently twice, on request and when it's about to commit in
    // the renderer). In the follow up calls an existing pending WebUI should
    // not be recreated if the URL didn't change. So instead of calling
    // CleanUpNavigation just discard the speculative RenderFrameHost if one
    // exists.
    if (speculative_render_frame_host_)
      DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());

    // If the navigation is to a WebUI and the current RenderFrameHost is going
    // to be used, there are only two possible ways to get here:
    // * The navigation is between two different documents belonging to the same
    //   WebUI or reloading the same document.
    // * Newly created window with a RenderFrameHost which hasn't committed a
    //   navigation yet.
    if (WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
            browser_context, request->common_params().url) &&
        request->state() < NavigationRequest::CANCELING) {
      if (render_frame_host_->has_committed_any_navigation() && render_frame_host_->web_ui()) {
        // If |render_frame_host_| has committed at least one navigation and it
        // is in a WebUI SiteInstance, then it must have the exact same WebUI
        // type if it will be reused.
        CHECK_EQ(render_frame_host_->web_ui_type(),
                 WebUIControllerFactoryRegistry::GetInstance()->GetWebUIType(
                     browser_context, request->common_params().url));
        render_frame_host_->web_ui()->RenderFrameReused(
            render_frame_host_.get());
      } else if (!render_frame_host_->web_ui()) {
        // It is possible to reuse a RenderFrameHost when going to a WebUI URL
        // and not have created a WebUI instance. An example is a WebUI main
        // frame that includes an iframe to URL that doesn't require WebUI but
        // stays in the parent frame SiteInstance (e.g. about:blank).  If that
        // frame is subsequently navigated to a URL in the same WebUI as the
        // parent frame, the RenderFrameHost will be reused and WebUI instance
        // for the child frame needs to be created.
        // During navigation, this method is called twice - at the beginning
        // and at ReadyToCommit time. The first call would have created the
        // WebUI instance and since the initial about:blank has not committed
        // a navigation, the else branch would be taken. Explicit check for
        // web_ui_ is required, otherwise we will allocate a new instance
        // unnecessarily here.
        render_frame_host_->CreateWebUI(request->common_params().url,
                                        request->bindings());
        if (render_frame_host_->IsRenderFrameLive()) {
          render_frame_host_->web_ui()->WebUIRenderFrameCreated(
              render_frame_host_.get());
        }
      }
    }

    navigation_rfh = render_frame_host_.get();

    DCHECK(!speculative_render_frame_host_);
  } else {
    // If the current RenderFrameHost cannot be used a speculative one is
    // created with the SiteInstance for the current URL. If a speculative
    // RenderFrameHost already exists we try as much as possible to reuse it and
    // its associated WebUI.

    // Check for cases that a speculative RenderFrameHost cannot be used and
    // create a new one if needed.
    if (!speculative_render_frame_host_ ||
        speculative_render_frame_host_->GetSiteInstance() !=
            dest_site_instance.get()) {
      // If a previous speculative RenderFrameHost didn't exist or if its
      // SiteInstance differs from the one for the current URL, a new one needs
      // to be created.
      CleanUpNavigation();
      bool success = CreateSpeculativeRenderFrameHost(
          current_site_instance, dest_site_instance.get(),
          recovering_without_early_commit);
      DCHECK(success);
    }
    DCHECK(speculative_render_frame_host_);

    // If the navigation is to a WebUI URL, the WebUI needs to be created to
    // allow the navigation to be served correctly.
    if (WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
            browser_context, request->common_params().url) &&
        request->state() < NavigationRequest::CANCELING) {
      bool created_web_ui = speculative_render_frame_host_->CreateWebUI(
          request->common_params().url, request->bindings());
      notify_webui_of_rf_creation =
          created_web_ui && speculative_render_frame_host_->web_ui();
    }

    navigation_rfh = speculative_render_frame_host_.get();

    if (!render_frame_host_->IsRenderFrameLive() &&
        !recovering_without_early_commit) {
      // The current RFH is not live. There's no reason to sit around with a
      // sad tab or a newly created RFH while we wait for the navigation to
      // complete. Just switch to the speculative RFH now and go back to
      // normal. (Note that we don't care about on{before}unload handlers if
      // the current RFH isn't live.)
      //
      // If the corresponding RenderFrame is currently associated with a
      // proxy, send a SwapIn message to ensure that the RenderFrame swaps
      // into the frame tree and replaces that proxy on the renderer side.
      // Normally this happens at navigation commit time, but in this case
      // this must be done earlier to keep browser and renderer state in sync.
      // This is important to do before CommitPending(), which destroys the
      // corresponding proxy. See https://crbug.com/487872.
      // TODO(https://crbug.com/1072817): Make this logic more robust to
      // consider the case for failed navigations after CommitPending.
      if (navigation_rfh->browsing_context_state()->GetRenderFrameProxyHost(
              static_cast<SiteInstanceImpl*>(dest_site_instance.get())
                  ->group())) {
        navigation_rfh->SwapIn();
      }
      navigation_rfh->OnCommittedSpeculativeBeforeNavigationCommit();

      // An Active RenderFrameHost MUST always have a PolicyContainerHost. A new
      // document is either:
      // - The initial empty document, via frame creation.
      // - A new document replacing the previous one, via a navigation.
      // Here this is an additional case: A new document (in a weird state) is
      // replacing the one crashed. In this case, it is not entirely clear what
      // PolicyContainerHost should be used. In the absence of anything better,
      // we simply keep the PolicyContainerHost that was previously active.
      // TODO(https://crbug.com/1072817): Remove this logic when removing the
      // early commit.
      navigation_rfh->SetPolicyContainerForEarlyCommitAfterCrash(
          current_frame_host()->policy_container_host()->Clone());

      CommitPending(std::move(speculative_render_frame_host_), nullptr,
                    request->coop_status().require_browsing_instance_swap());
    }
  }
  DCHECK(navigation_rfh &&
         (navigation_rfh == render_frame_host_.get() ||
          navigation_rfh == speculative_render_frame_host_.get()));
  DCHECK(!navigation_rfh->must_be_replaced());

  // If the RenderFrame that needs to navigate is not live (its process was just
  // created), initialize it. This can only happen for the initial main frame of
  // a WebContents which starts non-live but non-crashed.
  //
  // A speculative RenderFrameHost is created in the live state. A crashed
  // RenderFrameHost is replaced by a new speculative RenderFrameHost. A
  // non-speculative RenderFrameHost that is being reused is already live. This
  // leaves only a non-speculative RenderFrameHost that has never been used
  // before.
  if (!navigation_rfh->IsRenderFrameLive()) {
    DCHECK(!frame_tree_node_->parent());
    if (!ReinitializeMainRenderFrame(navigation_rfh))
      return nullptr;

    notify_webui_of_rf_creation = true;

    if (navigation_rfh == render_frame_host_.get()) {
      EnsureRenderFrameHostPageFocusConsistent();
      // TODO(nasko): This is a very ugly hack. The Chrome extensions process
      // manager still uses NotificationService and expects to see a
      // RenderViewHost changed notification after WebContents and
      // RenderFrameHostManager are completely initialized. This should be
      // removed once the process manager moves away from NotificationService.
      // See https://crbug.com/462682.
      if (frame_tree_node_->IsMainFrame()) {
        delegate_->NotifyMainFrameSwappedFromRenderManager(
            nullptr, render_frame_host_.get());
      }
    }
  }

  // If a WebUI was created in a speculative RenderFrameHost or a new
  // RenderFrame was created then the WebUI never interacted with the
  // RenderFrame. Notify using WebUIRenderFrameCreated.
  if (notify_webui_of_rf_creation && navigation_rfh->web_ui()) {
    navigation_rfh->web_ui()->WebUIRenderFrameCreated(navigation_rfh);
  }

  // If this function picked an incompatible process for the URL, capture a
  // crash dump to diagnose why it is occurring.
  // TODO(creis): Remove this check after we've gathered enough information to
  // debug issues with browser-side security checks. https://crbug.com/931895.
  auto* policy = ChildProcessSecurityPolicyImpl::GetInstance();
  const auto process_lock = navigation_rfh->GetProcess()->GetProcessLock();
  if (!process_lock.is_error_page() &&
      request->common_params().url.IsStandard() &&
      // TODO(https://crbug.com/888079): Replace `common_params().url` with
      // the origin to commit calculated on the browser side.
      !policy->CanAccessDataForOrigin(
          navigation_rfh->GetProcess()->GetID(),
          url::Origin::Create(request->common_params().url)) &&
      !request->IsForMhtmlSubframe()) {
    SCOPED_CRASH_KEY_STRING256("GetFrameHostForNav", "lock_url",
                               process_lock.ToString());
    SCOPED_CRASH_KEY_STRING64(
        "GetFrameHostForNav", "commit_origin",
        request->common_params().url.DeprecatedGetOriginAsURL().spec());
    SCOPED_CRASH_KEY_BOOL("GetFrameHostForNav", "is_main_frame",
                          frame_tree_node_->IsMainFrame());
    SCOPED_CRASH_KEY_BOOL("GetFrameHostForNav", "use_current_rfh",
                          use_current_rfh);
    NOTREACHED() << "Picked an incompatible process for URL: "
                 << process_lock.ToString() << " lock vs "
                 << request->common_params().url.DeprecatedGetOriginAsURL()
                 << ", request_is_sandboxed = "
                 << request->GetUrlInfo().is_sandboxed;
    base::debug::DumpWithoutCrashing();
  }

  return navigation_rfh;
}

void RenderFrameHostManager::MaybeCleanUpNavigation() {
  // This is called when a renderer aborts a NavigationRequest
  // that was in the READY_TO_COMMIT state. The caller has already
  // disassociated the NavigationRequest from the RenderFrameHost,
  // which may or may not have been the speculative one. Either way,
  // if there are no remaining NavigationRequests associated with
  // |speculative_render_frame_host_|, then it is safe to call
  // CleanUpNavigation() to discard |speculative_render_frame_host_|.
  if (!speculative_render_frame_host_ ||
      speculative_render_frame_host_->HasPendingCommitNavigation()) {
    return;
  }
  NavigationRequest* navigation_request =
      frame_tree_node_->navigation_request();
  if (navigation_request &&
      navigation_request->associated_site_instance_type() ==
          NavigationRequest::AssociatedSiteInstanceType::SPECULATIVE) {
    return;
  }
  CleanUpNavigation();
}

void RenderFrameHostManager::CleanUpNavigation() {
  TRACE_EVENT("navigation", "RenderFrameHostManager::CleanUpNavigation",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  if (speculative_render_frame_host_) {
    bool was_loading = speculative_render_frame_host_->is_loading();
    DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());
    // If we were navigating away from a crashed main frame then we will have
    // set the RVH's main frame routing ID to MSG_ROUTING_NONE. We need to set
    // it back to the crashed frame to avoid having a situation where it's
    // pointing to nothing even though there is no pending commit.
    if (ShouldSkipEarlyCommitPendingForCrashedFrame() &&
        frame_tree_node_->IsMainFrame() &&
        !render_frame_host_->IsRenderFrameLive()) {
      render_frame_host_->render_view_host()->SetMainFrameRoutingId(
          render_frame_host_->GetRoutingID());
    }
    if (was_loading)
      frame_tree_node_->DidStopLoading();
  }
}

std::unique_ptr<RenderFrameHostImpl>
RenderFrameHostManager::UnsetSpeculativeRenderFrameHost() {
  TRACE_EVENT("navigation",
              "RenderFrameHostManager::UnsetSpeculativeRenderFrameHost",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  speculative_render_frame_host_->GetProcess()->RemovePendingView();
  if (speculative_render_frame_host_->lifecycle_state() ==
      LifecycleStateImpl::kSpeculative) {
    speculative_render_frame_host_->DeleteRenderFrame(
        frame_tree_node_->parent()
            ? mojom::FrameDeleteIntention::kNotMainFrame
            : mojom::FrameDeleteIntention::
                  kSpeculativeMainFrameForNavigationCancelled);
  } else {
    DCHECK_EQ(speculative_render_frame_host_->lifecycle_state(),
              LifecycleStateImpl::kPendingCommit);
    // The browser process already asked the renderer to commit the navigation.
    // The renderer is guaranteed to commit the navigation and swap in the
    // provisional `RenderFrame` to replace the current `RenderFrameProxy`
    // unless the frame is detached: see `AssertNavigationCommits` in
    // `RenderFrameImpl` for more details about this enforcement.
    //
    // Instead of simply deleting the `RenderFrame`, the browser process must
    // unwind the renderer's state by sending it another IPC to "undo" the
    // commit by immediately swapping it out for a proxy again.

    // The renderer hasn't acknowledged the `CommitNavigation()` yet so the
    // `RenderFrameProxyHost` should still be alive. Reuse it.
    RenderFrameProxyHost* proxy =
        speculative_render_frame_host_->browsing_context_state()
            ->GetRenderFrameProxyHost(
                speculative_render_frame_host_->GetSiteInstance()->group());
    DCHECK(proxy);
    speculative_render_frame_host_->UndoCommitNavigation(
        *proxy, frame_tree_node_->IsLoading());
  }
  return std::move(speculative_render_frame_host_);
}

void RenderFrameHostManager::DiscardSpeculativeRenderFrameHostForShutdown() {
  TRACE_EVENT(
      "navigation",
      "RenderFrameHostManager::DiscardSpeculativeRenderFrameHostForShutdown",
      ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);

  DCHECK(speculative_render_frame_host_);

  speculative_render_frame_host_->GetProcess()->RemovePendingView();
  // No need to call `DeleteRenderFrame()`. When a RenderFrame or
  // RenderFrameProxy is detached, it also detaches any associated provisional
  // RenderFrame, whether this due to a child frame being removed from the
  // frame tree or the entire RenderView being torn down.
  //
  // When the LifecycleStateImpl is kSpeculative, there is no need to transition
  // to kReadyToBeDeleted as speculative RenderFrameHosts don't run any unload
  // handlers but gets deleted by reset directly in kSpeculative state.
  if (speculative_render_frame_host_->lifecycle_state() ==
      LifecycleStateImpl::kPendingCommit) {
    speculative_render_frame_host_->SetLifecycleState(
        LifecycleStateImpl::kReadyToBeDeleted);
  }
  // TODO(dcheng): Figure out why `RenderFrameDeleted()` doesn't seem to be
  // called on child `RenderFrameHost`s at shutdown. This is currently limited
  // to main frame-only because that is how it has worked for some time:
  // `~WebContentsImpl()` calls `FrameTree::Shutdown()` which calls
  // `RenderFrameDeleted()` for main frame RenderFrameHosts only... Since
  // `FrameTree::Shutdown()` now delegates to this method to shutdown the
  // speculative RenderFrameHost, match the previous behavior.
  if (frame_tree_node_->IsMainFrame()) {
    speculative_render_frame_host_->RenderFrameDeleted();
  }
  speculative_render_frame_host_.reset();
}

void RenderFrameHostManager::OnDidChangeCollapsedState(bool collapsed) {
  // If we are a MPArch fenced frame root then ask the outer delegate node
  // to collapse the frame. Note `IsFencedFrameRoot` returns true for
  // ShadowDOM as well so we need to check the `FrameTree::Type` as well.
  if (frame_tree_node_->IsFencedFrameRoot() &&
      frame_tree_node_->frame_tree()->type() == FrameTree::Type::kFencedFrame) {
    GetProxyToOuterDelegate()->GetAssociatedRemoteFrame()->Collapse(collapsed);
    return;
  }

  DCHECK(frame_tree_node_->parent());
  SiteInstanceImpl* parent_site_instance =
      frame_tree_node_->parent()->GetSiteInstance();

  // There will be no proxy to represent the pending or speculative RFHs in the
  // parent's SiteInstance until the navigation is committed, but the old RFH is
  // not unloaded before that happens either, so we can talk to the
  // FrameOwner in the parent via the child's current RenderFrame at any time.
  DCHECK(current_frame_host());
  if (current_frame_host()->GetSiteInstance() == parent_site_instance) {
    current_frame_host()->GetAssociatedLocalFrame()->Collapse(collapsed);
  } else {
    RenderFrameProxyHost* proxy_to_parent =
        frame_tree_node_->GetBrowsingContextStateForSubframe()
            ->GetRenderFrameProxyHost(parent_site_instance->group());
    proxy_to_parent->GetAssociatedRemoteFrame()->Collapse(collapsed);
  }
}

void RenderFrameHostManager::OnDidUpdateFrameOwnerProperties(
    const blink::mojom::FrameOwnerProperties& properties) {
  // FrameOwnerProperties exist only for frames that have a parent.
  CHECK(frame_tree_node_->parent());
  SiteInstanceImpl* parent_instance =
      frame_tree_node_->parent()->GetSiteInstance();

  auto properties_for_local_frame = properties.Clone();

  // Notify the RenderFrame if it lives in a different process from its parent.
  if (render_frame_host_->GetSiteInstance() != parent_instance) {
    render_frame_host_->GetAssociatedLocalFrame()->SetFrameOwnerProperties(
        std::move(properties_for_local_frame));
  }

  render_frame_host_->browsing_context_state()->OnDidUpdateFrameOwnerProperties(
      properties);
}

RenderFrameHostManager::SiteInstanceDescriptor::SiteInstanceDescriptor(
    UrlInfo dest_url_info,
    SiteInstanceRelation relation_to_current)
    : existing_site_instance(nullptr),
      dest_url_info(dest_url_info),
      relation(relation_to_current) {}

void RenderFrameHostManager::CancelPendingIfNecessary(
    RenderFrameHostImpl* render_frame_host) {
  if (render_frame_host == speculative_render_frame_host_.get()) {
    // TODO(nasko, clamy): This should just clean up the speculative RFH
    // without canceling the request.  See https://crbug.com/636119.
    if (frame_tree_node_->navigation_request()) {
      frame_tree_node_->navigation_request()->set_net_error(net::ERR_ABORTED);
      frame_tree_node_->ResetNavigationRequest(false);
    } else {
      // If we are far enough into the navigation that
      // TransferNavigationRequestOwnership has already been called then the
      // FrameTreeNode no longer owns the NavigationRequest and we need to clean
      // up the speculative RenderFrameHost.
      CleanUpNavigation();
    }
  }
}

void RenderFrameHostManager::UpdateUserActivationState(
    blink::mojom::UserActivationUpdateType update_type,
    blink::mojom::UserActivationNotificationType notification_type) {
  // Don't propagate user activations out of fenced frame trees.
  FrameTreeNode* root = frame_tree_node_->frame_tree()->root();
  if (root->IsFencedFrameRoot()) {
    return;
  }

  for (const auto& pair :
       render_frame_host_->browsing_context_state()->proxy_hosts()) {
    RenderFrameProxyHost* proxy = pair.second.get();
    proxy->GetAssociatedRemoteFrame()->UpdateUserActivationState(
        update_type, notification_type);
  }

  // If any frame in an inner delegate is activated, then the FrameTreeNode that
  // embeds the inner delegate in the outer delegate should be activated as well
  // (crbug.com/1013447).
  //
  // TODO(mustaq): We should add activation consumption propagation from inner
  // to outer delegates, and also all state propagation from outer to inner
  // delegates. crbug.com/1026617.
  RenderFrameProxyHost* outer_delegate_proxy =
      root->render_manager()->GetProxyToOuterDelegate();
  if (outer_delegate_proxy &&
      update_type ==
          blink::mojom::UserActivationUpdateType::kNotifyActivation) {
    outer_delegate_proxy->GetAssociatedRemoteFrame()->UpdateUserActivationState(
        update_type, notification_type);
    GetOuterDelegateNode()->UpdateUserActivationState(update_type,
                                                      notification_type);
  }
}

ShouldSwapBrowsingInstance
RenderFrameHostManager::ShouldSwapBrowsingInstancesForNavigation(
    const GURL& current_effective_url,
    bool current_is_view_source_mode,
    SiteInstanceImpl* source_instance,
    SiteInstanceImpl* current_instance,
    SiteInstance* destination_instance,
    const UrlInfo& destination_url_info,
    bool destination_is_view_source_mode,
    ui::PageTransition transition,
    bool is_failure,
    bool is_reload,
    bool is_same_document,
    bool cross_origin_opener_policy_mismatch,
    bool was_server_redirect,
    bool should_replace_current_entry) {
  const GURL& destination_url = destination_url_info.url;
  // A subframe must stay in the same BrowsingInstance as its parent.
  bool is_main_frame = frame_tree_node_->IsMainFrame();
  if (!is_main_frame)
    return ShouldSwapBrowsingInstance::kNo_NotMainFrame;

  if (is_same_document)
    return ShouldSwapBrowsingInstance::kNo_SameDocumentNavigation;

  // If new_entry already has a SiteInstance, assume it is correct.  We only
  // need to force a swap if it is in a different BrowsingInstance.
  if (destination_instance) {
    // If we are doing an history navigation/reload and end up failing, it might
    // not be suitable to host the error page in the original SiteInstance.
    // Error pages have a Cross-Origin-Opener-Policy of 'unsafe-none' and might
    // end up in a different BrowsingInstance.
    if (is_failure && cross_origin_opener_policy_mismatch)
      return ShouldSwapBrowsingInstance::kYes_ForceSwap;

    bool should_swap = !destination_instance->IsRelatedSiteInstance(
        render_frame_host_->GetSiteInstance());
    if (should_swap) {
      return ShouldSwapBrowsingInstance::kYes_ForceSwap;
    } else {
      return ShouldSwapBrowsingInstance::kNo_AlreadyHasMatchingBrowsingInstance;
    }
  }

  // Check for reasons to swap processes even if we are in a process model that
  // doesn't usually swap (e.g., process-per-tab).  Any time we return true,
  // the new URL will be rendered in a new SiteInstance AND BrowsingInstance.
  BrowserContext* browser_context =
      GetNavigationController().GetBrowserContext();
  const GURL& destination_effective_url =
      SiteInstanceImpl::GetEffectiveURL(browser_context, destination_url);
  // Don't force a new BrowsingInstance for URLs that are handled in the
  // renderer process, like javascript: or debug URLs like chrome://crash.
  if (blink::IsRendererDebugURL(destination_effective_url))
    return ShouldSwapBrowsingInstance::kNo_RendererDebugURL;

  if (cross_origin_opener_policy_mismatch)
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;

  // Transitions across BrowserContexts should always require a
  // BrowsingInstance swap. For example, this can happen if an extension in a
  // normal profile opens an incognito window with a web URL using
  // chrome.windows.create().
  //
  // TODO(alexmos): This check should've been enforced earlier in the
  // navigation, in chrome::Navigate().  Verify this, and then convert this to
  // a CHECK and remove the fallback.
  DCHECK_EQ(browser_context,
            render_frame_host_->GetSiteInstance()->GetBrowserContext());
  if (browser_context !=
      render_frame_host_->GetSiteInstance()->GetBrowserContext()) {
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;
  }

  // For security, we should transition between processes when one is a Web UI
  // page and one isn't, or if the WebUI types differ.
  if (ChildProcessSecurityPolicyImpl::GetInstance()->HasWebUIBindings(
          render_frame_host_->GetProcess()->GetID()) ||
      WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
          browser_context, current_effective_url)) {
    // If so, force a swap if destination is not an acceptable URL for Web UI.
    // Here, data URLs are never allowed.
    if (!WebUIControllerFactoryRegistry::GetInstance()->IsURLAcceptableForWebUI(
            browser_context, destination_effective_url)) {
      return ShouldSwapBrowsingInstance::kYes_ForceSwap;
    }

    // Force swap if the current WebUI type differs from the one for the
    // destination.
    if (WebUIControllerFactoryRegistry::GetInstance()->GetWebUIType(
            browser_context, current_effective_url) !=
        WebUIControllerFactoryRegistry::GetInstance()->GetWebUIType(
            browser_context, destination_effective_url)) {
      return ShouldSwapBrowsingInstance::kYes_ForceSwap;
    }
  } else {
    // Force a swap if it's a Web UI URL.
    if (WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
            browser_context, destination_effective_url)) {
      return ShouldSwapBrowsingInstance::kYes_ForceSwap;
    }
  }

  // Check with the content client as well.  Important to pass
  // current_effective_url here, which uses the SiteInstance's site if there is
  // no current_entry.
  if (GetContentClient()->browser()->ShouldSwapBrowsingInstancesForNavigation(
          render_frame_host_->GetSiteInstance(), current_effective_url,
          destination_effective_url)) {
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;
  }

  // We can't switch a RenderView between view source and non-view source mode
  // without screwing up the session history sometimes (when navigating between
  // "view-source:http://foo.com/" and "http://foo.com/", Blink doesn't treat
  // it as a new navigation). So require a BrowsingInstance switch.
  if (current_is_view_source_mode != destination_is_view_source_mode)
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;

  // If we haven't used the current SiteInstance but the destination is a
  // view-source URL, we should force a BrowsingInstance swap so that we won't
  // reuse the current SiteInstance.
  if (!current_instance->HasSite() && destination_is_view_source_mode)
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;

  // If the target URL's origin was dynamically isolated, and the isolation
  // wouldn't apply in the current BrowsingInstance, see if this navigation can
  // safely swap to a new BrowsingInstance where this isolation would take
  // effect.  This helps protect sites that have just opted into process
  // isolation, ensuring that the next navigation (e.g., a form submission
  // after user has typed in a password) can utilize a dedicated process when
  // possible (e.g., when there are no existing script references).
  if (ShouldSwapBrowsingInstancesForDynamicIsolation(
          render_frame_host_.get(),
          UrlInfo(UrlInfoInit(destination_effective_url)
                      .WithOriginIsolationRequest(
                          destination_url_info.origin_isolation_request)
                      .WithWebExposedIsolationInfo(
                          destination_url_info.web_exposed_isolation_info)))) {
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;
  }

  // If this is a cross-site navigation, we may be able to force a
  // BrowsingInstance swap to avoid unneeded process sharing. This is done for
  // certain main frame browser-initiated navigations where we can't use
  // |source_instance| and we don't need to preserve scripting
  // relationship for it (for isolated error pages).
  // See https://crbug.com/803367.
  bool is_for_isolated_error_page =
      is_failure && frame_tree_node_->IsErrorPageIsolationEnabled();

  if (current_instance->HasSite() &&
      !render_frame_host_->IsNavigationSameSite(destination_url_info) &&
      !CanUseSourceSiteInstance(destination_url_info, source_instance,
                                was_server_redirect, is_failure) &&
      !is_for_isolated_error_page &&
      IsBrowsingInstanceSwapAllowedForPageTransition(transition,
                                                     destination_url) &&
      render_frame_host_->has_committed_any_navigation()) {
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;
  }

  // If the navigation should end up in a different StoragePartition, create a
  // new BrowsingInstance, as we can only have one StoragePartition per
  // BrowsingInstance.
  //
  // Skip this check if effective URLs are involved. This ensures same-site
  // navigations to non-app sites from an isolated hosted app stay in the same
  // StoragePartition and process. This behavior is covered in
  // IsolatedAppTest.IsolatedAppProcessModel.
  if (DoesNavigationChangeStoragePartition(current_instance,
                                           destination_url_info) &&
      !current_instance
           ->IsNavigationAllowedToStayInSameProcessDueToEffectiveURLs(
               browser_context, is_main_frame, destination_url_info.url)) {
    return ShouldSwapBrowsingInstance::kYes_ForceSwap;
  }

  // Experimental mode to swap BrowsingInstances on most navigations when there
  // are no other windows in the BrowsingInstance.
  return ShouldProactivelySwapBrowsingInstance(destination_url_info, is_reload,
                                               should_replace_current_entry);
}

ShouldSwapBrowsingInstance
RenderFrameHostManager::ShouldProactivelySwapBrowsingInstance(
    const UrlInfo& destination_url_info,
    bool is_reload,
    bool should_replace_current_entry) {
  // If we've disabled proactive BrowsingInstance swap for this RenderFrameHost,
  // we should not try to do a proactive swap.
  if (render_frame_host_->HasTestDisabledProactiveBrowsingInstanceSwap())
    return ShouldSwapBrowsingInstance::kNo_ProactiveSwapDisabled;
  // We should only do proactive swap when either the flag is enabled, or if
  // it's needed for the back-forward cache (and the bfcache flag is enabled).
  if (!IsProactivelySwapBrowsingInstanceEnabled() &&
      !IsBackForwardCacheEnabled())
    return ShouldSwapBrowsingInstance::kNo_ProactiveSwapDisabled;

  // Only primary main frames are eligible to swap BrowsingInstances.
  if (frame_tree_node_->GetFrameType() != FrameType::kPrimaryMainFrame)
    return ShouldSwapBrowsingInstance::kNo_NotPrimaryMainFrame;

  // If the frame has not committed any navigation yet, we should not try to do
  // a proactive swap.
  if (!render_frame_host_->has_committed_any_navigation())
    return ShouldSwapBrowsingInstance::kNo_HasNotComittedAnyNavigation;

  // Skip cases when there are other windows that might script this one.
  SiteInstanceImpl* current_instance = render_frame_host_->GetSiteInstance();
  if (current_instance->GetRelatedActiveContentsCount() > 1u)
    return ShouldSwapBrowsingInstance::kNo_HasRelatedActiveContents;

  // "about:blank" and chrome-native-URL do not "use" a SiteInstance. This
  // allows the SiteInstance to be reused cross-site. Starting a new
  // BrowsingInstance would prevent the SiteInstance to be reused, that's why
  // this case is excluded here.
  if (!current_instance->HasSite())
    return ShouldSwapBrowsingInstance::kNo_DoesNotHaveSite;

  // Do not do a proactive BrowsingInstance swap when the previous document's
  // scheme is not HTTP/HTTPS, since only HTTP/HTTPS documents are eligible for
  // back-forward cache.
  const GURL& current_url = render_frame_host_->GetLastCommittedURL();
  if (!current_url.SchemeIsHTTPOrHTTPS())
    return ShouldSwapBrowsingInstance::kNo_SourceURLSchemeIsNotHTTPOrHTTPS;

  // WebView guests currently need to stay in the same SiteInstance and
  // BrowsingInstance.
  if (current_instance->IsGuest())
    return ShouldSwapBrowsingInstance::kNo_Guest;

  // We should check whether the new page will result in adding a new history
  // entry or not. If not, we should not do a proactive BrowsingInstance swap,
  // because these navigations are not interesting for bfcache (the old page
  // will not get into the bfcache). Cases include:
  // 1) When we know we're going to replace the history entry.
  if (should_replace_current_entry)
    return ShouldSwapBrowsingInstance::kNo_WillReplaceEntry;
  // Navigations where we will reuse the history entry:
  // 2) Different-document but same URL navigations. These navigations are
  // not classified as same-document (which got filtered earlier) so they will
  // use a different document, but they will reuse the history entry in
  // RendererDidNavigateToExistingEntry. They will usually be converted to a
  // reload (and would be handled below), but not always (e.g., POSTs to the
  // same URL use the same entry but aren't considered reloads).
  bool is_same_url = current_url.EqualsIgnoringRef(destination_url_info.url);
  if (is_same_url)
    return ShouldSwapBrowsingInstance::kNo_SameUrlNavigation;
  // 3) Reloads. Note that most reloads will not actually reach this part, as
  // ShouldSwapBrowsingInstancesForNavigation will return early if the reload
  // has a destination SiteInstance. Reloads that don't have a destination
  // SiteInstance include: doing reload after a replaceState call, reloading a
  // URL for which we've just installed a hosted app, and duplicating a tab.
  if (is_reload)
    return ShouldSwapBrowsingInstance::kNo_Reload;

  bool is_same_site =
      render_frame_host_->IsNavigationSameSite(destination_url_info);
  if (is_same_site) {
    // If it's a same-site navigation, we should only swap if same-site
    // ProactivelySwapBrowsingInstance is enabled, or if same-site
    // BackForwardCache is enabled and the current RFH is eligible for
    // back-forward cache (checked later).
    if (IsProactivelySwapBrowsingInstanceOnSameSiteNavigationEnabled()) {
      return ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap;
    }
    if (!IsSameSiteBackForwardCacheEnabled())
      return ShouldSwapBrowsingInstance::kNo_SameSiteNavigation;
    // We should not do a proactive BrowsingInstance swap on pages with unload
    // handlers if we explicitly specified to do so to avoid exposing a
    // web-observable behavior change (unload handlers running after a same-site
    // navigation). Note that we're only checking for unload handlers in frames
    // that share the same SiteInstance as the main frame, because unload
    // handlers that exist in cross-SiteInstance subframes will be dispatched
    // after we committed the navigation, regardless of our decision to swap
    // BrowsingInstances or not.
    if (ShouldSkipSameSiteBackForwardCacheForPageWithUnload() &&
        render_frame_host_->UnloadHandlerExistsInSameSiteInstanceSubtree()) {
      return ShouldSwapBrowsingInstance::
          kNo_UnloadHandlerExistsOnSameSiteNavigation;
    }
  }

  if (IsProactivelySwapBrowsingInstanceEnabled())
    return ShouldSwapBrowsingInstance::kYes_CrossSiteProactiveSwap;

  // If BackForwardCache is enabled, swap BrowsingInstances only when the
  // previous page can be stored in the back-forward cache.
  DCHECK(IsBackForwardCacheEnabled());
  NavigationControllerImpl& controller =
      render_frame_host_->frame_tree_node()->navigator().controller();

  auto can_store =
      controller.GetBackForwardCache().CanPotentiallyStorePageLater(
          render_frame_host_.get());
  if (can_store) {
    if (is_same_site) {
      return ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap;
    } else {
      return ShouldSwapBrowsingInstance::kYes_CrossSiteProactiveSwap;
    }
  } else {
    // As CanPotentiallyStorePageLater is used instead of CanStorePageNow, non-
    // sticky reasons are not recorded here. This is intentional because it is
    // impossible to get correct non-sticky reasons at this timing.
    BackForwardCacheMetrics* back_forward_cache_metrics =
        render_frame_host_->GetBackForwardCacheMetrics();
    if (back_forward_cache_metrics)
      back_forward_cache_metrics->MarkNotRestoredWithReason(can_store);
    return ShouldSwapBrowsingInstance::kNo_NotNeededForBackForwardCache;
  }
}

scoped_refptr<SiteInstance>
RenderFrameHostManager::GetSiteInstanceForNavigation(
    const UrlInfo& dest_url_info,
    SiteInstanceImpl* source_instance,
    SiteInstanceImpl* dest_instance,
    SiteInstanceImpl* candidate_instance,
    ui::PageTransition transition,
    bool is_failure,
    bool is_reload,
    bool is_same_document,
    bool dest_is_restore,
    bool dest_is_view_source_mode,
    bool was_server_redirect,
    bool cross_origin_opener_policy_mismatch,
    bool should_replace_current_entry,
    bool force_new_browsing_instance,
    std::string* reason) {
  // On renderer-initiated navigations, when the frame initiating the navigation
  // and the frame being navigated differ, |source_instance| is set to the
  // SiteInstance of the initiating frame. |dest_instance| is present on session
  // history navigations. The two cannot be set simultaneously.
  DCHECK(!source_instance || !dest_instance);

  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();
  BrowserContext* browser_context =
      GetNavigationController().GetBrowserContext();

  // Do not currently swap processes for navigations in webview tag guests,
  // unless site isolation is enabled for them.
  if (current_instance->IsGuest() &&
      !SiteIsolationPolicy::IsSiteIsolationForGuestsEnabled() &&
      !(nw::gGuestSwapProcessHook && nw::gGuestSwapProcessHook(browser_context,
                                                               dest_url_info.url))) {
    AppendReason(reason,
                 "GetSiteInstanceForNavigation => current_instance (IsGuest)");
    return current_instance;
  }

  // Determine if we need a new BrowsingInstance for this entry.  If true, this
  // implies that it will get a new SiteInstance (and likely process), and that
  // other tabs in the current BrowsingInstance will be unable to script it.
  // This is used for cases that require a process swap even in the
  // process-per-tab model, such as WebUI pages.

  // First determine the effective URL of the current RenderFrameHost. This is
  // the last URL it successfully committed. If it has yet to commit a URL, this
  // falls back to the Site URL of its SiteInstance.
  // Note: the effective URL of the current RenderFrameHost may differ from the
  // URL of the last committed NavigationEntry, which cannot be used to decide
  // whether to use a new SiteInstance. This happens when navigating a subframe,
  // or when a new RenderFrameHost has been swapped in at the beginning of a
  // navigation to replace a crashed RenderFrameHost.
  const GURL& current_effective_url =
      !render_frame_host_->last_successful_url().is_empty()
          ? SiteInstanceImpl::GetEffectiveURL(
                browser_context, render_frame_host_->last_successful_url())
          : render_frame_host_->GetSiteInstance()->GetSiteInfo().site_url();

  // Determine if the current RenderFrameHost is in view source mode.
  // TODO(clamy): If the current_effective_url doesn't match the last committed
  // NavigationEntry's URL, current_is_view_source_mode should not be computed
  // using the NavigationEntry. This can happen when a tab crashed, and a new
  // RenderFrameHost was swapped in at the beginning of the navigation. See
  // https://crbug.com/766630.
  NavigationEntry* current_entry =
      GetNavigationController().GetLastCommittedEntry();
  bool current_is_view_source_mode =
      (current_entry && !current_entry->IsInitialEntry())
          ? current_entry->IsViewSourceMode()
          : dest_is_view_source_mode;

  SiteInstanceImpl* current_instance_impl =
      static_cast<SiteInstanceImpl*>(current_instance);
  ShouldSwapBrowsingInstance should_swap_result =
      force_new_browsing_instance
          ? ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap
          : ShouldSwapBrowsingInstancesForNavigation(
                current_effective_url, current_is_view_source_mode,
                source_instance, current_instance_impl, dest_instance,
                dest_url_info, dest_is_view_source_mode, transition, is_failure,
                is_reload, is_same_document,
                cross_origin_opener_policy_mismatch, was_server_redirect,
                should_replace_current_entry);

  TraceShouldSwapBrowsingInstanceResult(frame_tree_node_->frame_tree_node_id(),
                                        should_swap_result);

  bool proactive_swap =
      (should_swap_result ==
           ShouldSwapBrowsingInstance::kYes_CrossSiteProactiveSwap ||
       should_swap_result ==
           ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap);
  bool should_swap =
      (should_swap_result == ShouldSwapBrowsingInstance::kYes_ForceSwap) ||
      proactive_swap;
  if (frame_tree_node_->IsMainFrame()) {
    if (BackForwardCacheMetrics* back_forward_cache_metrics =
            render_frame_host_->GetBackForwardCacheMetrics()) {
      back_forward_cache_metrics->SetBrowsingInstanceSwapResult(
          should_swap_result);
    }
  }

  SiteInstanceDescriptor new_instance_descriptor = DetermineSiteInstanceForURL(
      dest_url_info, source_instance, current_instance, dest_instance,
      transition, is_failure, dest_is_restore, dest_is_view_source_mode,
      should_swap, was_server_redirect, reason);

  scoped_refptr<SiteInstance> new_instance =
      ConvertToSiteInstance(new_instance_descriptor, candidate_instance);
  SiteInstanceImpl* new_instance_impl =
      static_cast<SiteInstanceImpl*>(new_instance.get());
  DCHECK(IsSiteInstanceCompatibleWithWebExposedIsolation(
      new_instance_impl, dest_url_info.web_exposed_isolation_info));

  // If |should_swap| is true, we must use a different SiteInstance than the
  // current one. If we didn't, we would have two RenderFrameHosts in the same
  // SiteInstance and the same frame, breaking lookup of RenderFrameHosts by
  // SiteInstance.
  if (should_swap)
    CHECK_NE(new_instance, current_instance);

  if (new_instance == current_instance) {
    // If we're navigating to the same site instance, we won't need to use the
    // current spare RenderProcessHost.
    RenderProcessHostImpl::NotifySpareManagerAboutRecentlyUsedBrowserContext(
        browser_context);
  }

  // Double-check that the new SiteInstance is associated with the right
  // BrowserContext.
  DCHECK_EQ(new_instance->GetBrowserContext(), browser_context);

  // If |new_instance| is a new SiteInstance for a subframe that requires a
  // dedicated process, set its process reuse policy so that such subframes are
  // consolidated into existing processes for that site.
  if (!frame_tree_node_->IsMainFrame() &&
      !new_instance_impl->HasProcess() &&
      new_instance_impl->RequiresDedicatedProcess()) {
    // Also give the embedder and user-specifiable feature a chance to override
    // this decision. Certain frames have different enough workloads so that
    // it's better to avoid placing a subframe into an existing process for
    // better performance isolation.  See https://crbug.com/899418.
    if (!base::FeatureList::IsEnabled(features::kDisableProcessReuse) &&
        GetContentClient()->browser()->ShouldSubframesTryToReuseExistingProcess(
            frame_tree_node_->frame_tree()->GetMainFrame())) {
      new_instance_impl->set_process_reuse_policy(
          SiteInstanceImpl::ProcessReusePolicy::
              REUSE_PENDING_OR_COMMITTED_SITE);
    }
  }

  bool is_same_site_proactive_swap =
      (should_swap_result ==
       ShouldSwapBrowsingInstance::kYes_SameSiteProactiveSwap);
  bool reuse_current_process_if_possible = false;
  // With proactive BrowsingInstance swap, we should try to reuse the current
  // SiteInstance's process. This avoids swapping processes too many times,
  // which might cause performance regressions.
  // Note: process reuse might not be possible in some cases, e.g. for
  // cross-site navigations when the current SiteInstance needs a dedicated
  // process.

  // Process-reuse cases include:
  // 1) When ProactivelySwapBrowsingInstance with process-reuse is explicitly
  // enabled. In this case, we will try to reuse process on both cross-site and
  // same-site navigations.
  if (IsProactivelySwapBrowsingInstanceWithProcessReuseEnabled() &&
      proactive_swap &&
      (!current_instance->RequiresDedicatedProcess() ||
       is_same_site_proactive_swap)) {
    reuse_current_process_if_possible = true;
  }

  // 2) When BackForwardCache is enabled on same-site navigations.
  // Note 1: When BackForwardCache is disabled, we typically reuse processes on
  // same-site navigations. This follows that behavior.
  // Note 2: This doesn't cover cross-site navigations. Cross-site process-reuse
  // is being experimented independently and is covered in path #1 above.
  // See crbug.com/1122974 for further details.
  if (IsSameSiteBackForwardCacheEnabled() && is_same_site_proactive_swap) {
    reuse_current_process_if_possible = true;
  }

  // 3) When we're doing a same-site history navigation with different
  // BrowsingInstances. We typically do not swap BrowsingInstances on same-site
  // navigations. This might indicate that the original navigation did a
  // proactive BrowsingInstance swap (and process-reuse) before, so we should
  // try to reuse the current process.
  bool is_history_navigation = !!dest_instance;
  bool swapped_browsing_instance =
      !new_instance->IsRelatedSiteInstance(current_instance);
  bool is_same_site_proactive_swap_enabled =
      IsProactivelySwapBrowsingInstanceOnSameSiteNavigationEnabled() ||
      IsSameSiteBackForwardCacheEnabled();
  if (is_same_site_proactive_swap_enabled && is_history_navigation &&
      swapped_browsing_instance &&
      render_frame_host_->IsNavigationSameSite(dest_url_info)) {
    reuse_current_process_if_possible = true;
  }

  if (reuse_current_process_if_possible) {
    DCHECK(frame_tree_node_->IsMainFrame());
    new_instance_impl->ReuseCurrentProcessIfPossible(
        current_instance->GetProcess());
  }

  return new_instance;
}

bool RenderFrameHostManager::InitializeMainRenderFrameForImmediateUse() {
  // TODO(jam): this copies some logic inside GetFrameHostForNavigation, which
  // also duplicates logic in Navigate. They should all use this method, but
  // that involves slight reordering.
  // http://crbug.com/794229
  DCHECK(frame_tree_node_->IsMainFrame());
  if (render_frame_host_->IsRenderFrameLive())
    return true;

  render_frame_host_->reset_must_be_replaced();

  if (!ReinitializeMainRenderFrame(render_frame_host_.get())) {
    NOTREACHED();
    return false;
  }

  EnsureRenderFrameHostPageFocusConsistent();

  // TODO(nasko): This is a very ugly hack. The Chrome extensions process
  // manager still uses NotificationService and expects to see a
  // RenderViewHost changed notification after WebContents and
  // RenderFrameHostManager are completely initialized. This should be
  // removed once the process manager moves away from NotificationService.
  // See https://crbug.com/462682.
  delegate_->NotifyMainFrameSwappedFromRenderManager(nullptr,
                                                     render_frame_host_.get());
  return true;
}

void RenderFrameHostManager::PrepareForInnerDelegateAttach(
    RenderFrameHost::PrepareForInnerWebContentsAttachCallback callback) {
  CHECK(frame_tree_node_->parent());
  attach_inner_delegate_callback_ = std::move(callback);
  DCHECK_EQ(attach_to_inner_delegate_state_, AttachToInnerDelegateState::NONE);
  attach_to_inner_delegate_state_ = AttachToInnerDelegateState::PREPARE_FRAME;
  if (current_frame_host()->ShouldDispatchBeforeUnload(
          false /* check_subframes_only */)) {
    // If there are beforeunload handlers in the frame or a nested subframe we
    // should first dispatch the event and wait for the ACK form the renderer
    // before proceeding with CreateNewFrameForInnerDelegateAttachIfNecessary.
    current_frame_host()->DispatchBeforeUnload(
        RenderFrameHostImpl::BeforeUnloadType::INNER_DELEGATE_ATTACH, false);
    return;
  }
  CreateNewFrameForInnerDelegateAttachIfNecessary();
}

RenderFrameHostManager::SiteInstanceDescriptor
RenderFrameHostManager::DetermineSiteInstanceForURL(
    const UrlInfo& dest_url_info,
    SiteInstance* source_instance,
    SiteInstance* current_instance,
    SiteInstance* dest_instance,
    ui::PageTransition transition,
    bool is_failure,
    bool dest_is_restore,
    bool dest_is_view_source_mode,
    bool force_browsing_instance_swap,
    bool was_server_redirect,
    std::string* reason) {
  // Note that this function should return SiteInstance with
  // SiteInstanceRelation::UNRELATED relation to `current_instance` iff
  // `force_browsing_instance_swap` is true. All cases that result in an
  // unrelated SiteInstance should return Yes_ForceSwap or Yes_ProactiveSwap in
  // ShouldSwapBrowsingInstancesForNavigation.
  SiteInstanceImpl* current_instance_impl =
      static_cast<SiteInstanceImpl*>(current_instance);
  NavigationControllerImpl& controller = GetNavigationController();

  // If the entry has an instance already we should usually use it, unless it is
  // no longer suitable.
  if (dest_instance) {
    // Note: The later call to IsSuitableForUrlInfo does not have context about
    // error page navigations, so we cannot rely on it to return correct value
    // when error pages are involved.
    if (IsSiteInstanceCompatibleWithErrorIsolation(
            dest_instance, *frame_tree_node_, is_failure)) {
      if (IsSiteInstanceCompatibleWithWebExposedIsolation(
              dest_instance, dest_url_info.web_exposed_isolation_info)) {
        // TODO(nasko,creis): The check whether data: or about: URLs are allowed
        // to commit in the current process should be in IsSuitableForUrlInfo.
        // However, making this change has further implications and needs more
        // investigation of what behavior changes. For now, use a conservative
        // approach and explicitly check before calling IsSuitableForUrlInfo.
        SiteInstanceImpl* dest_instance_impl =
            static_cast<SiteInstanceImpl*>(dest_instance);
        // Make sure that if the destination frame is sandboxed that we don't
        // skip the IsSuitableForUrlInfo() check. Note that it's impossible to
        // have a sandboxed parent but unsandboxed child.
        bool is_data_or_about_and_not_sandboxed =
            IsDataOrAbout(dest_url_info.url) && !dest_url_info.is_sandboxed;
        if (is_data_or_about_and_not_sandboxed ||
            dest_instance_impl->IsSuitableForUrlInfo(dest_url_info)) {
          // If we are forcing a swap, this should be in a different
          // BrowsingInstance.
          if (force_browsing_instance_swap) {
            CHECK(!dest_instance->IsRelatedSiteInstance(
                render_frame_host_->GetSiteInstance()));
          }
          AppendReason(reason, "DetermineSiteInstanceForURL => dest_instance");
          return SiteInstanceDescriptor(dest_instance);
        }
      }
    }
  }

  // If error page navigations should be isolated, ensure a dedicated
  // SiteInstance is used for them.
  if (is_failure && frame_tree_node_->IsErrorPageIsolationEnabled()) {
    // If the target URL requires a BrowsingInstance swap, put the error page
    // in a new BrowsingInstance, since the scripting relationships would
    // have been broken anyway if there were no error. Otherwise, we keep it
    // in the same BrowsingInstance to preserve scripting relationships after
    // reloads. In UrlInfo below we use kNone for OriginIsolationRequest since
    // error pages cannot request origin isolation: this is done implicitly in
    // the UrlInfoInit constructor.
    AppendReason(reason, "DetermineSiteInstanceForURL => error-instance");
    return SiteInstanceDescriptor(
        UrlInfo(UrlInfoInit(GURL(kUnreachableWebDataURL))
                    .WithWebExposedIsolationInfo(
                        dest_url_info.web_exposed_isolation_info)),
        force_browsing_instance_swap ? SiteInstanceRelation::UNRELATED
                                     : SiteInstanceRelation::RELATED);
  }

  // If a swap is required, we need to force the SiteInstance AND
  // BrowsingInstance to be different ones, using CreateForURL.
  bool can_use_source_instance = CanUseSourceSiteInstance(
      dest_url_info, source_instance, was_server_redirect, is_failure);
  if (force_browsing_instance_swap) {
    // In rare cases, `source_instance` maybe be already in another
    // BrowsingInstance from `current_instance` (e.g. see how the
    // ExtensionApiTabTest.HostPermission test uses chrome.tabs.update API to
    // navigate from "chrome://new-tab-page/" to "about:blank").  In such cases,
    // using `source_instance` will 1) effectively force browsing instance swap
    // and 2) use a process compatible with "about:blank"'s origin (unlike a
    // new, unrelated SiteInstance that might use an unlocked process even
    // when the origin requires a locked process).
    if (can_use_source_instance &&
        !source_instance->IsRelatedSiteInstance(current_instance)) {
      AppendReason(reason,
                   "DetermineSiteInstanceForURL => source_instance"
                   " (browsing-instance-swap)");
      return SiteInstanceDescriptor(source_instance);
    }

    // Force browsing instance_swap by asking for a new, unrelated SiteInstance.
    AppendReason(reason,
                 "DetermineSiteInstanceForURL / browsing-instance-swap");
    return SiteInstanceDescriptor(dest_url_info,
                                  SiteInstanceRelation::UNRELATED);
  }

  // TODO(https://crbug.com/566091): Don't create OOPIFs on the NTP.  Remove
  // this when the NTP supports OOPIFs or is otherwise omitted from site
  // isolation policy.
  if (!frame_tree_node_->IsMainFrame()) {
    SiteInstance* parent_site_instance =
        frame_tree_node_->parent()->GetSiteInstance();
    if (GetContentClient()->browser()->ShouldStayInParentProcessForNTP(
            dest_url_info.url, parent_site_instance)) {
      // NTP is considered non-isolated.
      DCHECK(!dest_url_info.IsIsolated());
      AppendReason(reason,
                   "DetermineSiteInstanceForURL => parent_site_instance");
      return SiteInstanceDescriptor(parent_site_instance);
    }
  }

  // Check if we should use `source_instance`, such as for about:blank and data:
  // URLs.  Preferring `source_instance` over a site-less `current_instance` is
  // important in session restore scenarios which should commit in the
  // SiteInstance based on FrameNavigationEntry's initiator_origin.
  if (can_use_source_instance) {
    AppendReason(reason, "DetermineSiteInstanceForURL => source_instance");
    return SiteInstanceDescriptor(source_instance);
  }

  // If we haven't used our SiteInstance yet, then we can use it for this
  // entry.  We won't commit the SiteInstance to this site until the response
  // is received (in OnResponseStarted), unless the navigation entry was
  // restored or it's a Web UI as described below.
  // TODO(ahemery): In theory we should be able to go for an unused SiteInstance
  // with the same web exposed isolation status.
  if (!current_instance_impl->HasSite() && !dest_url_info.IsIsolated() &&
      !current_instance_impl->IsCrossOriginIsolated()) {
    // If we've already created a SiteInstance for our destination, we don't
    // want to use this unused SiteInstance; use the existing one.  (We don't
    // do this check if the current_instance has a site, because for now, we
    // want to compare against the current URL and not the SiteInstance's site.
    // In this case, there is no current URL, so comparing against the site is
    // ok.  See additional comments below.)
    //
    // Also, if the URL's site should use process-per-site mode and there is an
    // existing process for the site, we should use it.  We can call
    // GetRelatedSiteInstance() for this, which will eagerly set the site and
    // thus use the correct process.
    DCHECK_EQ(controller.GetBrowserContext(),
              current_instance_impl->GetBrowserContext());

    const SiteInfo dest_site_info =
        current_instance_impl->DeriveSiteInfo(dest_url_info);
    bool use_process_per_site =
        dest_site_info.ShouldUseProcessPerSite(
            current_instance_impl->GetBrowserContext()) &&
        RenderProcessHostImpl::GetSoleProcessHostForSite(
            current_instance_impl->GetIsolationContext(), dest_site_info);

    if (current_instance_impl->HasRelatedSiteInstance(dest_site_info) ||
        use_process_per_site) {
      AppendReason(reason,
                   "DetermineSiteInstanceForURL / !current->HasSite / "
                   "has-related-site-instance-or-using-process-per-site");
      return SiteInstanceDescriptor(dest_url_info,
                                    SiteInstanceRelation::RELATED);
    }

    // For extensions, Web UI URLs (such as the new tab page), and apps we do
    // not want to use the `current_instance_impl` if it has no site, since it
    // will have a non-privileged RenderProcessHost. Create a new SiteInstance
    // for this URL instead (with the correct process type).
    if (!current_instance_impl->IsSuitableForUrlInfo(dest_url_info)) {
      AppendReason(reason,
                   "DetermineSiteInstanceForURL / !current->HasSite / "
                   "!current_instance->IsSuitable");
      return SiteInstanceDescriptor(dest_url_info,
                                    SiteInstanceRelation::RELATED);
    }

    // Normally the "site" on the SiteInstance is set lazily when the load
    // actually commits. This is to support better process sharing in case
    // the site redirects to some other site: we want to use the destination
    // site in the site instance.
    //
    // In the case of session restore, as it loads all the pages immediately
    // we need to set the site first, otherwise after a restore none of the
    // pages would share renderers in process-per-site.
    //
    // The embedder can request some urls never to be assigned to SiteInstance
    // through the ShouldAssignSiteForURL() content client method, so that
    // renderers created for particular chrome urls (e.g. the chrome-native://
    // scheme) can be reused for subsequent navigations in the same WebContents.
    // See http://crbug.com/386542.
    if (dest_is_restore &&
        SiteInstanceImpl::ShouldAssignSiteForURL(dest_url_info.url)) {
      current_instance_impl->ConvertToDefaultOrSetSite(dest_url_info);
    }

    AppendReason(reason,
                 "DetermineSiteInstanceForURL => current_instance_impl");
    return SiteInstanceDescriptor(current_instance_impl);
  }

  // Use the current SiteInstance for same site navigations.
  if (render_frame_host_->IsNavigationSameSite(dest_url_info)) {
    AppendReason(reason, "DetermineSiteInstanceForURL / same-site-navigation");
    return SiteInstanceDescriptor(render_frame_host_->GetSiteInstance());
  }

  // Shortcut some common cases for reusing an existing frame's SiteInstance.
  // There are several reasons for this:
  // - with hosted apps, this allows same-site, non-app subframes to be kept
  //   inside the hosted app process.
  // - this avoids putting same-site iframes into different processes after
  //   navigations from isolated origins.  This matters for some OAuth flows;
  //   see https://crbug.com/796912.
  //
  // TODO(alexmos): Ideally, the right SiteInstance for these cases should be
  // found later, as part of creating a new related SiteInstance from
  // BrowsingInstance::GetSiteInstanceForURL().  However, the lookup there (1)
  // does not properly deal with hosted apps (see https://crbug.com/718516),
  // and (2) does not yet deal with cases where a SiteInstance is shared by
  // several sites that don't require a dedicated process (see
  // https://crbug.com/787576).
  if (!frame_tree_node_->IsMainFrame()) {
    RenderFrameHostImpl* main_frame =
        frame_tree_node_->frame_tree()->root()->current_frame_host();
    if (IsCandidateSameSite(main_frame, dest_url_info)) {
      AppendReason(reason,
                   "DetermineSiteInstanceForURL / subframe-reuse => "
                   "main-frame-instance");
      return SiteInstanceDescriptor(main_frame->GetSiteInstance());
    }
    RenderFrameHostImpl* parent = frame_tree_node_->parent();
    if (IsCandidateSameSite(parent, dest_url_info)) {
      AppendReason(reason,
                   "DetermineSiteInstanceForURL / subframe-reuse => "
                   "parent-instance");
      return SiteInstanceDescriptor(parent->GetSiteInstance());
    }
  }
  if (frame_tree_node_->opener()) {
    RenderFrameHostImpl* opener_frame =
        frame_tree_node_->opener()->current_frame_host();
    if (IsCandidateSameSite(opener_frame, dest_url_info)) {
      AppendReason(reason, "DetermineSiteInstanceForURL => opener-instance");
      return SiteInstanceDescriptor(opener_frame->GetSiteInstance());
    }
  }

  // Keep subframes in the parent's SiteInstance unless a dedicated process is
  // required for either the parent or the subframe's destination URL. Although
  // this consolidation is usually handled by default SiteInstances, there are
  // some corner cases in which default SiteInstances cannot currently be used,
  // such as file: URLs.  This logic prevents unneeded OOPIFs in those cases.
  // This turns out to be important for correctness on Android Webview, which
  // does not yet support OOPIFs (https://crbug.com/1101214).
  // TODO(https://crbug.com/1103352): Remove this block when default
  // SiteInstances support file: URLs.
  //
  // Also if kProcessSharingWithStrictSiteInstances is enabled, don't lump the
  // subframe into the same SiteInstance as the parent. These separate
  // SiteInstances can get assigned to the same process later.
  if (!base::FeatureList::IsEnabled(
          features::kProcessSharingWithStrictSiteInstances)) {
    if (!frame_tree_node_->IsMainFrame()) {
      RenderFrameHostImpl* parent = frame_tree_node_->parent();
      auto& parent_isolation_context =
          parent->GetSiteInstance()->GetIsolationContext();

      auto site_info =
          SiteInfo::Create(parent_isolation_context, dest_url_info);
      if (!parent->GetSiteInstance()->RequiresDedicatedProcess() &&
          !site_info.RequiresDedicatedProcess(parent_isolation_context)) {
        AppendReason(reason,
                     "DetermineSiteInstanceForURL => parent-instance"
                     " (no-strict-site-instances)");
        return SiteInstanceDescriptor(parent->GetSiteInstance());
      }
    }
  }

  // Start the new renderer in a new SiteInstance, but in the current
  // BrowsingInstance, unless the destination URL's web-exposed isolated state
  // cannot be hosted by it.
  if (IsSiteInstanceCompatibleWithWebExposedIsolation(
          render_frame_host_->GetSiteInstance(),
          dest_url_info.web_exposed_isolation_info)) {
    AppendReason(reason,
                 "DetermineSiteInstanceForURL / fallback / coop-compatible");
    return SiteInstanceDescriptor(dest_url_info, SiteInstanceRelation::RELATED);
  } else {
    AppendReason(
        reason, "DetermineSiteInstanceForURL / fallback / not-coop-compatible");
    return SiteInstanceDescriptor(dest_url_info,
                                  SiteInstanceRelation::UNRELATED);
  }
}

bool RenderFrameHostManager::IsBrowsingInstanceSwapAllowedForPageTransition(
    ui::PageTransition transition,
    const GURL& dest_url) {
  // Disallow BrowsingInstance swaps for subframes.
  if (!frame_tree_node_->IsMainFrame())
    return false;

  // Skip data: and file: URLs, as some tests rely on browser-initiated
  // navigations to those URLs to stay in the same process.  Swapping
  // BrowsingInstances for those URLs may not carry much benefit anyway, since
  // they're likely less common.
  //
  // Note that such URLs are not considered same-site, but since their
  // SiteInstance site URL is based only on scheme (e.g., all data URLs use a
  // site URL of "data:"), a browser-initiated navigation from one such URL to
  // another will still stay in the same SiteInstance, due to the matching site
  // URL.
  if (dest_url.SchemeIsFile() || dest_url.SchemeIs(url::kDataScheme))
    return false;

  // Allow page transitions corresponding to certain browser-initiated
  // navigations: typing in the URL, using a bookmark, or using search.
  switch (ui::PageTransitionStripQualifier(transition)) {
    case ui::PAGE_TRANSITION_TYPED:
    case ui::PAGE_TRANSITION_AUTO_BOOKMARK:
    case ui::PAGE_TRANSITION_GENERATED:
    case ui::PAGE_TRANSITION_KEYWORD:
      return true;
    // TODO(alexmos): PAGE_TRANSITION_AUTO_TOPLEVEL is not included due to a
    // bug that would cause unneeded BrowsingInstance swaps for DevTools,
    // https://crbug.com/733767.  Once that bug is fixed, consider adding this
    // transition here.
    default:
      return false;
  }
}

scoped_refptr<SiteInstance> RenderFrameHostManager::ConvertToSiteInstance(
    const SiteInstanceDescriptor& descriptor,
    SiteInstanceImpl* candidate_instance) {
  SiteInstanceImpl* current_instance = render_frame_host_->GetSiteInstance();

  // If we are asked to return a related SiteInstance but the BrowsingInstance
  // has a different cross_origin_isolated state, something went wrong.
  CHECK(descriptor.relation != SiteInstanceRelation::RELATED ||
        WebExposedIsolationInfo::AreCompatible(
            current_instance->GetWebExposedIsolationInfo(),
            descriptor.dest_url_info.web_exposed_isolation_info));

  // Note: If the `candidate_instance` matches the descriptor, it will already
  // be set to `descriptor.existing_site_instance`.
  if (descriptor.existing_site_instance) {
    DCHECK_EQ(descriptor.relation, SiteInstanceRelation::PREEXISTING);
    return descriptor.existing_site_instance.get();
  } else {
    DCHECK_NE(descriptor.relation, SiteInstanceRelation::PREEXISTING);
  }

  // Note: If the `candidate_instance` matches the descriptor,
  // GetRelatedSiteInstance will return it.
  // Note that by the time we get here, we've already ensured that this
  // BrowsingInstance has a compatible cross-origin isolated state, so we are
  // guaranteed to return a SiteInstance that will be compatible with
  // |descriptor.web_exposed_isolation_info|."
  if (descriptor.relation == SiteInstanceRelation::RELATED)
    return current_instance->GetRelatedSiteInstanceImpl(
        descriptor.dest_url_info);

  // At this point we know an unrelated site instance must be returned.

  // If the current SiteInstance is for a guest, the new unrelated
  // SiteInstance must also be for a guest and must stay in the same
  // StoragePartition.  Note that we should only attempt BrowsingInstance
  // swaps in guests when site isolation for guests is enabled.
  UrlInfo dest_url_info = descriptor.dest_url_info;
  if (current_instance->IsGuest()) {
    DCHECK(SiteIsolationPolicy::IsSiteIsolationForGuestsEnabled());
    dest_url_info.storage_partition_config =
        current_instance->GetSiteInfo().storage_partition_config();
  }

  // First check if the candidate SiteInstance matches.  For example, we get
  // here when we recompute the SiteInstance after receiving a response, and
  // `candidate_instance` is the SiteInstance that was created at request start
  // time.
  if (candidate_instance &&
      !current_instance->IsRelatedSiteInstance(candidate_instance) &&
      candidate_instance->DoesSiteInfoForURLMatch(dest_url_info)) {
    return candidate_instance;
  }

  // Otherwise return a new SiteInstance in a new BrowsingInstance.
  return SiteInstanceImpl::CreateForUrlInfo(
      GetNavigationController().GetBrowserContext(), dest_url_info,
      current_instance->IsGuest());
}

bool RenderFrameHostManager::CanUseSourceSiteInstance(
    const UrlInfo& dest_url_info,
    SiteInstance* source_instance,
    bool was_server_redirect,
    bool is_failure) {
  if (!source_instance)
    return false;

  // We use the source SiteInstance in case of data URLs, about:srcdoc pages and
  // about:blank pages because the content is then controlled and/or scriptable
  // by the initiator and therefore needs to stay in the `source_instance`.
  if (!IsDataOrAbout(dest_url_info.url))
    return false;

  // If `dest_url_info` is sandboxed, then we can't assign it to a SiteInstance
  // that isn't sandboxed. But if the `source_instance` is also sandboxed, then
  // it's possible (e.g. a sandboxed child frame in a sandboxed parent frame).
  if (dest_url_info.is_sandboxed !=
      static_cast<SiteInstanceImpl*>(source_instance)
          ->GetSiteInfo()
          .is_sandboxed()) {
    return false;
  }

  // One exception (where data URLs, about:srcdoc or about:blank pages are *not*
  // controlled by the initiator) is when these URLs are reached via a server
  // redirect.
  //
  // Normally, redirects to data: or about: URLs are disallowed as
  // net::ERR_UNSAFE_REDIRECT, but extensions can still redirect arbitrary
  // requests to those URLs using webRequest or declarativeWebRequest API (for
  // an example, see NavigationInitiatedByCrossSiteSubframeRedirectedTo... test
  // cases in the ChromeNavigationBrowserTest test suite.  For such data: URL
  // redirects, the content is controlled by the extension (rather than by the
  // `source_instance`), so we don't use the `source_instance` for data: URLs if
  // there was a server redirect.
  if (was_server_redirect && dest_url_info.url.SchemeIs(url::kDataScheme))
    return false;

  // Make sure that error isolation is taken into account.  See also
  // ChromeNavigationBrowserTest.RedirectErrorPageReloadToAboutBlank.
  if (!IsSiteInstanceCompatibleWithErrorIsolation(
          source_instance, *frame_tree_node_, is_failure)) {
    return false;
  }

  if (!IsSiteInstanceCompatibleWithWebExposedIsolation(
          source_instance, dest_url_info.web_exposed_isolation_info)) {
    return false;
  }

  // PDF content should never share a SiteInstance with non-PDF content. In
  // practice, this prevents the PDF viewer extension from incorrectly sharing
  // a process with PDF content that was loaded from a data URL.
  if (dest_url_info.is_pdf) {
    DCHECK(!source_instance->GetProcess()->IsPdf());
    return false;
  }

  // Okay to use `source_instance`.
  return true;
}

bool RenderFrameHostManager::IsCandidateSameSite(RenderFrameHostImpl* candidate,
                                                 const UrlInfo& dest_url_info) {
  DCHECK_EQ(GetNavigationController().GetBrowserContext(),
            candidate->GetSiteInstance()->GetBrowserContext());
  if (!WebExposedIsolationInfo::AreCompatible(
          candidate->GetSiteInstance()->GetWebExposedIsolationInfo(),
          dest_url_info.web_exposed_isolation_info)) {
    return false;
  }

  // Note: We are mixing the frame_tree_node_->IsMainFrame() status of this
  // object with the URL & origin of `candidate`. This is to determine if
  // `dest_url_info` would be considered "same site" if `candidate` occupied the
  // position of this object in the frame tree.
  return candidate->GetSiteInstance()->IsNavigationSameSite(
      candidate->last_successful_url(), candidate->GetLastCommittedOrigin(),
      frame_tree_node_->IsMainFrame(), dest_url_info);
}

void RenderFrameHostManager::CreateProxiesForNewRenderFrameHost(
    SiteInstance* old_instance,
    SiteInstance* new_instance,
    bool recovering_without_early_commit,
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  // Only create opener proxies if they are in the same BrowsingInstance.
  if (new_instance->IsRelatedSiteInstance(old_instance)) {
    CreateOpenerProxies(new_instance, frame_tree_node_, browsing_context_state);
  } else {
    // Ensure that the frame tree has RenderFrameProxyHosts for the
    // new SiteInstance in all necessary nodes.  We do this for all frames in
    // the tree, whether they are in the same BrowsingInstance or not.  If
    // |new_instance| is in the same BrowsingInstance as |old_instance|, this
    // will be done as part of CreateOpenerProxies above; otherwise, we do this
    // here.  We will still check whether two frames are in the same
    // BrowsingInstance before we allow them to interact (e.g., postMessage).
    frame_tree_node_->frame_tree()->CreateProxiesForSiteInstance(
        frame_tree_node_, new_instance, browsing_context_state);
  }

  // When navigating same-site and recovering from a crash, create a proxy
  // in the new process. This will be swapped for a frame if we commit.
  // TODO(https://crbug.com/1072817): Consider handling this case in
  // FrameTree::CreateProxiesForSiteInstance.
  if (recovering_without_early_commit &&
      render_frame_host_->GetSiteInstance() == new_instance) {
    if (frame_tree_node_->IsMainFrame()) {
      frame_tree_node_->frame_tree()
          ->GetRenderViewHost(
              static_cast<SiteInstanceImpl*>(new_instance)->group())
          ->SetMainFrameRoutingId(MSG_ROUTING_NONE);
    }

    // As there is an explicit check for |render_frame_host_|'s SiteInstance
    // being the same as the "new" RenderFrameHost,
    // |render_frame_host_->browsing_context_state()| is the right
    // BrowsingContextState to use.
    CreateRenderFrameProxy(new_instance,
                           render_frame_host_->browsing_context_state());
  }
}

void RenderFrameHostManager::CreateProxiesForNewNamedFrame(
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  DCHECK(!frame_tree_node_->frame_name().empty());

  // If this is a top-level frame, create proxies for this node in the
  // SiteInstances of its opener's ancestors, which are allowed to discover
  // this frame by name (see https://crbug.com/511474 and part 4 of
  // https://html.spec.whatwg.org/C/#the-rules-for-choosing-a-browsing-context-given-a-browsing-context-name
  // ).
  FrameTreeNode* opener = frame_tree_node_->opener();
  if (!opener || !frame_tree_node_->IsMainFrame())
    return;
  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();

  // Start from opener's parent.  There's no need to create a proxy in the
  // opener's SiteInstance, since new windows are always first opened in the
  // same SiteInstance as their opener, and if the new window navigates
  // cross-site, that proxy would be created as part of unloading.
  for (RenderFrameHost* ancestor = opener->parent(); ancestor;
       ancestor = ancestor->GetParent()) {
    if (ancestor->GetSiteInstance() != current_instance)
      CreateRenderFrameProxy(ancestor->GetSiteInstance(),
                             browsing_context_state);
  }
}

std::unique_ptr<RenderFrameHostImpl>
RenderFrameHostManager::CreateRenderFrameHost(
    CreateFrameCase create_frame_case,
    SiteInstance* site_instance,
    int32_t frame_routing_id,
    mojo::PendingAssociatedRemote<mojom::Frame> frame_remote,
    const blink::LocalFrameToken& frame_token,
    bool renderer_initiated_creation,
    scoped_refptr<BrowsingContextState> browsing_context_state) {
  FrameTree* frame_tree = frame_tree_node_->frame_tree();

  // Only the kInitChild case passes in a frame routing id.
  DCHECK_EQ(create_frame_case != CreateFrameCase::kInitChild,
            frame_routing_id == MSG_ROUTING_NONE);
  if (frame_routing_id == MSG_ROUTING_NONE) {
    frame_routing_id = site_instance->GetProcess()->GetNextRoutingID();
  }

  scoped_refptr<RenderViewHostImpl> render_view_host =
      frame_tree->GetRenderViewHost(
          static_cast<SiteInstanceImpl*>(site_instance)->group());

  switch (create_frame_case) {
    case CreateFrameCase::kInitChild:
      DCHECK(!frame_tree_node_->IsMainFrame());
      // The first RenderFrameHost for a child FrameTreeNode is always in the
      // same SiteInstance as its parent.
      DCHECK_EQ(frame_tree_node_->parent()->GetSiteInstance(), site_instance);
      // The RenderViewHost must already exist for the parent's SiteInstance.
      DCHECK(render_view_host);
      // Only main frames can be marked as renderer-initiated, as it refers to
      // a renderer-created window.
      DCHECK(!renderer_initiated_creation);
      break;
    case CreateFrameCase::kInitRoot:
      DCHECK(frame_tree_node_->IsMainFrame());
      // The view should not already exist when we are initializing the frame
      // tree.
      DCHECK(!render_view_host);
      break;
    case CreateFrameCase::kCreateSpeculative:
      // We create speculative frames both for main frame and subframe
      // navigations. The view might exist already if the SiteInstance already
      // has frames hosted in the target process. So we don't check the view.
      //
      // A speculative frame should be replacing an existing frame.
      DCHECK(render_frame_host_);
      // Only the initial main frame can be marked as renderer-initiated, as it
      // refers to a renderer-created window. A speculative frame is always
      // created later by the browser.
      DCHECK(!renderer_initiated_creation);
      break;
  }
  if (!render_view_host) {
    render_view_host = frame_tree->CreateRenderViewHost(
        site_instance, frame_routing_id,
        /*swapped_out=*/false, renderer_initiated_creation,
        features::GetBrowsingContextMode() ==
                features::BrowsingContextStateImplementationType::
                    kSwapForCrossBrowsingInstanceNavigations
            ? browsing_context_state
            : nullptr);
  }
  CHECK(render_view_host);

  // LifecycleStateImpl of newly created RenderFrameHost.
  LifecycleStateImpl lifecycle_state;

  if (create_frame_case == CreateFrameCase::kCreateSpeculative) {
    lifecycle_state = LifecycleStateImpl::kSpeculative;
  } else {
    // For the creation of initial documents:
    // - We create RenderFrameHost in kPrerendering state in case of
    // prerendering frame tree.
    // - We create RenderFrameHost in kActive state in all other cases.
    lifecycle_state = frame_tree->is_prerendering()
                          ? LifecycleStateImpl::kPrerendering
                          : LifecycleStateImpl::kActive;
  }

  return RenderFrameHostFactory::Create(
      site_instance, std::move(render_view_host),
      frame_tree->render_frame_delegate(), frame_tree, frame_tree_node_,
      frame_routing_id, std::move(frame_remote), frame_token,
      renderer_initiated_creation, lifecycle_state,
      std::move(browsing_context_state));
}

bool RenderFrameHostManager::CreateSpeculativeRenderFrameHost(
    SiteInstance* old_instance,
    SiteInstance* new_instance,
    bool recovering_without_early_commit) {
  CHECK(new_instance);
  // This DCHECK is going to be fully removed as part of RenderDocument [1].
  //
  // With RenderDocument for sub frames or main frames: cross-document
  // navigation creates a new RenderFrameHost. The navigation is potentially
  // same-SiteInstance.
  //
  // With RenderDocument for crashed frames: navigations from a crashed
  // RenderFrameHost creates a new RenderFrameHost. The navigation is
  // potentially same-SiteInstance.
  //
  // [1] http://crbug.com/936696
  DCHECK(old_instance != new_instance ||
         render_frame_host_->must_be_replaced() ||
         ShouldCreateNewHostForSameSiteSubframe());

  // The process for the new SiteInstance may (if we're sharing a process with
  // another host that already initialized it) or may not (we have our own
  // process or the existing process crashed) have been initialized. Calling
  // Init() multiple times will be ignored, so this is safe.
  if (!new_instance->GetProcess()->Init())
    return false;

  scoped_refptr<BrowsingContextState> browsing_context_state;
  if (features::GetBrowsingContextMode() ==
      features::BrowsingContextStateImplementationType::
          kLegacyOneToOneWithFrameTreeNode) {
    browsing_context_state = render_frame_host_->browsing_context_state();
  } else {
    // For speculative frame hosts, we will need to create a new
    // BrowsingContextState when we have a cross-BrowsingInstance navigation,
    // as the browsing context + BrowsingInstance combination changes. An
    // exception is when the RenderViewHost for the speculative
    // RenderFrameHost's SiteInstance is still around, e.g. on history
    // navigations.
    // TODO(crbug.com/1179502): FrameReplicationState is a mix of things that
    // are per-frame, per-browsing context and per-document. Currently, we pass
    // the entire FrameReplicationState to match the old behaviour of storing
    // FrameReplicationState on FrameTreeNode. We should consider splitting
    // FrameReplicationState into multiple structs with different lifetimes.
    // TODO(crbug.com/1270671): conditionally avoid copying the frame name here
    // if DidChangeName arrives after DidCommitNavigation.
    if (render_frame_host_->GetSiteInstance()->IsRelatedSiteInstance(
            new_instance)) {
      // We're reusing the current BrowsingInstance, so also reuse the
      // BrowsingContextState.
      browsing_context_state = render_frame_host_->browsing_context_state();
    } else {
      scoped_refptr<RenderViewHostImpl> render_view_host =
          frame_tree_node_->frame_tree()->GetRenderViewHost(
              static_cast<SiteInstanceImpl*>(new_instance)->group());
      if (render_view_host) {
        // If we reuse a RenderViewHost for a main-frame cross-BrowsingInstance
        // navigation, we need to reuse the RenderFrameProxyHost representing
        // its main frame and BrowsingContextState associated with this proxy.
        // This is possible when we are performing a history navigation (which
        // reuses existing SiteInstance associated with the corresponding
        // FrameNavigationEntry) and there is a pending deletion RenderViewHost
        // associated with the same SiteInstance, and we are creating a new
        // BrowsingContextState. Both proxies and RenderViewHosts are keyed by
        // SiteInstance(Group), and we don't want to have two different proxies
        // in the same frame belonging to the same RenderViewHost due to these
        // proxies belonging to different BrowsingContextStates. Since
        // RenderViewHost is also keyed by SiteInstance, when there is an
        // existing RenderViewHost, we want to use the correct corresponding
        // proxy when unloading a frame and committing a navigation.
        // TODO(crbug.com/1302242): Migrate storage of SiteInstance(Group) =>
        // RenderViewHost to BrowsingContextState to eliminate this branch.
        browsing_context_state =
            render_view_host->main_browsing_context_state();
        CHECK(frame_tree_node_->IsMainFrame());
      } else {
        browsing_context_state = base::MakeRefCounted<BrowsingContextState>(
            render_frame_host_->browsing_context_state()
                ->current_replication_state()
                .Clone(),
            frame_tree_node_->parent(), new_instance->GetBrowsingInstanceId());

        // Add a proxy to the outer delegate if one exists, as this is not
        // copied over to the new BrowsingContextState otherwise.
        FrameTreeNode* outer_contents_frame_tree_node = GetOuterDelegateNode();
        if (outer_contents_frame_tree_node) {
          DCHECK(outer_contents_frame_tree_node->parent());
          browsing_context_state->CreateOuterDelegateProxy(
              outer_contents_frame_tree_node->parent()->GetSiteInstance(),
              frame_tree_node_);
        }
      }
    }
  }

  CreateProxiesForNewRenderFrameHost(old_instance, new_instance,
                                     recovering_without_early_commit,
                                     browsing_context_state);

  speculative_render_frame_host_ = CreateSpeculativeRenderFrame(
      new_instance, recovering_without_early_commit, browsing_context_state);
  return !!speculative_render_frame_host_;
}

std::unique_ptr<RenderFrameHostImpl>
RenderFrameHostManager::CreateSpeculativeRenderFrame(
    SiteInstance* instance,
    bool recovering_without_early_commit,
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  TRACE_EVENT("navigation",
              "RenderFrameHostManager::CreateSpeculativeRenderFrame",
              ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);

  CHECK(instance);
  // This DCHECK is going to be fully removed as part of RenderDocument [1].
  //
  // With RenderDocument for sub frames or main frames: cross-document
  // navigation creates a new RenderFrameHost. The navigation is potentially
  // same-SiteInstance.
  //
  // With RenderDocument for crashed frames: navigations from a crashed
  // RenderFrameHost creates a new RenderFrameHost. The navigation is
  // potentially same-SiteInstance.
  //
  // [1] http://crbug.com/936696
  DCHECK(render_frame_host_->GetSiteInstance() != instance ||
         render_frame_host_->must_be_replaced() ||
         ShouldCreateNewHostForSameSiteSubframe());

  std::unique_ptr<RenderFrameHostImpl> new_render_frame_host =
      CreateRenderFrameHost(CreateFrameCase::kCreateSpeculative, instance,
                            /*frame_routing_id=*/MSG_ROUTING_NONE,
                            mojo::PendingAssociatedRemote<mojom::Frame>(),
                            blink::LocalFrameToken(),
                            /*renderer_initiated_creation=*/false,
                            browsing_context_state);
  DCHECK_EQ(new_render_frame_host->GetSiteInstance(), instance);

  // Prevent the process from exiting while we're trying to navigate in it.
  new_render_frame_host->GetProcess()->AddPendingView();

  RenderViewHostImpl* render_view_host =
      new_render_frame_host->render_view_host();
  if (frame_tree_node_->IsMainFrame()) {
    if (render_view_host == render_frame_host_->render_view_host()) {
      // We are replacing the main frame's host with |new_render_frame_host|.
      // RenderViewHost is reused after a crash and in order for InitRenderView
      // to find |new_render_frame_host| as the new main frame, we set the
      // routing ID now. This is safe to do as we will call CommitPending() in
      // GetFrameHostForNavigation() before yielding to other tasks.
      render_view_host->SetMainFrameRoutingId(
          new_render_frame_host->GetRoutingID());
    }

    SiteInstanceGroup* site_instance_group =
        static_cast<SiteInstanceImpl*>(instance)->group();
    if (!InitRenderView(site_instance_group, render_view_host,
                        browsing_context_state->GetRenderFrameProxyHost(
                            site_instance_group))) {
      return nullptr;
    }

    // If we are reusing the RenderViewHost and it doesn't already have a
    // RenderWidgetHostView, we need to create one if this is the main frame.
    if (!render_view_host->GetWidget()->GetView()) {
      // TODO(crbug.com/1161585): The RenderWidgetHostView should be created
      // *before* we create the renderer-side objects through InitRenderView().
      // Then we should remove the null-check for the RenderWidgetHostView in
      // RenderWidgetHostImpl::RendererWidgetCreated().
      delegate_->CreateRenderWidgetHostViewForRenderManager(render_view_host);
      // If we are recovering a crashed frame in the same SiteInstance and we
      // are not skipping early commit then we will create a proxy and that will
      // prevent the regular outer delegate reattach path in
      // CreateRenderViewForRenderManager() from working.
      if (recovering_without_early_commit &&
          render_frame_host_->GetSiteInstance() == instance)
        delegate_->ReattachOuterDelegateIfNeeded();
    }
    // And since we are reusing the RenderViewHost make sure it is hidden, like
    // a new RenderViewHost would be, until navigation commits.
    render_view_host->GetWidget()->GetView()->Hide();
  }

  DCHECK(render_view_host->IsRenderViewLive());
  // RenderViewHost for |instance| might exist prior to calling
  // CreateRenderFrame. In such a case, InitRenderView will not create the
  // RenderFrame in the renderer process and it needs to be done
  // explicitly.
  if (!InitRenderFrame(new_render_frame_host.get()))
    return nullptr;

  return new_render_frame_host;
}

void RenderFrameHostManager::CreateRenderFrameProxy(
    SiteInstance* instance,
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  CHECK(instance);
  TRACE_EVENT_INSTANT("navigation",
                      "RenderFrameHostManager::CreateRenderFrameProxy",
                      ChromeTrackEvent::kSiteInstance,
                      *static_cast<SiteInstanceImpl*>(instance),
                      ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  // If we are creating a proxy to recover from a crash and skipping the early
  // CommitPending then it could be in the same SiteInstance. In all other
  // cases we should be creating it in a different one.
  if (ShouldSkipEarlyCommitPendingForCrashedFrame()) {
    // TODO(fergal): We cannot put a CHECK in the else of this if because we do
    // not have enough information about who is calling this. If we knew it was
    // navigating then we could CHECK_EQ and CHECK_NE otherwise.
    if (!render_frame_host_->must_be_replaced())
      CHECK_NE(instance, render_frame_host_->GetSiteInstance());
  } else {
    // If policy allows early commit, a RenderFrameProxyHost should never be
    // created in the same SiteInstance as the current RFH.
    CHECK_NE(instance, render_frame_host_->GetSiteInstance());
  }

  // If a proxy already exists and is alive, nothing needs to be done.
  SiteInstanceGroup* group = static_cast<SiteInstanceImpl*>(instance)->group();
  RenderFrameProxyHost* proxy =
      browsing_context_state->GetRenderFrameProxyHost(group);
  if (proxy && proxy->is_render_frame_proxy_live())
    return;

  // At this point we know that we either have to 1) create a new
  // RenderFrameProxyHost or 2) revive an existing, but no longer alive
  // RenderFrameProxyHost.
  if (!proxy) {
    // The RenderViewHost creates the page level structure in Blink. The first
    // object to depend on it is necessarily a main frame one.
    scoped_refptr<RenderViewHostImpl> render_view_host =
        frame_tree_node_->frame_tree()->GetRenderViewHost(group);
    CHECK(render_view_host || frame_tree_node_->IsMainFrame());
    if (!render_view_host) {
      // Before creating a new RenderFrameProxyHost, ensure a RenderViewHost
      // exists for |instance|, as it creates the page level structure in Blink.
      render_view_host = frame_tree_node_->frame_tree()->CreateRenderViewHost(
          instance, /*main_frame_routing_id=*/MSG_ROUTING_NONE,
          /*swapped_out=*/true, /*renderer_initiated_creation=*/false,
          features::GetBrowsingContextMode() ==
                  features::BrowsingContextStateImplementationType::
                      kSwapForCrossBrowsingInstanceNavigations
              ? render_frame_host_->browsing_context_state()
              : nullptr);
    } else {
      TRACE_EVENT_INSTANT("navigation",
                          "RenderFrameHostManager::CreateRenderFrameProxy_RVH",
                          ChromeTrackEvent::kRenderViewHost, *render_view_host);
    }

    proxy = browsing_context_state->CreateRenderFrameProxyHost(
        instance, std::move(render_view_host), frame_tree_node_);
  }

  // Make sure that the RenderFrameProxy is present in the renderer.
  if (frame_tree_node_->IsMainFrame() && proxy->GetRenderViewHost()) {
    InitRenderView(group, proxy->GetRenderViewHost(), proxy);
  } else {
    proxy->InitRenderFrameProxy();
  }
}

void RenderFrameHostManager::CreateProxiesForChildFrame(FrameTreeNode* child) {
  TRACE_EVENT_INSTANT(
      "navigation", "RenderFrameHostManager::CreateProxiesForChildFrame_Parent",
      ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  TRACE_EVENT_INSTANT(
      "navigation", "RenderFrameHostManager::CreateProxiesForChildFrame_Child",
      ChromeTrackEvent::kFrameTreeNodeInfo, *child);
  RenderFrameProxyHost* outer_delegate_proxy =
      IsMainFrameForInnerDelegate() ? GetProxyToOuterDelegate() : nullptr;

  // Initial document in the child frame always belongs to the same SiteInstance
  // as its parent document, so we iterate over the proxies in the parent frame
  // to get a list of SiteInstances to create proxies in for in the child frame.
  DCHECK_EQ(render_frame_host_.get(), child->parent());

  for (const auto& pair :
       render_frame_host_->browsing_context_state()->proxy_hosts()) {
    TRACE_EVENT_INSTANT(
        "navigation",
        "RenderFrameHostManager::CreateProxiesForChildFrame_ProxyHost",
        ChromeTrackEvent::kRenderFrameProxyHost, *pair.second);
    // Do not create proxies for subframes in the outer delegate's process,
    // since the outer delegate does not need to interact with them.
    //
    // TODO(alexmos): This is potentially redundant with the
    // IsRelatedSiteInstance() check below.  Verify this and remove if so.
    if (pair.second.get() == outer_delegate_proxy)
      continue;

    // Do not create proxies for subframes for SiteInstances belonging to a
    // different BrowsingInstance.  This may happen when a main frame is
    // navigating across BrowsingInstances, and the current document adds a
    // subframe after that navigation starts but before it commits.  In that
    // time window, the main frame's FrameTreeNode would have a proxy in the
    // destination SiteInstance, but the current document's subframes shouldn't
    // create a proxy in the destination SiteInstance, since the new
    // BrowsingInstance need not know about them.  Not doing this used to
    // trigger inconsistencies and crashes if the old document was stored in
    // BackForwardCache and later restored (since this preserves all of the
    // subframe FrameTreeNodes and proxies).  See https://crbug.com/1243541.
    if (!pair.second->GetSiteInstance()->IsRelatedSiteInstance(
            render_frame_host_->GetSiteInstance())) {
      continue;
    }

    child->render_manager()->CreateRenderFrameProxy(
        pair.second->GetSiteInstance(),
        child->current_frame_host()->browsing_context_state());
  }
}

void RenderFrameHostManager::EnsureRenderViewInitialized(
    RenderViewHostImpl* render_view_host,
    SiteInstanceGroup* group) {
  DCHECK(frame_tree_node_->IsMainFrame());

  if (render_view_host->IsRenderViewLive())
    return;

  // If the proxy in |instance| doesn't exist, this RenderView is not swapped
  // out and shouldn't be reinitialized here.
  RenderFrameProxyHost* proxy =
      render_frame_host_->browsing_context_state()->GetRenderFrameProxyHost(
          group);
  if (!proxy)
    return;

  InitRenderView(group, render_view_host, proxy);
}

void RenderFrameHostManager::SwapOuterDelegateFrame(
    RenderFrameHostImpl* render_frame_host,
    RenderFrameProxyHost* proxy) {
  // Swap the outer WebContents's frame with the proxy to inner WebContents.
  //
  // We are in the outer WebContents, and its FrameTree would never see
  // a load start for any of its inner WebContents. Eventually, that also makes
  // the FrameTree never see the matching load stop. Therefore, we always pass
  // false to |is_loading| below.
  // TODO(lazyboy): This |is_loading| behavior might not be what we want,
  // investigate and fix.
  DCHECK_EQ(render_frame_host->GetSiteInstance()->group(),
            proxy->site_instance_group());
  render_frame_host->SwapOuterDelegateFrame(proxy);
  proxy->SetRenderFrameProxyCreated(true);
}

void RenderFrameHostManager::SetRWHViewForInnerFrameTree(
    RenderWidgetHostViewChildFrame* child_rwhv) {
  DCHECK(IsMainFrameForInnerDelegate());
  DCHECK(GetProxyToOuterDelegate());
  GetProxyToOuterDelegate()->SetChildRWHView(child_rwhv, nullptr);
}

bool RenderFrameHostManager::InitRenderView(
    SiteInstanceGroup* site_instance_group,
    RenderViewHostImpl* render_view_host,
    RenderFrameProxyHost* proxy) {
  // Ensure the renderer process is initialized before creating the
  // RenderView.
  if (!render_view_host->GetAgentSchedulingGroup().Init())
    return false;

  // We may have initialized this RenderViewHost for another RenderFrameHost.
  if (render_view_host->IsRenderViewLive())
    return true;

  auto opener_frame_token = GetOpenerFrameToken(site_instance_group);

  bool created = delegate_->CreateRenderViewForRenderManager(
      render_view_host, opener_frame_token, proxy);

  if (created && proxy) {
    proxy->SetRenderFrameProxyCreated(true);

    // If this main frame proxy was created for a frame that hasn't yet
    // finished loading, let the renderer know so it can also mark the proxy as
    // loading. See https://crbug.com/916137.
    if (frame_tree_node_->IsLoading())
      proxy->GetAssociatedRemoteFrame()->DidStartLoading();
  }

  return created;
}

scoped_refptr<SiteInstance>
RenderFrameHostManager::GetSiteInstanceForNavigationRequest(
    NavigationRequest* request,
    std::string* reason) {
  SiteInstance* current_site_instance = render_frame_host_->GetSiteInstance();

  // All children of MHTML documents must be MHTML documents. They all live in
  // the same process.
  if (request->IsForMhtmlSubframe()) {
    AppendReason(reason,
                 "GetSiteInstanceForNavigationRequest => current_site_instance"
                 " (IsForMhtmlSubframe)");
    return base::WrapRefCounted(current_site_instance);
  }

  // Srcdoc documents are only in the same SiteInstance as their parent if they
  // both have the same value for is_sandboxed(). They load their content from
  // the "srcdoc" iframe attribute which lives in the parent's process. Using
  // `GetParent()` is correct here because we never share BrowsingInstance /
  // SiteInstance across inner and outer frame tree.
  RenderFrameHostImpl* parent = render_frame_host_->GetParent();
  if (parent && request->common_params().url.IsAboutSrcdoc()) {
    const UrlInfo& url_info = request->GetUrlInfo();
    if (url_info.is_sandboxed &&
        !parent->GetSiteInstance()->GetSiteInfo().is_sandboxed()) {
      // TODO(wjmaclean); For now, SiteInfo::is_sandboxed() and
      // UrlInfo::is_sandboxed both mean "origin-restricted sandbox", so this
      // simple comparison suffices. But when we extend sandbox isolation to
      // depend on other sandbox flags as well, we may want to do a more
      // detailed comparison to make sure everything is compatible. E.g. if both
      // the parent and child are sandboxed, but with different flags, then we
      // may need separate SiteInstances, but that will be left for future CL.
      AppendReason(reason,
                   "GetSiteInstanceForNavigationRequest => compatible "
                   "sandboxed instance (IsAboutSrcdoc)");
      // In all the non-srcdoc cases we have a value for src and hence a UrlInfo
      // from which to build a SiteInfo for the sandboxed frame. But in the case
      // of a srcdoc iframe, we're basically picking a SiteInstance that is the
      // same as the parent frame, but with the `is_sandbox` flag set. srcdoc
      // iframes are normally considered to have the same origin as their
      // parents, so this seems reasonable.
      return parent->GetSiteInstance()->GetCompatibleSandboxedSiteInstance();
    }
    AppendReason(reason,
                 "GetSiteInstanceForNavigationRequest => parent-instance"
                 " (IsAboutSrcdoc)");
    return base::WrapRefCounted(parent->GetSiteInstance());
  }

  // Compute the SiteInstance that the navigation should use, which will be
  // either the current SiteInstance or a new one.
  //
  // TODO(clamy): We should also consider as a candidate SiteInstance the
  // speculative SiteInstance that was computed on redirects.
  SiteInstanceImpl* candidate_site_instance =
      speculative_render_frame_host_
          ? speculative_render_frame_host_->GetSiteInstance()
          : nullptr;

  // Accounts for all types of reloads, including renderer-initiated reloads.
  bool is_reload =
      NavigationTypeUtils::IsReload(request->common_params().navigation_type);

  scoped_refptr<SiteInstance> dest_site_instance = GetSiteInstanceForNavigation(
      request->GetUrlInfo(), request->GetSourceSiteInstance(),
      request->dest_site_instance(), candidate_site_instance,
      ui::PageTransitionFromInt(request->common_params().transition),
      request->state() >= NavigationRequest::CANCELING, is_reload,
      request->IsSameDocument(),
      request->GetRestoreType() == RestoreType::kRestored,
      request->commit_params().is_view_source, request->WasServerRedirect(),
      request->coop_status().require_browsing_instance_swap(),
      request->common_params().should_replace_current_entry,
      request->force_new_browsing_instance(), reason);

  TRACE_EVENT_INSTANT(
      "navigation",
      "RenderFrameHostManager::GetSiteInstanceForNavigationRequest_Result",
      ChromeTrackEvent::kSiteInstance,
      *static_cast<SiteInstanceImpl*>(dest_site_instance.get()),
      ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_,
      [&](perfetto::EventContext ctx) {
        auto rvh = frame_tree_node_->frame_tree()->GetRenderViewHost(
            static_cast<SiteInstanceImpl*>(dest_site_instance.get())->group());
        if (rvh) {
          auto* event = ctx.event<ChromeTrackEvent>();
          rvh->WriteIntoTrace(ctx.Wrap(event->set_render_view_host()));
        }
      });
  // If the NavigationRequest's dest_site_instance was present but incorrect,
  // then ensure no sensitive state is kept on the request. This can happen for
  // cross-process redirects, error pages, etc.
  if (request->dest_site_instance() &&
      request->dest_site_instance() != dest_site_instance) {
    request->ResetStateForSiteInstanceChange();
  }

  return dest_site_instance;
}

bool RenderFrameHostManager::InitRenderFrame(
    RenderFrameHostImpl* render_frame_host) {
  if (render_frame_host->IsRenderFrameLive())
    return true;

  SiteInstanceGroup* site_instance_group =
      render_frame_host->GetSiteInstance()->group();

  absl::optional<blink::FrameToken> opener_frame_token;
  if (frame_tree_node_->opener())
    opener_frame_token = GetOpenerFrameToken(site_instance_group);

  int parent_routing_id = MSG_ROUTING_NONE;
  if (frame_tree_node_->parent()) {
    parent_routing_id =
        frame_tree_node_->parent()
            ->frame_tree_node()
            ->render_manager()
            ->GetRoutingIdForSiteInstanceGroup(site_instance_group);
    CHECK_NE(parent_routing_id, MSG_ROUTING_NONE);
  }

  // At this point, all RenderFrameProxies for sibling frames have already been
  // created, including any proxies that come after this frame.  To preserve
  // correct order for indexed window access (e.g., window.frames[1]), pass the
  // previous sibling frame so that this frame is correctly inserted into the
  // frame tree on the renderer side.
  int previous_sibling_routing_id = MSG_ROUTING_NONE;
  FrameTreeNode* previous_sibling =
      frame_tree_node_->current_frame_host()->PreviousSibling();
  if (previous_sibling) {
    previous_sibling_routing_id =
        previous_sibling->render_manager()->GetRoutingIdForSiteInstanceGroup(
            site_instance_group);
    CHECK_NE(previous_sibling_routing_id, MSG_ROUTING_NONE);
  }

  RenderFrameProxyHost* existing_proxy =
      render_frame_host->browsing_context_state()->GetRenderFrameProxyHost(
          site_instance_group);
  if (existing_proxy && !existing_proxy->is_render_frame_proxy_live())
    existing_proxy->InitRenderFrameProxy();

  // Figure out the routing ID of the frame or proxy that this frame will
  // replace. This will usually will be |existing_proxy|'s routing ID, but
  // with RenderDocument it might also be a RenderFrameHost's routing ID.
  int previous_routing_id =
      GetReplacementRoutingId(existing_proxy, render_frame_host);

  return render_frame_host->CreateRenderFrame(
      previous_routing_id, opener_frame_token, parent_routing_id,
      previous_sibling_routing_id);
}

int RenderFrameHostManager::GetReplacementRoutingId(
    RenderFrameProxyHost* existing_proxy,
    RenderFrameHostImpl* render_frame_host) const {
  // Check whether there is an existing proxy for this frame in this
  // SiteInstance. If there is, the new RenderFrame needs to be able to find
  // the proxy it is replacing, so that it can fully initialize itself.
  // NOTE: This is the only time that a RenderFrameProxyHost can be in the same
  // SiteInstance as its RenderFrameHost. This is only the case until the
  // RenderFrameHost commits, at which point it will replace and delete the
  // RenderFrameProxyHost.
  if (existing_proxy) {
    // We are navigating cross-SiteInstance in a main frame or subframe.
    int proxy_routing_id = existing_proxy->GetRoutingID();
    CHECK_NE(proxy_routing_id, MSG_ROUTING_NONE);
    return proxy_routing_id;
  } else {
    // No proxy means that this is one of:
    // - a same-SiteInstance subframe navigation
    // - a cross-SiteInstance navigation from a crashed subframe that will do an
    //   early commit and the SiteInstance is not already in the frame tree.
    // A main frame navigation with no proxy would have its RenderFrame init
    // handled by InitRenderView. This will change with RenderDocument for main
    // frames.
    DCHECK(frame_tree_node_->parent());
    if (current_frame_host()->IsRenderFrameLive()) {
      CHECK_EQ(render_frame_host->GetSiteInstance(),
               current_frame_host()->GetSiteInstance());
      // The new frame will replace an existing frame in the renderer. For now
      // this can only be when RenderDocument-subframe is enabled.
      DCHECK(ShouldCreateNewHostForSameSiteSubframe());
      DCHECK_NE(render_frame_host, current_frame_host());
      return current_frame_host()->GetRoutingID();
    } else {
      // The renderer crashed and there is no previous proxy or previous frame
      // in the renderer to be replaced.
      DCHECK(current_frame_host()->must_be_replaced());
      DCHECK_NE(render_frame_host, current_frame_host());
      return MSG_ROUTING_NONE;
    }
  }
}

bool RenderFrameHostManager::ReinitializeMainRenderFrame(
    RenderFrameHostImpl* render_frame_host) {
  CHECK(!frame_tree_node_->parent());

  // This should be used only when the RenderFrame is not live.
  DCHECK(!render_frame_host->IsRenderFrameLive());
  DCHECK(!render_frame_host->must_be_replaced());

  // Recreate the opener chain.
  CreateOpenerProxies(render_frame_host->GetSiteInstance(), frame_tree_node_,
                      render_frame_host_->browsing_context_state());

  // Main frames need both the RenderView and RenderFrame reinitialized, so
  // use InitRenderView.
  DCHECK(!render_frame_host->browsing_context_state()->GetRenderFrameProxyHost(
      render_frame_host->GetSiteInstance()->group()));
  if (!InitRenderView(render_frame_host->GetSiteInstance()->group(),
                      render_frame_host->render_view_host(), nullptr))
    return false;

  DCHECK(render_frame_host->IsRenderFrameLive());

  // The RenderWidgetHostView goes away with the render process. Initializing a
  // RenderFrame means we'll be creating (or reusing, https://crbug.com/419087)
  // a RenderWidgetHostView. The new RenderWidgetHostView should take its
  // visibility from the RenderWidgetHostImpl, but this call exists to handle
  // cases where it did not during a same-process navigation.
  // TODO(danakj): We now hide the widget unconditionally (treating main frame
  // and child frames alike) and show in DidFinishNavigation() always, so this
  // should be able to go away. Try to remove this.
  if (render_frame_host == render_frame_host_.get())
    EnsureRenderFrameHostVisibilityConsistent();

  return true;
}

int RenderFrameHostManager::GetRoutingIdForSiteInstanceGroup(
    SiteInstanceGroup* site_instance_group) {
  if (render_frame_host_->GetSiteInstance()->group() == site_instance_group)
    return render_frame_host_->GetRoutingID();

  RenderFrameProxyHost* proxy =
      render_frame_host_->browsing_context_state()->GetRenderFrameProxyHost(
          site_instance_group);
  if (proxy)
    return proxy->GetRoutingID();

  return MSG_ROUTING_NONE;
}

absl::optional<blink::FrameToken>
RenderFrameHostManager::GetFrameTokenForSiteInstanceGroup(
    SiteInstanceGroup* site_instance_group) {
  // We want to ensure that we don't create proxies for the new speculative site
  // instance after a browsing instance swap, and we want to ensure that this
  // doesn't break anything, so we tie it to the GetBrowsingContextMode which
  // needs it and is disabled-by-default)
  if (features::GetBrowsingContextMode() ==
          features::BrowsingContextStateImplementationType::
              kSwapForCrossBrowsingInstanceNavigations &&
      render_frame_host_->GetSiteInstance()->group()->browsing_instance_id() !=
          site_instance_group->browsing_instance_id()) {
    return absl::nullopt;
  }
  if (render_frame_host_->GetSiteInstance()->group() == site_instance_group)
    return render_frame_host_->GetFrameToken();

  RenderFrameProxyHost* proxy =
      render_frame_host_->browsing_context_state()->GetRenderFrameProxyHost(
          site_instance_group);
  if (proxy)
    return proxy->GetFrameToken();

  return absl::nullopt;
}

void RenderFrameHostManager::CommitPending(
    std::unique_ptr<RenderFrameHostImpl> pending_rfh,
    std::unique_ptr<StoredPage> pending_stored_page,
    bool clear_proxies_on_commit) {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::CommitPending",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());
  CHECK(pending_rfh);
  // We either come here with a `pending_rfh` that is
  // 1) a speculative RenderFrameHost, which would have been deleted
  //    immediately upon renderer process exit, so it must still have a live
  //    connection to its renderer frame.
  // 2) a current RenderFrameHost which has just received a commit IPC from the
  //    renderer, so it must have a live connection to its renderer frame in
  //    order to receive the IPC.
  DCHECK(pending_rfh->IsRenderFrameCreated());

  // We should not have a pending bfcache entry unless bfcache or prerendering
  // is enabled. Note that in prerendering, the prerendering page information is
  // stored in `pending_stored_page` prior to activating the page
  // (despite the "bfcache" name).
  DCHECK(!pending_stored_page || IsBackForwardCacheEnabled() ||
         blink::features::IsPrerender2Enabled());

#if BUILDFLAG(IS_MAC)
  // The old RenderWidgetHostView will be hidden before the new
  // RenderWidgetHostView takes its contents. Ensure that Cocoa sees this as
  // a single transaction.
  // https://crbug.com/829523
  // TODO(ccameron): This can be removed when the RenderWidgetHostViewMac uses
  // the same ui::Compositor as MacViews.
  // https://crbug.com/331669
  gfx::ScopedCocoaDisableScreenUpdates disabler;
#endif  // BUILDFLAG(IS_MAC)

  RenderWidgetHostView* old_view = render_frame_host_->GetView();
  bool is_main_frame = frame_tree_node_->IsMainFrame();

  // Remember if the page was focused so we can focus the new renderer in
  // that case.
  bool focus_render_view =
      old_view && old_view->HasFocus() &&
      render_frame_host_->GetMainFrame()->GetRenderWidgetHost()->is_focused();

  // Remove the current frame and its descendants from the set of fullscreen
  // frames immediately. They can stay in pending deletion for some time.
  // Removing them when they are deleted is too late.
  // This needs to be done before updating the frame tree structure, else it
  // will have trouble removing the descendants.
  frame_tree_node_->frame_tree()
      ->render_frame_delegate()
      ->FullscreenStateChanged(current_frame_host(), false,
                               blink::mojom::FullscreenOptionsPtr());

  // If the removed frame was created by a script, then its history entry will
  // never be reused - we can save some memory by removing the history entry.
  // See also https://crbug.com/784356.
  // This is done in ~FrameTreeNode, but this is needed here as well. For
  // instance if the user navigates from A(B) to C and B is deleted after C
  // commits, then the last committed navigation entry wouldn't match anymore.
  NavigationEntryImpl* navigation_entry =
      GetNavigationController().GetLastCommittedEntry();
  if (navigation_entry) {
    frame_tree_node_->PruneChildFrameNavigationEntries(navigation_entry);
  }

  // If we navigate to an existing page (i.e. |pending_stored_page| is not
  // null), check that |pending_rfh|'s old lifecycle state supports that.
  RenderFrameHostImpl::LifecycleStateImpl prev_state =
      pending_rfh->lifecycle_state();
  DCHECK(!pending_stored_page ||
         prev_state == RenderFrameHostImpl::LifecycleStateImpl::kPrerendering ||
         prev_state ==
             RenderFrameHostImpl::LifecycleStateImpl::kInBackForwardCache);

  // Swap in the new frame and make it active. Also ensure the FrameTree
  // stays in sync.
  std::unique_ptr<RenderFrameHostImpl> old_render_frame_host;
  old_render_frame_host = SetRenderFrameHost(std::move(pending_rfh));

  // If a document is being restored from the BackForwardCache or is being
  // activated from Prerendering, restore all cached state now.
  if (pending_stored_page) {
    // This is only implemented for the legacy mode of BrowsingContextState
    // because in the new implementation, proxies will be swapped/restored
    // whenever the RenderFrameHost (and internal BrowsingContextState) is
    // restored.
    if (features::GetBrowsingContextMode() ==
        features::BrowsingContextStateImplementationType::
            kLegacyOneToOneWithFrameTreeNode) {
      BrowsingContextState::RenderFrameProxyHostMap proxy_hosts_to_restore =
          std::move(pending_stored_page->proxy_hosts);
      for (auto& proxy : proxy_hosts_to_restore) {
        // We only cache pages when swapping BrowsingInstance, so we should
        // never be reusing SiteInstanceGroups.
        CHECK(!base::Contains(
            render_frame_host_->browsing_context_state()->proxy_hosts(),
            proxy.second->site_instance_group()->GetId()));
        proxy.second->site_instance_group()->AddObserver(
            render_frame_host_->browsing_context_state().get());
        TRACE_EVENT_INSTANT(
            "navigation", "RenderFrameHostManager::CommitPending_RestoreProxy",
            ChromeTrackEvent::kRenderFrameProxyHost, *proxy.second);
        render_frame_host_->browsing_context_state()->proxy_hosts().insert(
            std::move(proxy));
      }
    }

    std::set<RenderViewHostImpl*> render_view_hosts_to_restore =
        std::move(pending_stored_page->render_view_hosts);
    if (prev_state ==
        RenderFrameHostImpl::LifecycleStateImpl::kInBackForwardCache) {
      blink::RecordUMAEventPageShowPersisted(
          blink::EventPageShowPersisted::
              kYesInBrowser_RenderFrameHostManager_CommitPending);
      for (RenderViewHostImpl* rvh : render_view_hosts_to_restore) {
        bool restoring_main_frame_from_back_forward_cache =
            render_frame_host_->render_view_host() == rvh;
        rvh->LeaveBackForwardCache(
            pending_stored_page->page_restore_params.Clone(),
            restoring_main_frame_from_back_forward_cache);
      }
    } else {
      DCHECK_EQ(prev_state,
                RenderFrameHostImpl::LifecycleStateImpl::kPrerendering);
      current_frame_host()->GetPage().ActivateForPrerendering(
          render_view_hosts_to_restore);
    }
  }

  // For all main frames, the RenderWidgetHost will not be destroyed when the
  // local frame is detached. https://crbug.com/419087
  //
  // The RenderWidget in the renderer process is destroyed, but the
  // RenderWidgetHost and RenderWidgetHostView are still kept alive for a remote
  // main frame.
  //
  // To work around that, we hide it here. Truly this is to hit all the hide
  // paths in the browser side, but has a side effect of also hiding the
  // renderer side RenderWidget, even though it will get frozen anyway in the
  // future. However freezing doesn't do all the things hiding does at this time
  // so that's probably good.
  //
  // Note the RenderWidgetHostView can be missing if the process for the old
  // RenderFrameHost crashed.
  //
  // TODO(crbug.com/419087): This is only done for the main frame, as for sub
  // frames the RenderWidgetHost and its view will be destroyed when the frame
  // is detached, but for the main frame it is not. This call to Hide() can go
  // away when the main frame's RenderWidgetHost is destroyed on frame detach.
  // Note that calling this on a subframe that is not a local root would be
  // incorrect as it would hide an ancestor local root's RenderWidget when that
  // frame is not necessarily navigating. Removing this Hide() has previously
  // been attempted without success in r426913 (https://crbug.com/658688) and
  // r438516 (broke assumptions about RenderWidgetHosts not changing
  // RenderWidgetHostViews over time).
  //
  // |old_rvh| and |new_rvh| can be the same when navigating same-site from a
  // crashed RenderFrameHost. When RenderDocument will be implemented, this will
  // happen for each same-site navigation.
  RenderViewHostImpl* old_rvh = old_render_frame_host->render_view_host();
  RenderViewHostImpl* new_rvh = render_frame_host_->render_view_host();
  if (is_main_frame && old_view && old_rvh != new_rvh) {
    // Note that this hides the RenderWidget but does not hide the Page. If it
    // did hide the Page then making a new RenderFrameHost on another call to
    // here would need to make sure it showed the RenderView when the
    // RenderWidget was created as visible.
    old_view->Hide();
  }

  RenderWidgetHostView* new_view = render_frame_host_->GetView();
  // Since the committing renderer frame is live, the RenderWidgetHostView must
  // also exist. For a local root frame, they share lifetimes exactly. For
  // another child frame, the RenderWidgetHostView comes from a parent, but if
  // this renderer frame is live its ancestors must be as well.
  DCHECK(new_view);

  if (focus_render_view) {
    if (is_main_frame) {
      new_view->Focus();
    } else {
      // The current WebContents has page-level focus, so we need to propagate
      // page-level focus to the subframe's renderer. Before doing that, also
      // tell the new renderer what the focused frame is if that frame is not
      // in its process, so that Blink's page-level focus logic won't try to
      // reset frame focus to the main frame.  See https://crbug.com/802156.
      // TODO(https://crbug.com/1261963, yangsharon): Cover the case where
      // different SiteInstances are in the same SiteInstanceGroup, and thus
      // don't have a proxy. GetRenderFrameProxyHost below should not be called
      // in that case.
      FrameTreeNode* focused_frame =
          frame_tree_node_->frame_tree()->GetFocusedFrame();
      if (focused_frame && !focused_frame->IsMainFrame() &&
          focused_frame->current_frame_host()->GetSiteInstance() !=
              render_frame_host_->GetSiteInstance()) {
        focused_frame->GetBrowsingContextStateForSubframe()
            ->GetRenderFrameProxyHost(
                render_frame_host_->GetSiteInstance()->group())
            ->SetFocusedFrame();
      }
      frame_tree_node_->frame_tree()->SetPageFocus(
          render_frame_host_->GetSiteInstance()->group(), true);
    }
  }

  // Notify that we have no `old_view` from which to TakeFallbackContentFrom.
  // This will clear the current Fallback Surface, which would be from a
  // previous Navigation. This way we do not display old content if this new
  // PendingCommit does not lead to a successful Navigation. This must be called
  // before NotifySwappedFromRenderManager, which will allocate a new
  // viz::LocalSurfaceId, which will allow the Renderer to submit new content.
  // TODO(crbug.com/1072817): Remove this once CommitPending has more explicit
  // shutdown, both for successful and failed navigations.
  if (!old_view) {
    delegate_->NotifySwappedFromRenderManagerWithoutFallbackContent(
        render_frame_host_.get());
  }

  // Notify that we've swapped RenderFrameHosts. We do this before shutting down
  // the RFH so that we can clean up RendererResources related to the RFH first.
  delegate_->NotifySwappedFromRenderManager(old_render_frame_host.get(),
                                            render_frame_host_.get());

  // Make the new view show the contents of old view until it has something
  // useful to show.
  if (is_main_frame && old_view && old_view != new_view)
    new_view->TakeFallbackContentFrom(old_view);

  // The RenderViewHost keeps track of the main RenderFrameHost routing id.
  // If this is committing a main frame navigation, update it and set the
  // routing id in the RenderViewHost associated with the old RenderFrameHost
  // to MSG_ROUTING_NONE.
  if (is_main_frame) {
    // If the RenderViewHost is transitioning from an inactive to active state,
    // it was reused, so dispatch a RenderViewReady event. For example, this is
    // necessary to hide the sad tab if one is currently displayed. See
    // https://crbug.com/591984.
    //
    // Note that observers of RenderViewReady() will see the updated main frame
    // routing ID, since PostRenderViewReady() posts a task.
    //
    // TODO(alexmos): Remove this and move RenderViewReady consumers to use
    // the main frame's RenderFrameCreated instead.
    if (!new_rvh->is_active())
      new_rvh->PostRenderViewReady();

    new_rvh->SetMainFrameRoutingId(render_frame_host_->GetRoutingID());
    if (old_rvh != new_rvh)
      old_rvh->SetMainFrameRoutingId(MSG_ROUTING_NONE);
  }

  // Store the old_render_frame_host's current frame size so that it can be used
  // to initialize the child RWHV.
  absl::optional<gfx::Size> old_size = old_render_frame_host->frame_size();

  // Store the old_render_frame_host's BrowsingContextState so that it can be
  // used to update/delete proxies.
  scoped_refptr<BrowsingContextState> old_browsing_context_state =
      old_render_frame_host->browsing_context_state();

  // Unload the old frame now that the new one is visible.
  // This will unload it and schedule it for deletion when the unload ack
  // arrives (or immediately if the process isn't live).
  UnloadOldFrame(std::move(old_render_frame_host));

  // Since the new RenderFrameHost is now committed, there must be no proxies
  // for its SiteInstance. Delete any existing ones.
  render_frame_host_->browsing_context_state()->DeleteRenderFrameProxyHost(
      render_frame_host_->GetSiteInstance()->group());

  // If this is a top-level frame, and COOP triggered a BrowsingInstance swap,
  // make sure all relationships with the previous BrowsingInstance are severed
  // by removing the opener, the openee's opener, and the proxies with unrelated
  // SiteInstances.
  // TODO(crbug.com/1270671): Make this a no-op for the non-legacy
  // implementation of BrowsingContextState.
  if (clear_proxies_on_commit) {
    TRACE_EVENT("navigation",
                "RenderFrameHostManager::CommitPending_ClearProxiesOnCommit");
    DCHECK(frame_tree_node_->IsMainFrame());

    // If this frame has opened popups, we need to clear the opened popup's
    // opener. This is done here on the browser side. A similar mechanism occurs
    // in the renderer process when the RenderView of this frame is destroyed,
    // via blink::OpenedFrameTracker.
    frame_tree_node_->ClearOpenerReferences();

    // We've just cleared other frames' "opener" referencing this frame, we now
    // clear this frame's "opener".
    if (frame_tree_node_->opener() &&
        !render_frame_host_->GetSiteInstance()->IsRelatedSiteInstance(
            frame_tree_node_->opener()
                ->current_frame_host()
                ->GetSiteInstance())) {
      frame_tree_node_->SetOpener(nullptr);
      // Note: It usually makes sense to notify the proxies of that frame that
      // the opener was removed. However since these proxies are destroyed right
      // after it is not necessary in this particuliar case.
    }

    // Now that opener references are gone in both direction, we can clear the
    // underlying proxies that were used for that purpose.
    std::vector<RenderFrameProxyHost*> removed_proxies;
    for (auto& it :
         render_frame_host_->browsing_context_state()->proxy_hosts()) {
      const auto& proxy = it.second;
      // The outer delegate proxy is *always* cross-browsing context group, but
      // it is the only proxy we must preserve.
      if (!render_frame_host_->GetSiteInstance()->IsRelatedSiteInstance(
              proxy->GetSiteInstance()) &&
          proxy.get() != GetProxyToOuterDelegate()) {
        removed_proxies.push_back(proxy.get());
      }
    }

    TRACE_EVENT("navigation",
                "RenderFrameHostManager::CommitPending_"
                "DeleteProxiesFromOldBrowsingContextState",
                ChromeTrackEvent::kBrowsingContextState,
                old_browsing_context_state);
    for (auto* proxy : removed_proxies) {
      old_browsing_context_state->DeleteRenderFrameProxyHost(
          proxy->site_instance_group());
    }
  }

  // If this is a subframe or inner frame tree, it should have a
  // CrossProcessFrameConnector created already.  Use it to link the new RFH's
  // view to the proxy that belongs to the parent frame's SiteInstance. If this
  // navigation causes an out-of-process frame to return to the same process as
  // its parent, the proxy would have been removed from
  // render_frame_host_->browsing_context_state()->proxy_hosts() above.
  // Note: We do this after unloading the old RFH because that may create
  // the proxy we're looking for.
  RenderFrameProxyHost* proxy_to_parent_or_outer_delegate =
      GetProxyToParentOrOuterDelegate();
  if (proxy_to_parent_or_outer_delegate) {
    proxy_to_parent_or_outer_delegate->SetChildRWHView(
        static_cast<RenderWidgetHostViewChildFrame*>(new_view),
        old_size ? &*old_size : nullptr);
  }

  if (render_frame_host_->is_local_root()) {
    // RenderFrames are created with a hidden RenderWidgetHost. When navigation
    // finishes, we show it if the delegate is shown.
    if (!frame_tree_node_->frame_tree()->IsHidden())
      new_view->Show();
  }

  // The process will no longer try to exit, so we can decrement the count.
  render_frame_host_->GetProcess()->RemovePendingView();

  // After all is done, there must never be a proxy in the list which has the
  // same SiteInstanceGroup as the current RenderFrameHost.
  CHECK(!render_frame_host_->browsing_context_state()->GetRenderFrameProxyHost(
      render_frame_host_->GetSiteInstance()->group()));
}

std::unique_ptr<RenderFrameHostImpl> RenderFrameHostManager::SetRenderFrameHost(
    std::unique_ptr<RenderFrameHostImpl> render_frame_host) {
  // Swap the two.
  std::unique_ptr<RenderFrameHostImpl> old_render_frame_host =
      std::move(render_frame_host_);
  render_frame_host_ = std::move(render_frame_host);

  FrameTree* frame_tree = frame_tree_node_->frame_tree();

  // Swapping the current RenderFrameHost in a FrameTreeNode comes along with an
  // update to its LifecycleStateImpl.

  // The lifecycle state of the old RenderFrameHost is either:
  // - kActive: starts unloading or enters the BackForwardCache.
  // - kPrerendering: starts unloading.

  // The lifecycle state of the new RenderFrameHost is either:
  // - kSpeculative: for early-commit navigations (see
  //   https://crbug.com/1072817) and when attaching an inner delegate (when
  //   embedding one WebContents inside another).
  // - kPendingCommit: for regular cross-RenderFrameHost navigations.
  // - kBackForwardCache: for BackForwardCache restore navigation.
  // - kPrerendering: for a prerender activation navigation.
  // It should become kActive in the primary frame tree and kPrerendering for
  // navigations inside the prerendered frame tree.

  // Note that Prerender2 introduces the concept of a prerendered frame tree.
  // It also allows navigations within the prerendered tree to enable loading
  // and running pages while in the background. Here, the old RenderFrameHost's
  // state isn't kActive, but kPrerendering. The new RenderFrameHost doesn't
  // become kActive, but kPrerendering because documents in kPrerendering state
  // are considered current in the prerendered frame tree and invisible to the
  // user, unlike kActive state.
  if (render_frame_host_) {
    if (frame_tree->is_prerendering()) {
      if (render_frame_host_->lifecycle_state() ==
          LifecycleStateImpl::kPendingCommit) {
        render_frame_host_->SetLifecycleState(
            LifecycleStateImpl::kPrerendering);
      }
    } else {
      if (render_frame_host_->lifecycle_state() != LifecycleStateImpl::kActive)
        render_frame_host_->SetLifecycleState(LifecycleStateImpl::kActive);
    }
  }

  // Note that we don't know yet what the next state will be, so it is
  // temporarily marked with SetHasPendingLifecycleStateUpdate().
  // TODO(https://crbug.com/1182237): Determine the next state earlier and
  // remove SetHasPendingLifecycleStateUpdate().
  if (old_render_frame_host && !old_render_frame_host->IsPendingDeletion()) {
    // After the old RenderFrameHost is no longer the current one, set the value
    // of |has_pending_lifecycle_state_update_| to true if it is not null.
    old_render_frame_host->SetHasPendingLifecycleStateUpdate();
  }

  if (frame_tree_node_->IsMainFrame()) {
    // Update the count of top-level frames using this SiteInstance.  All
    // subframes are in the same BrowsingInstance as the main frame, so we only
    // count top-level ones.  This makes the value easier for consumers to
    // interpret.
    if (render_frame_host_) {
      render_frame_host_->GetSiteInstance()
          ->IncrementRelatedActiveContentsCount();
    }
    if (old_render_frame_host) {
      old_render_frame_host->GetSiteInstance()
          ->DecrementRelatedActiveContentsCount();
    }
  }

  return old_render_frame_host;
}

void RenderFrameHostManager::CollectOpenerFrameTrees(
    std::vector<FrameTree*>* opener_frame_trees,
    std::unordered_set<FrameTreeNode*>* nodes_with_back_links) {
  CHECK(opener_frame_trees);
  opener_frame_trees->push_back(frame_tree_node_->frame_tree());

  // Add the FrameTree of the given node's opener to the list of
  // |opener_frame_trees| if it doesn't exist there already. |visited_index|
  // indicates which FrameTrees in |opener_frame_trees| have already been
  // visited (i.e., those at indices less than |visited_index|).
  // |nodes_with_back_links| collects FrameTreeNodes with openers in FrameTrees
  // that have already been visited (such as those with cycles).
  size_t visited_index = 0;
  while (visited_index < opener_frame_trees->size()) {
    FrameTree* frame_tree = (*opener_frame_trees)[visited_index];
    visited_index++;
    for (FrameTreeNode* node : frame_tree->Nodes()) {
      if (!node->opener())
        continue;

      FrameTree* opener_tree = node->opener()->frame_tree();
      const auto& existing_tree_it = std::find(
          opener_frame_trees->begin(), opener_frame_trees->end(), opener_tree);

      if (existing_tree_it == opener_frame_trees->end()) {
        // This is a new opener tree that we will need to process.
        opener_frame_trees->push_back(opener_tree);
      } else {
        // If this tree is already on our processing list *and* we have visited
        // it,
        // then this node's opener is a back link.  This means the node will
        // need
        // special treatment to process its opener.
        size_t position =
            std::distance(opener_frame_trees->begin(), existing_tree_it);
        if (position < visited_index)
          nodes_with_back_links->insert(node);
      }
    }
  }
}

void RenderFrameHostManager::CreateOpenerProxies(
    SiteInstance* instance,
    FrameTreeNode* skip_this_node,
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  // TODO(crbug.com/1270671): Add a DCHECK verifying that |instance
  // is a related site instance to the site instance in |render_frame_host_|. At
  // the moment, this DCHECK fails due to a bug in choosing SiteInstance in
  // web_contents_impl.cc.
  std::vector<FrameTree*> opener_frame_trees;
  std::unordered_set<FrameTreeNode*> nodes_with_back_links;

  CollectOpenerFrameTrees(&opener_frame_trees, &nodes_with_back_links);

  // Create opener proxies for frame trees, processing furthest openers from
  // this node first and this node last.  In the common case without cycles,
  // this will ensure that each tree's openers are created before the tree's
  // nodes need to reference them.
  for (FrameTree* tree : base::Reversed(opener_frame_trees)) {
    tree->root()->render_manager()->CreateOpenerProxiesForFrameTree(
        instance, skip_this_node, browsing_context_state);
  }

  // Set openers for nodes in |nodes_with_back_links| in a second pass.
  // The proxies created at these FrameTreeNodes in
  // CreateOpenerProxiesForFrameTree won't have their opener routing ID
  // available when created due to cycles or back links in the opener chain.
  // They must have their openers updated as a separate step after proxy
  // creation.
  for (auto* node : nodes_with_back_links) {
    RenderFrameProxyHost* proxy =
        node->render_manager()
            ->current_frame_host()
            ->browsing_context_state()
            ->GetRenderFrameProxyHost(
                static_cast<SiteInstanceImpl*>(instance)->group());
    // If there is no proxy, the cycle may involve nodes in the same process,
    // or, if this is a subframe, --site-per-process may be off.  Either way,
    // there's nothing more to do.
    if (!proxy)
      continue;

    auto opener_frame_token = node->render_manager()->GetOpenerFrameToken(
        static_cast<SiteInstanceImpl*>(instance)->group());
    DCHECK(opener_frame_token);
    proxy->GetAssociatedRemoteFrame()->UpdateOpener(opener_frame_token);
  }
}

void RenderFrameHostManager::CreateOpenerProxiesForFrameTree(
    SiteInstance* instance,
    FrameTreeNode* skip_this_node,
    const scoped_refptr<BrowsingContextState>& browsing_context_state) {
  // Currently, this function is only called on main frames.  It should
  // actually work correctly for subframes as well, so if that need ever
  // arises, it should be sufficient to remove this DCHECK.
  DCHECK(frame_tree_node_->IsMainFrame());

  FrameTree* frame_tree = frame_tree_node_->frame_tree();

  // Ensure that all the nodes in the opener's FrameTree have
  // RenderFrameProxyHosts for the new SiteInstance.  Only pass the node to
  // be skipped if it's in the same FrameTree.
  if (skip_this_node && skip_this_node->frame_tree() != frame_tree)
    skip_this_node = nullptr;
  frame_tree->CreateProxiesForSiteInstance(skip_this_node, instance,
                                           browsing_context_state);
}

absl::optional<blink::FrameToken> RenderFrameHostManager::GetOpenerFrameToken(
    SiteInstanceGroup* group) {
  if (!frame_tree_node_->opener())
    return absl::nullopt;

  return frame_tree_node_->opener()
      ->render_manager()
      ->GetFrameTokenForSiteInstanceGroup(group);
}

void RenderFrameHostManager::ExecutePageBroadcastMethod(
    PageBroadcastMethodCallback callback,
    SiteInstance* instance_to_skip) {
  // TODO(dcheng): Now that RenderView and RenderWidget are increasingly
  // separated, it might be possible/desirable to just route to the view.
  DCHECK(!frame_tree_node_->parent());

  // When calling a PageBroadcast Mojo method for an inner WebContents, we don't
  // want to also call it for the outer WebContent's frame as well.
  RenderFrameProxyHost* outer_delegate_proxy =
      IsMainFrameForInnerDelegate() ? GetProxyToOuterDelegate() : nullptr;
  for (const auto& pair :
       render_frame_host_->browsing_context_state()->proxy_hosts()) {
    if (outer_delegate_proxy == pair.second.get())
      continue;
    if (pair.second->GetSiteInstance() == instance_to_skip)
      continue;
    callback.Run(pair.second->GetRenderViewHost());
  }

  if (speculative_render_frame_host_ &&
      speculative_render_frame_host_->GetSiteInstance() != instance_to_skip) {
    callback.Run(speculative_render_frame_host_->render_view_host());
  }

  if (render_frame_host_->GetSiteInstance() != instance_to_skip) {
    callback.Run(render_frame_host_->render_view_host());
  }
}

void RenderFrameHostManager::ExecuteRemoteFramesBroadcastMethod(
    RemoteFramesBroadcastMethodCallback callback,
    SiteInstance* instance_to_skip) {
  DCHECK(!frame_tree_node_->parent());

  // When calling a ExecuteRemoteFramesBroadcastMethod() for an inner
  // WebContents, we don't want to also call it for the outer WebContent's
  // frame as well.
  RenderFrameProxyHost* outer_delegate_proxy =
      IsMainFrameForInnerDelegate() ? GetProxyToOuterDelegate() : nullptr;
  render_frame_host_->browsing_context_state()
      ->ExecuteRemoteFramesBroadcastMethod(callback, instance_to_skip,
                                           outer_delegate_proxy);
}

void RenderFrameHostManager::EnsureRenderFrameHostVisibilityConsistent() {
  RenderWidgetHostView* view = GetRenderWidgetHostView();
  if (view &&
      static_cast<RenderWidgetHostImpl*>(view->GetRenderWidgetHost())
              ->is_hidden() != frame_tree_node_->frame_tree()->IsHidden()) {
    if (frame_tree_node_->frame_tree()->IsHidden()) {
      view->Hide();
    } else {
      view->Show();
    }
  }
}

void RenderFrameHostManager::EnsureRenderFrameHostPageFocusConsistent() {
  frame_tree_node_->frame_tree()->SetPageFocus(
      render_frame_host_->GetSiteInstance()->group(),
      frame_tree_node_->frame_tree()
          ->root()
          ->current_frame_host()
          ->GetRenderWidgetHost()
          ->is_focused());
}

void RenderFrameHostManager::CreateNewFrameForInnerDelegateAttachIfNecessary() {
  TRACE_EVENT(
      "navigation",
      "RenderFrameHostManager::CreateNewFrameForInnerDelegateAttachIfNecessary",
      ChromeTrackEvent::kFrameTreeNodeInfo, *frame_tree_node_);
  DCHECK(is_attaching_inner_delegate());
  // Remove all navigations and any speculative frames which might interfere
  // with the loading state.
  current_frame_host()->ResetNavigationRequests();
  current_frame_host()->ResetLoadingState();
  // Remove any speculative frames first and ongoing navigation state. This
  // should reset the loading state for good.
  frame_tree_node_->ResetNavigationRequest(false /* keep_state */);
  if (speculative_render_frame_host_) {
    // The FrameTreeNode::ResetNavigationRequest call above may not have cleaned
    // up the speculative RenderFrameHost if the NavigationRequest had already
    // been transferred to RenderFrameHost.  Ensure it is cleaned up now.
    DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());
  }

  DCHECK(!current_frame_host()->is_main_frame());
  if (current_frame_host()->GetSiteInstance() ==
      current_frame_host()->GetParent()->GetSiteInstance()) {
    // At this point the beforeunload is dispatched and the result has been to
    // proceed with attaching. There are also no upcoming navigations which
    // would interfere with the upcoming attach. If the frame is in the same
    // SiteInstance as its parent it can be safely used for attaching an inner
    // Delegate.
    NotifyPrepareForInnerDelegateAttachComplete(true /* success */);
    return;
  }

  // We need a new RenderFrameHost in its parent's SiteInstance to be able to
  // safely use the WebContentsImpl attach API.
  DCHECK(!speculative_render_frame_host_);
  if (!CreateSpeculativeRenderFrameHost(
          current_frame_host()->GetSiteInstance(),
          current_frame_host()->GetParent()->GetSiteInstance(),
          /*recovering_without_early_commit=*/false)) {
    NotifyPrepareForInnerDelegateAttachComplete(false /* success */);
    return;
  }
  // Swap in the speculative frame. It will later be replaced when
  // WebContents::AttachToOuterWebContentsFrame is called.
  speculative_render_frame_host_->SwapIn();
  CommitPending(std::move(speculative_render_frame_host_), nullptr,
                false /* clear_proxies_on_commit */);
  NotifyPrepareForInnerDelegateAttachComplete(true /* success */);
}

void RenderFrameHostManager::NotifyPrepareForInnerDelegateAttachComplete(
    bool success) {
  DCHECK(is_attaching_inner_delegate());
  int32_t process_id = success ? render_frame_host_->GetProcess()->GetID()
                               : ChildProcessHost::kInvalidUniqueID;
  int32_t routing_id =
      success ? render_frame_host_->GetRoutingID() : MSG_ROUTING_NONE;
  // Invoking the callback asynchronously to meet the APIs promise.
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](RenderFrameHost::PrepareForInnerWebContentsAttachCallback callback,
             int32_t process_id, int32_t routing_id) {
            std::move(callback).Run(
                RenderFrameHostImpl::FromID(process_id, routing_id));
          },
          std::move(attach_inner_delegate_callback_), process_id, routing_id));
}

NavigationControllerImpl& RenderFrameHostManager::GetNavigationController() {
  return frame_tree_node_->frame_tree()->controller();
}

}  // namespace content
