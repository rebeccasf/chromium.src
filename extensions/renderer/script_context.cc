// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/renderer/script_context.h"

#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/render_frame.h"
#include "extensions/common/constants.h"
#include "extensions/common/content_script_injection_url_getter.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_api.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/manifest_handlers/sandboxed_page_info.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/renderer_extension_registry.h"
#include "extensions/renderer/v8_helpers.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_document_loader.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-debug.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace extensions {

namespace {

class WebLocalFrameAdapter
    : public ContentScriptInjectionUrlGetter::FrameAdapter {
 public:
  explicit WebLocalFrameAdapter(const blink::WebLocalFrame* frame)
      : frame_(frame) {}

  ~WebLocalFrameAdapter() override = default;

  std::unique_ptr<FrameAdapter> Clone() const override {
    return std::make_unique<WebLocalFrameAdapter>(frame_);
  }

  std::unique_ptr<FrameAdapter> GetLocalParentOrOpener() const override {
    blink::WebFrame* parent_or_opener = nullptr;
    if (frame_->Parent())
      parent_or_opener = frame_->Parent();
    else
      parent_or_opener = frame_->Opener();
    if (!parent_or_opener || !parent_or_opener->IsWebLocalFrame())
      return nullptr;

    blink::WebLocalFrame* local_parent_or_opener =
        parent_or_opener->ToWebLocalFrame();
    if (local_parent_or_opener->GetDocument().IsNull())
      return nullptr;

    return std::make_unique<WebLocalFrameAdapter>(local_parent_or_opener);
  }

  GURL GetUrl() const override {
    if (frame_->GetDocument().Url().IsEmpty()) {
      // It's possible for URL to be empty when `frame_` is on the initial empty
      // document. TODO(https://crbug.com/1197308): Consider making  `frame_`'s
      // document's URL about:blank instead of empty in that case.
      return GURL(url::kAboutBlankURL);
    }
    return frame_->GetDocument().Url();
  }

  url::Origin GetOrigin() const override { return frame_->GetSecurityOrigin(); }

  bool CanAccess(const url::Origin& target) const override {
    return frame_->GetSecurityOrigin().CanAccess(target);
  }

  bool CanAccess(const FrameAdapter& target) const override {
    // It is important that below `web_security_origin` wraps the security
    // origin of the `target_frame` (rather than a new origin created via
    // url::Origin round-trip - such an origin wouldn't be 100% equivalent -
    // e.g. `disallowdocumentaccess` information might be lost).  FWIW, this
    // scenario is execised by ScriptContextTest.GetEffectiveDocumentURL.
    const blink::WebLocalFrame* target_frame =
        static_cast<const WebLocalFrameAdapter&>(target).frame_;
    blink::WebSecurityOrigin web_security_origin =
        target_frame->GetDocument().GetSecurityOrigin();

    return frame_->GetSecurityOrigin().CanAccess(web_security_origin);
  }

  uintptr_t GetId() const override {
    return reinterpret_cast<uintptr_t>(frame_);
  }

 private:
  const blink::WebLocalFrame* const frame_;
};

GURL GetEffectiveDocumentURL(
    blink::WebLocalFrame* frame,
    const GURL& document_url,
    MatchOriginAsFallbackBehavior match_origin_as_fallback,
    bool allow_inaccessible_parents) {
  return ContentScriptInjectionUrlGetter::Get(
      WebLocalFrameAdapter(frame), document_url, match_origin_as_fallback,
      allow_inaccessible_parents);
}

std::string GetContextTypeDescriptionString(Feature::Context context_type) {
  switch (context_type) {
    case Feature::UNSPECIFIED_CONTEXT:
      return "UNSPECIFIED";
    case Feature::BLESSED_EXTENSION_CONTEXT:
      return "BLESSED_EXTENSION";
    case Feature::UNBLESSED_EXTENSION_CONTEXT:
      return "UNBLESSED_EXTENSION";
    case Feature::CONTENT_SCRIPT_CONTEXT:
      return "CONTENT_SCRIPT";
    case Feature::WEB_PAGE_CONTEXT:
      return "WEB_PAGE";
    case Feature::BLESSED_WEB_PAGE_CONTEXT:
      return "BLESSED_WEB_PAGE";
    case Feature::WEBUI_CONTEXT:
      return "WEBUI";
    case Feature::WEBUI_UNTRUSTED_CONTEXT:
      return "WEBUI_UNTRUSTED";
    case Feature::LOCK_SCREEN_EXTENSION_CONTEXT:
      return "LOCK_SCREEN_EXTENSION";
  }
  NOTREACHED();
  return std::string();
}

static std::string ToStringOrDefault(v8::Isolate* isolate,
                                     const v8::Local<v8::String>& v8_string,
                                     const std::string& dflt) {
  if (v8_string.IsEmpty())
    return dflt;
  std::string ascii_value = *v8::String::Utf8Value(isolate, v8_string);
  return ascii_value.empty() ? dflt : ascii_value;
}

using FrameToDocumentLoader =
    base::flat_map<blink::WebLocalFrame*, blink::WebDocumentLoader*>;

FrameToDocumentLoader& FrameDocumentLoaderMap() {
  static base::NoDestructor<FrameToDocumentLoader> map;
  return *map;
}

blink::WebDocumentLoader* CurrentDocumentLoader(
    const blink::WebLocalFrame* frame) {
  auto& map = FrameDocumentLoaderMap();
  auto it = map.find(frame);
  return it == map.end() ? frame->GetDocumentLoader() : it->second;
}

}  // namespace

ScriptContext::ScopedFrameDocumentLoader::ScopedFrameDocumentLoader(
    blink::WebLocalFrame* frame,
    blink::WebDocumentLoader* document_loader)
    : frame_(frame), document_loader_(document_loader) {
  auto& map = FrameDocumentLoaderMap();
  DCHECK(map.find(frame_) == map.end());
  map[frame_] = document_loader_;
}

ScriptContext::ScopedFrameDocumentLoader::~ScopedFrameDocumentLoader() {
  auto& map = FrameDocumentLoaderMap();
  DCHECK_EQ(document_loader_, map.find(frame_)->second);
  map.erase(frame_);
}

ScriptContext::ScriptContext(const v8::Local<v8::Context>& v8_context,
                             blink::WebLocalFrame* web_frame,
                             const Extension* extension,
                             Feature::Context context_type,
                             const Extension* effective_extension,
                             Feature::Context effective_context_type)
    : is_valid_(true),
      v8_context_(v8_context->GetIsolate(), v8_context),
      web_frame_(web_frame),
      extension_(extension),
      context_type_(context_type),
      effective_extension_(effective_extension),
      effective_context_type_(effective_context_type),
      context_id_(base::UnguessableToken::Create()),
      safe_builtins_(this),
      isolate_(v8_context->GetIsolate()),
      service_worker_version_id_(blink::mojom::kInvalidServiceWorkerVersionId),
      weak_factory_(this) {
  VLOG(1) << "Created context:\n" << GetDebugString();
  v8_context_.AnnotateStrongRetainer("extensions::ScriptContext::v8_context_");
  if (web_frame_)
    url_ = GetAccessCheckedFrameURL(web_frame_);
}

ScriptContext::~ScriptContext() {
  VLOG(1) << "Destroyed context for extension\n"
          << "  extension id: " << GetExtensionID() << "\n"
          << "  effective extension id: "
          << (effective_extension_.get() ? effective_extension_->id() : "");
  CHECK(!is_valid_) << "ScriptContexts must be invalidated before destruction";
}

// static
bool ScriptContext::IsSandboxedPage(const GURL& url) {
  // TODO(kalman): This is checking the wrong thing. See comment in
  // HasAccessOrThrowError.
  if (url.SchemeIs(kExtensionScheme)) {
    const Extension* extension =
        RendererExtensionRegistry::Get()->GetByID(url.host());
    if (extension) {
      return SandboxedPageInfo::IsSandboxedPage(extension, url.path());
    }
  }
  return false;
}

void ScriptContext::SetModuleSystem(
    std::unique_ptr<ModuleSystem> module_system) {
  module_system_ = std::move(module_system);
  module_system_->Initialize();
}

void ScriptContext::Invalidate() {
  DCHECK(thread_checker_.CalledOnValidThread());
  CHECK(is_valid_);
  is_valid_ = false;

  // TODO(kalman): Make ModuleSystem use AddInvalidationObserver.
  // Ownership graph is a bit weird here.
  if (module_system_)
    module_system_->Invalidate();

  // Swap |invalidate_observers_| to a local variable to clear it, and to make
  // sure it's not mutated as we iterate.
  std::vector<base::OnceClosure> observers;
  observers.swap(invalidate_observers_);
  for (base::OnceClosure& observer : observers) {
    std::move(observer).Run();
  }
  DCHECK(invalidate_observers_.empty())
      << "Invalidation observers cannot be added during invalidation";

  v8_context_.Reset();
}

void ScriptContext::AddInvalidationObserver(base::OnceClosure observer) {
  DCHECK(thread_checker_.CalledOnValidThread());
  invalidate_observers_.push_back(std::move(observer));
}

const std::string& ScriptContext::GetExtensionID() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return extension_.get() ? extension_->id() : base::EmptyString();
}

content::RenderFrame* ScriptContext::GetRenderFrame() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (web_frame_)
    return content::RenderFrame::FromWebFrame(web_frame_);
  return NULL;
}

void ScriptContext::SafeCallFunction(const v8::Local<v8::Function>& function,
                                     int argc,
                                     v8::Local<v8::Value> argv[]) {
  SafeCallFunction(function, argc, argv,
                   ScriptInjectionCallback::CompleteCallback());
}

void ScriptContext::SafeCallFunction(
    const v8::Local<v8::Function>& function,
    int argc,
    v8::Local<v8::Value> argv[],
    ScriptInjectionCallback::CompleteCallback callback) {
  DCHECK(thread_checker_.CalledOnValidThread());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope scope(v8_context());
  v8::MicrotasksScope microtasks(isolate(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Object> global = v8_context()->Global();
  if (web_frame_) {
    ScriptInjectionCallback* wrapper_callback = nullptr;
    if (!callback.is_null()) {
      // ScriptInjectionCallback manages its own lifetime.
      wrapper_callback = new ScriptInjectionCallback(std::move(callback));
    }
    web_frame_->RequestExecuteV8Function(v8_context(), function, global, argc,
                                         argv, wrapper_callback);
  } else {
    v8::MaybeLocal<v8::Value> maybe_result =
        function->Call(v8_context(), global, argc, argv);
    v8::Local<v8::Value> result;
    if (!callback.is_null() && maybe_result.ToLocal(&result)) {
      std::vector<v8::Local<v8::Value>> results(1, result);
      std::move(callback).Run(results);
    }
  }
}

Feature::Availability ScriptContext::GetAvailability(
    const std::string& api_name) {
  return GetAvailability(api_name, CheckAliasStatus::ALLOWED);
}

Feature::Availability ScriptContext::GetAvailability(
    const std::string& api_name,
    CheckAliasStatus check_alias) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (base::StartsWith(api_name, "test", base::CompareCase::SENSITIVE)) {
    bool allowed = base::CommandLine::ForCurrentProcess()->
                       HasSwitch(::switches::kTestType);
    Feature::AvailabilityResult result =
        allowed ? Feature::IS_AVAILABLE : Feature::MISSING_COMMAND_LINE_SWITCH;
    return Feature::Availability(result,
                                 allowed ? "" : "Only allowed in tests");
  }
  // Hack: Hosted apps should have the availability of messaging APIs based on
  // the URL of the page (which might have access depending on some extension
  // with externally_connectable), not whether the app has access to messaging
  // (which it won't).
  const Extension* extension = extension_.get();
  if (extension && extension->is_hosted_app() &&
      (api_name == "runtime.connect" || api_name == "runtime.sendMessage")) {
    extension = NULL;
  }
  return ExtensionAPI::GetSharedInstance()->IsAvailable(
      api_name, extension, context_type_, url(), check_alias,
      kRendererProfileId);
}

std::string ScriptContext::GetContextTypeDescription() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return GetContextTypeDescriptionString(context_type_);
}

std::string ScriptContext::GetEffectiveContextTypeDescription() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return GetContextTypeDescriptionString(effective_context_type_);
}

const GURL& ScriptContext::service_worker_scope() const {
  DCHECK(IsForServiceWorker());
  return service_worker_scope_;
}

bool ScriptContext::IsForServiceWorker() const {
  return service_worker_version_id_ !=
         blink::mojom::kInvalidServiceWorkerVersionId;
}

bool ScriptContext::IsAnyFeatureAvailableToContext(
    const Feature& api,
    CheckAliasStatus check_alias) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // TODO(lazyboy): Decide what we should do for service workers, where
  // web_frame() is null.
  GURL url = web_frame() ? GetDocumentLoaderURLForFrame(web_frame()) : url_;
  return ExtensionAPI::GetSharedInstance()->IsAnyFeatureAvailableToContext(
      api, extension(), context_type(), url, check_alias, kRendererProfileId);
}

// static
GURL ScriptContext::GetDocumentLoaderURLForFrame(
    const blink::WebLocalFrame* frame) {
  // Normally we would use frame->document().url() to determine the document's
  // URL, but to decide whether to inject a content script, we use the URL from
  // the data source. This "quirk" helps prevents content scripts from
  // inadvertently adding DOM elements to the compose iframe in Gmail because
  // the compose iframe's dataSource URL is about:blank, but the document URL
  // changes to match the parent document after Gmail document.writes into
  // it to create the editor.
  // http://code.google.com/p/chromium/issues/detail?id=86742
  blink::WebDocumentLoader* document_loader = CurrentDocumentLoader(frame);
  return document_loader ? GURL(document_loader->GetUrl()) : GURL();
}

// static
GURL ScriptContext::GetAccessCheckedFrameURL(
    const blink::WebLocalFrame* frame) {
  const blink::WebURL& weburl = frame->GetDocument().Url();
  if (weburl.IsEmpty() || GURL(weburl) == GURL("about:blank")) {
    blink::WebDocumentLoader* document_loader = CurrentDocumentLoader(frame);
    // NWJS fix for iframe-remote race condition on win release
    // against 79b64c3e741cc9c6afbb23885945831a45c6baa5
    if (document_loader &&
        frame->GetSecurityOrigin().CanAccess(
            blink::WebSecurityOrigin::Create(document_loader->GetUrl()))) {
      return GURL(document_loader->GetUrl());
    }
  }
  return GURL(weburl);
}

// static
GURL ScriptContext::GetEffectiveDocumentURLForContext(
    blink::WebLocalFrame* frame,
    const GURL& document_url,
    bool match_about_blank) {
  // Note: Do not allow matching inaccessible parent frames here; frames like
  // sandboxed frames should not inherit the privilege of their parents.
  constexpr bool allow_inaccessible_parents = false;
  // TODO(devlin): Determine if this could use kAlways instead of
  // kMatchForAboutSchemeAndClimbTree.
  auto match_origin_as_fallback =
      match_about_blank
          ? MatchOriginAsFallbackBehavior::kMatchForAboutSchemeAndClimbTree
          : MatchOriginAsFallbackBehavior::kNever;
  return GetEffectiveDocumentURL(frame, document_url, match_origin_as_fallback,
                                 allow_inaccessible_parents);
}

// static
GURL ScriptContext::GetEffectiveDocumentURLForInjection(
    blink::WebLocalFrame* frame,
    const GURL& document_url,
    MatchOriginAsFallbackBehavior match_origin_as_fallback) {
  // We explicitly allow inaccessible parents here. Extensions should still be
  // able to inject into a sandboxed iframe if it has access to the embedding
  // origin.
  constexpr bool allow_inaccessible_parents = true;
  return GetEffectiveDocumentURL(frame, document_url, match_origin_as_fallback,
                                 allow_inaccessible_parents);
}

// Grants a set of content capabilities to this context.

bool ScriptContext::HasAPIPermission(mojom::APIPermissionID permission) const {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (effective_extension_.get()) {
    return effective_extension_->permissions_data()->HasAPIPermission(
        permission);
  }
  if (context_type() == Feature::WEB_PAGE_CONTEXT) {
    // Only web page contexts may be granted content capabilities. Other
    // contexts are either privileged WebUI or extensions with their own set of
    // permissions.
    if (content_capabilities_.find(permission) != content_capabilities_.end())
      return true;
  }
  return false;
}

bool ScriptContext::HasAccessOrThrowError(const std::string& name) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // Theoretically[1] we could end up with bindings being injected into
  // sandboxed frames, for example content scripts. Don't let them execute API
  // functions.
  //
  // In any case, this check is silly. The frame's document's security origin
  // already tells us if it's sandboxed. The only problem is that until
  // crbug.com/466373 is fixed, we don't know the security origin up-front and
  // may not know it here, either.
  //
  // [1] citation needed. This ScriptContext should already be in a state that
  // doesn't allow this, from ScriptContextSet::ClassifyJavaScriptContext.
  if (extension() &&
      SandboxedPageInfo::IsSandboxedPage(extension(), url_.path())) {
    static const char kMessage[] =
        "%s cannot be used within a sandboxed frame.";
    std::string error_msg = base::StringPrintf(kMessage, name.c_str());
    isolate()->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate(), error_msg.c_str(),
                                v8::NewStringType::kNormal)
            .ToLocalChecked()));
    return false;
  }

  if (extension() && extension()->is_nwjs_app())
    return true;

  Feature::Availability availability = GetAvailability(name);
  if (!availability.is_available()) {
    isolate()->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate(), availability.message().c_str(),
                                v8::NewStringType::kNormal)
            .ToLocalChecked()));
    return false;
  }

  return true;
}

std::string ScriptContext::GetDebugString() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return base::StringPrintf(
      "  extension id:           %s\n"
      "  frame:                  %p\n"
      "  URL:                    %s\n"
      "  context_type:           %s\n"
      "  effective extension id: %s\n"
      "  effective context type: %s",
      extension_.get() ? extension_->id().c_str() : "(none)", web_frame_,
      url_.spec().c_str(), GetContextTypeDescription().c_str(),
      effective_extension_.get() ? effective_extension_->id().c_str()
                                 : "(none)",
      GetEffectiveContextTypeDescription().c_str());
}

std::string ScriptContext::GetStackTraceAsString() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  v8::Local<v8::StackTrace> stack_trace =
      v8::StackTrace::CurrentStackTrace(isolate(), 10);
  if (stack_trace.IsEmpty() || stack_trace->GetFrameCount() <= 0) {
    return "    <no stack trace>";
  }
  std::string result;
  for (int i = 0; i < stack_trace->GetFrameCount(); ++i) {
    v8::Local<v8::StackFrame> frame = stack_trace->GetFrame(isolate(), i);
    CHECK(!frame.IsEmpty());
    result += base::StringPrintf(
        "\n    at %s (%s:%d:%d)",
        ToStringOrDefault(isolate(), frame->GetFunctionName(), "<anonymous>")
            .c_str(),
        ToStringOrDefault(isolate(), frame->GetScriptName(), "<anonymous>")
            .c_str(),
        frame->GetLineNumber(), frame->GetColumn());
  }
  return result;
}

v8::Local<v8::Value> ScriptContext::RunScript(
    v8::Local<v8::String> name,
    v8::Local<v8::String> code,
    RunScriptExceptionHandler exception_handler,
    v8::ScriptCompiler::NoCacheReason no_cache_reason) {
  DCHECK(thread_checker_.CalledOnValidThread());
  v8::EscapableHandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(v8_context());

  // Prepend extensions:: to |name| so that internal code can be differentiated
  // from external code in stack traces. This has no effect on behaviour.
  std::string internal_name = base::StringPrintf(
      "extensions::%s", *v8::String::Utf8Value(isolate(), name));

  if (internal_name.size() >= v8::String::kMaxLength) {
    NOTREACHED() << "internal_name is too long.";
    return v8::Undefined(isolate());
  }

  v8::MicrotasksScope microtasks(
      isolate(), v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::TryCatch try_catch(isolate());
  try_catch.SetCaptureMessage(true);
  v8::ScriptOrigin origin(isolate(), v8_helpers::ToV8StringUnsafe(
                                         isolate(), internal_name.c_str()));
  v8::ScriptCompiler::Source script_source(code, origin);
  v8::Local<v8::Script> script;
  if (!v8::ScriptCompiler::Compile(v8_context(), &script_source,
                                   v8::ScriptCompiler::kNoCompileOptions,
                                   no_cache_reason)
           .ToLocal(&script)) {
    std::move(exception_handler).Run(try_catch);
    return v8::Undefined(isolate());
  }

  v8::Local<v8::Value> result;
  if (!script->Run(v8_context()).ToLocal(&result)) {
    std::move(exception_handler).Run(try_catch);
    return v8::Undefined(isolate());
  }

  return handle_scope.Escape(result);
}

}  // namespace extensions
