// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/app_window/app_window_contents.h"

#include "content/browser/web_contents/web_contents_impl.h"

#include <memory>
#include <string>
#include <utility>

#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/app_window/native_app_window.h"
#include "extensions/browser/extension_web_contents_observer.h"
#include "extensions/common/extension_messages.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

#include "content/nw/src/nw_content.h"

namespace extensions {

AppWindowContentsImpl::AppWindowContentsImpl(AppWindow* host, std::unique_ptr<content::WebContents> web_contents)
  :host_(host), web_contents_(std::move(web_contents)) {}

AppWindowContentsImpl::~AppWindowContentsImpl() {}

void AppWindowContentsImpl::Initialize(content::BrowserContext* context,
                                       content::RenderFrameHost* creator_frame,
                                       const GURL& url,
                                       const Extension* extension,
                                       bool skip_blocking_parser) {
  url_ = url;

  bool new_site = url.SchemeIs("chrome") || !nw::PinningRenderer();
  content::WebContents::CreateParams create_params(
                                                   //NWJS#5163: fix regression
       context, nw::PinningRenderer() ? creator_frame->GetSiteInstance() : content::SiteInstance::CreateForURL(context, url_));
  create_params.opener_render_process_id = creator_frame->GetProcess()->GetID();
  create_params.opener_render_frame_id = creator_frame->GetRoutingID();
  if (!web_contents_)
    web_contents_ = content::WebContents::Create(create_params);

  static_cast<content::WebContentsImpl*>(web_contents_.get())->SetSkipBlockingParser(skip_blocking_parser || new_site);
  Observe(web_contents_.get());
  blink::RendererPreferences* render_prefs =
      web_contents_->GetMutableRendererPrefs();
  if (!extension || !extension->is_nwjs_app())
    render_prefs->browser_handles_all_top_level_requests = true;
  std::string user_agent;
  if (nw::GetUserAgentFromManifest(&user_agent))
    render_prefs->user_agent_override.ua_string_override = user_agent;
  web_contents_->SyncRendererPrefs();
}

void AppWindowContentsImpl::LoadContents(int32_t creator_process_id) {
  // Sandboxed page that are not in the Chrome App package are loaded in a
  // different process.
  if (web_contents_->GetMainFrame()->GetProcess()->GetID() !=
      creator_process_id) {
    VLOG(1) << "AppWindow created in new process ("
            << web_contents_->GetMainFrame()->GetProcess()->GetID()
            << ") != creator (" << creator_process_id << "). Routing disabled.";
  }
  web_contents_->GetController().LoadURL(
      url_, content::Referrer(), ui::PAGE_TRANSITION_LINK,
      std::string());
}

void AppWindowContentsImpl::NativeWindowChanged(
    NativeAppWindow* native_app_window) {
  base::Value dictionary(base::Value::Type::DICTIONARY);
  host_->GetSerializedState(&dictionary);
  base::Value::List args;
  args.Append(std::move(dictionary));

  content::RenderFrameHost* rfh = web_contents_->GetMainFrame();
  // Return early if this method is called before RenderFrameCreated(). (e.g.
  // if AppWindow is created and shown before navigation, this method is called
  // for the visibility change.)
  if (!rfh->IsRenderFrameLive())
    return;
  ExtensionWebContentsObserver::GetForWebContents(web_contents())
      ->GetLocalFrame(rfh)
      ->MessageInvoke(host_->extension_id(), "app.window",
                      "updateAppWindowProperties", std::move(args));
}

void AppWindowContentsImpl::NativeWindowClosed(bool send_onclosed) {
  // Return early if this method is called when the render frame is not live.
  if (!web_contents_->GetMainFrame()->IsRenderFrameLive())
    return;
  ExtensionWebContentsObserver::GetForWebContents(web_contents())
      ->GetLocalFrame(web_contents_->GetMainFrame())
      ->AppWindowClosed(send_onclosed);
}

content::WebContents* AppWindowContentsImpl::GetWebContents() const {
  return web_contents_.get();
}

WindowController* AppWindowContentsImpl::GetWindowController() const {
  return nullptr;
}

bool AppWindowContentsImpl::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* sender) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(AppWindowContentsImpl, message, sender)
    IPC_MESSAGE_HANDLER(ExtensionHostMsg_UpdateDraggableRegions,
                        UpdateDraggableRegions)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void AppWindowContentsImpl::DidFinishNavigation(
    content::NavigationHandle* handle) {
  // The callback inside app_window will be moved after the first call.
  host_->OnDidFinishFirstNavigation();
}

void AppWindowContentsImpl::UpdateDraggableRegions(
    content::RenderFrameHost* sender,
    const std::vector<DraggableRegion>& regions) {
  if (!sender->GetParent())  // Only process events from the main frame.
    host_->UpdateDraggableRegions(regions);
}

}  // namespace extensions
