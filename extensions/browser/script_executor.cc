// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/script_executor.h"

#include <set>
#include <string>

#include "base/bind.h"
#include "base/check_op.h"
#include "base/containers/contains.h"
#include "base/containers/cxx20_erase.h"
#include "base/dcheck_is_on.h"
#include "base/hash/hash.h"
#include "base/memory/weak_ptr.h"
#include "base/pickle.h"
#include "base/ranges/algorithm.h"
#include "base/strings/stringprintf.h"
#include "base/types/pass_key.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/content_script_tracker.h"
#include "extensions/browser/extension_api_frame_id_map.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_web_contents_observer.h"
#include "extensions/common/extension_messages.h"
#include "extensions/common/mojom/host_id.mojom.h"
#include "ipc/ipc_message.h"
#include "ipc/ipc_message_macros.h"

namespace base {
class ListValue;
}  // namespace base

namespace extensions {

namespace {

// A handler for a single injection request. On creation this will send the
// injection request to the renderer, and it will be destroyed after either the
// corresponding response comes from the renderer, or the renderer is destroyed.
class Handler : public content::WebContentsObserver {
 public:
  // OnceCallback version of ScriptExecutor::ScriptsExecutedNotification:
  using ScriptsExecutedOnceCallback = base::OnceCallback<
      void(content::WebContents*, const ExecutingScriptsMap&, const GURL&)>;

  Handler(base::PassKey<ScriptExecutor> pass_key,
          ScriptsExecutedOnceCallback observer,
          content::WebContents* web_contents,
          mojom::ExecuteCodeParamsPtr params,
          ScriptExecutor::FrameScope scope,
          const std::set<int>& frame_ids,
          ScriptExecutor::ScriptFinishedCallback callback)
      : content::WebContentsObserver(web_contents),
        observer_(std::move(observer)),
        host_id_(params->host_id->type, params->host_id->id),
        callback_(std::move(callback)) {
    for (int frame_id : frame_ids) {
      content::RenderFrameHost* frame =
          ExtensionApiFrameIdMap::GetRenderFrameHostById(web_contents,
                                                         frame_id);
      if (!frame) {
        AddWillNotInjectResult(
            frame_id, ExtensionApiFrameIdMap::DocumentId(),
            base::StringPrintf("No frame with ID: %d", frame_id));
        continue;
      }

      DCHECK(!base::Contains(pending_render_frames_, frame));
      if (!frame->IsRenderFrameLive()) {
        ExtensionApiFrameIdMap::DocumentId document_id =
            ExtensionApiFrameIdMap::GetDocumentId(frame);
        AddWillNotInjectResult(
            frame_id, document_id,
            base::StringPrintf("Frame with ID %d is not ready", frame_id));
        continue;
      }

      if (frame->IsErrorDocument()) {
        ExtensionApiFrameIdMap::DocumentId document_id =
            ExtensionApiFrameIdMap::GetDocumentId(frame);
        AddWillNotInjectResult(
            frame_id, document_id,
            base::StringPrintf("Frame with ID %d is showing error page",
                               frame_id));
        continue;
      }

      pending_render_frames_.push_back(frame);
    }

    // If there is a single frame specified (and it was valid), we consider it
    // the "root" frame, which is used in result ordering and error collection.
    if (frame_ids.size() == 1 && pending_render_frames_.size() == 1)
      root_rfh_id_ = *frame_ids.begin();

    // If we are to include subframes, iterate over all descendants of frames in
    // `pending_render_frames_` and add them if they are alive (and not already
    // contained in `pending_frames`).
    if (scope == ScriptExecutor::INCLUDE_SUB_FRAMES) {
      auto append_frame = [](content::WebContents* web_contents,
                             std::vector<raw_ptr<content::RenderFrameHost>>*
                                 pending_frames,
                             content::RenderFrameHost* frame) {
        // Avoid inner web contents. If we need to execute scripts on
        // inner WebContents this class needs to be updated.
        // See crbug.com/1301320.
        if (content::WebContents::FromRenderFrameHost(frame) != web_contents) {
          return content::RenderFrameHost::FrameIterationAction::kSkipChildren;
        }
        if (!frame->IsRenderFrameLive() ||
            base::Contains(*pending_frames, frame)) {
          return content::RenderFrameHost::FrameIterationAction::kContinue;
        }

        pending_frames->push_back(frame);
        return content::RenderFrameHost::FrameIterationAction::kContinue;
      };

      // We iterate over the requested frames. Note we can't use an iterator
      // as the for loop will mutate `pending_render_frames_`.
      size_t requested_frame_count = pending_render_frames_.size();
      for (size_t i = 0; i < requested_frame_count; ++i) {
        pending_render_frames_.at(i)->ForEachRenderFrameHost(
            base::BindRepeating(append_frame, web_contents,
                                &pending_render_frames_));
      }
    }

    for (content::RenderFrameHost* frame : pending_render_frames_)
      SendExecuteCode(pass_key, params.Clone(), frame);

    if (pending_render_frames_.empty())
      Finish();
  }

  Handler(const Handler&) = delete;
  Handler& operator=(const Handler&) = delete;

 private:
  // This class manages its own lifetime.
  ~Handler() override {}

  // content::WebContentsObserver:
  // TODO(devlin): Could we just rely on the RenderFrameDeleted() notification?
  // If so, we could remove this.
  void WebContentsDestroyed() override {
    for (content::RenderFrameHost* frame : pending_render_frames_) {
      int frame_id = ExtensionApiFrameIdMap::GetFrameId(frame);
      ExtensionApiFrameIdMap::DocumentId document_id =
          ExtensionApiFrameIdMap::GetDocumentId(frame);
      AddWillNotInjectResult(
          frame_id, document_id,
          base::StringPrintf("Tab containing frame with ID %d was removed.",
                             frame_id));
    }
    pending_render_frames_.clear();
    Finish();
  }

  void RenderFrameDeleted(
      content::RenderFrameHost* render_frame_host) override {
    int erased_count = base::Erase(pending_render_frames_, render_frame_host);
    DCHECK_LE(erased_count, 1);
    if (erased_count == 0)
      return;

    int frame_id = ExtensionApiFrameIdMap::GetFrameId(render_frame_host);
    ExtensionApiFrameIdMap::DocumentId document_id =
        ExtensionApiFrameIdMap::GetDocumentId(render_frame_host);
    AddWillNotInjectResult(
        frame_id, document_id,
        base::StringPrintf("Frame with ID %d was removed.", frame_id));
    if (pending_render_frames_.empty())
      Finish();
  }

  void AddWillNotInjectResult(
      int frame_id,
      const ExtensionApiFrameIdMap::DocumentId& document_id,
      std::string error) {
    ScriptExecutor::FrameResult result;
    result.frame_id = frame_id;
    result.document_id = document_id;
    result.error = std::move(error);
    results_.push_back(std::move(result));
  }

  // Sends an ExecuteCode message to the given frame host, and increments
  // the number of pending messages.
  void SendExecuteCode(base::PassKey<ScriptExecutor> pass_key,
                       mojom::ExecuteCodeParamsPtr params,
                       content::RenderFrameHost* frame) {
    DCHECK(frame->IsRenderFrameLive());
    DCHECK(base::Contains(pending_render_frames_, frame));
    ContentScriptTracker::WillExecuteCode(pass_key, frame, host_id_);
    ExtensionWebContentsObserver::GetForWebContents(web_contents())
        ->GetLocalFrame(frame)
        ->ExecuteCode(std::move(params),
                      base::BindOnce(&Handler::OnExecuteCodeFinished,
                                     weak_ptr_factory_.GetWeakPtr(),
                                     frame->GetProcess()->GetID(),
                                     frame->GetRoutingID()));
  }

  // Handles the ExecuteCodeFinished message.
  void OnExecuteCodeFinished(int render_process_id,
                             int render_frame_id,
                             const std::string& error,
                             const GURL& on_url,
                             absl::optional<base::Value> result) {
    auto* render_frame_host =
        content::RenderFrameHost::FromID(render_process_id, render_frame_id);
    if (!render_frame_host)
      return;

    DCHECK(!pending_render_frames_.empty());
    size_t erased = base::Erase(pending_render_frames_, render_frame_host);
    DCHECK_EQ(1u, erased);

    ScriptExecutor::FrameResult frame_result;
    frame_result.frame_responded = true;
    frame_result.frame_id =
        ExtensionApiFrameIdMap::GetFrameId(render_frame_host);
    frame_result.document_id =
        ExtensionApiFrameIdMap::GetDocumentId(render_frame_host);
    frame_result.error = error;
    // TODO(devlin): Do we need to trust the renderer for the URL here? Is there
    // a risk of the frame having navigated since the injection happened?
    frame_result.url = on_url;
    if (result.has_value())
      frame_result.value = result->Clone();

    results_.push_back(std::move(frame_result));

    // Wait until the final request finishes before reporting back.
    if (pending_render_frames_.empty())
      Finish();
  }

  void Finish() {
    DCHECK(pending_render_frames_.empty());
    DCHECK(!results_.empty());

    // TODO(devlin): This would be simpler (and more thorough) if we could just
    // invoke the observer for each frame. Investigate.
    if (observer_ && root_rfh_id_) {
      auto root_frame_result =
          std::find_if(results_.begin(), results_.end(),
                       [root_rfh_id = *root_rfh_id_](const auto& frame_result) {
                         return frame_result.frame_id == root_rfh_id;
                       });
      DCHECK(root_frame_result != results_.end());
      if (root_frame_result->error.empty() &&
          host_id_.type == mojom::HostID::HostType::kExtensions) {
        std::move(observer_).Run(web_contents(), {{host_id_.id, {}}},
                                 root_frame_result->url);
      }
    }

    if (callback_)
      std::move(callback_).Run(std::move(results_));

    delete this;
  }

  ScriptsExecutedOnceCallback observer_;

  // The id of the host (the extension or the webui) doing the injection.
  mojom::HostID host_id_;

  // The id of the primary frame of the injection, if only a single frame is
  // explicitly specified.
  absl::optional<int> root_rfh_id_;

  // The hosts of the still-running injections. Note: this is a vector because
  // order matters (some tests - and therefore perhaps some extensions - rely on
  // the execution mirroring the frame tree hierarchy). The contents, however,
  // should be unique (i.e., no duplicated frames).
  // TODO(devlin): Extensions *shouldn't* rely on order here, because there's
  // never a guarantee. We should probably just adjust the test and disregard
  // order (except the root frame).
  std::vector<raw_ptr<content::RenderFrameHost>> pending_render_frames_;

  // The results of the injection.
  std::vector<ScriptExecutor::FrameResult> results_;

  // The callback to run after all injections complete.
  ScriptExecutor::ScriptFinishedCallback callback_;

  base::WeakPtrFactory<Handler> weak_ptr_factory_{this};
};

}  // namespace

ScriptExecutor::FrameResult::FrameResult() = default;
ScriptExecutor::FrameResult::FrameResult(FrameResult&&) = default;
ScriptExecutor::FrameResult& ScriptExecutor::FrameResult::operator=(
    FrameResult&&) = default;

ScriptExecutor::ScriptExecutor(content::WebContents* web_contents)
    : web_contents_(web_contents) {
  CHECK(web_contents_);
}

ScriptExecutor::~ScriptExecutor() {}

// static
std::string ScriptExecutor::GenerateInjectionKey(const mojom::HostID& host_id,
                                                 const GURL& script_url,
                                                 const std::string& code) {
  const std::string& source = script_url.is_valid() ? script_url.spec() : code;
  return base::StringPrintf("%c%s%zu", script_url.is_valid() ? 'F' : 'C',
                            host_id.id.c_str(), base::FastHash(source));
}

void ScriptExecutor::ExecuteScript(const mojom::HostID& host_id,
                                   mojom::CodeInjectionPtr injection,
                                   ScriptExecutor::FrameScope frame_scope,
                                   const std::set<int>& frame_ids,
                                   ScriptExecutor::MatchAboutBlank about_blank,
                                   mojom::RunLocation run_at,
                                   ScriptExecutor::WorldType world_type,
                                   ScriptExecutor::ProcessType process_type,
                                   const GURL& webview_src,
                                   ScriptFinishedCallback callback) {
  if (host_id.type == mojom::HostID::HostType::kExtensions) {
    // Don't execute if the extension has been unloaded.
    const Extension* extension =
        ExtensionRegistry::Get(web_contents_->GetBrowserContext())
            ->enabled_extensions()
            .GetByID(host_id.id);
    if (!extension)
      return;
  } else {
    CHECK(process_type == WEB_VIEW_PROCESS);
  }

#if DCHECK_IS_ON()
  if (injection->is_css()) {
    bool expect_injection_key =
        host_id.type == mojom::HostID::HostType::kExtensions;
    if (injection->get_css()->operation ==
        mojom::CSSInjection::Operation::kRemove) {
      DCHECK(expect_injection_key)
          << "Only extensions (with injection keys supplied) can remove CSS.";
    }
    DCHECK(base::ranges::all_of(
        injection->get_css()->sources,
        [expect_injection_key](const mojom::CSSSourcePtr& source) {
          return expect_injection_key == source->key.has_value();
        }));
  }
#endif

  auto params = mojom::ExecuteCodeParams::New();
  params->host_id = host_id.Clone();
  params->injection = std::move(injection);
  params->in_main_world = (world_type == MAIN_WORLD);
  params->match_about_blank = (about_blank == MATCH_ABOUT_BLANK);
  params->run_at = run_at;
  params->is_web_view = (process_type == WEB_VIEW_PROCESS);
  params->webview_src = webview_src;

  // Handler handles IPCs and deletes itself on completion.
  new Handler(base::PassKey<ScriptExecutor>(), observer_, web_contents_,
              std::move(params), frame_scope, frame_ids, std::move(callback));
}

}  // namespace extensions
