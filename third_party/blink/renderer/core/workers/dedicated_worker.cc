// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/workers/dedicated_worker.h"

#include <utility>

#include "base/feature_list.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"

#include "third_party/node-nw/src/node_webkit.h"
#define BLINK_HOOK_MAP(type, sym, fn) CORE_EXPORT type fn = nullptr;
#if defined(COMPONENT_BUILD) && defined(WIN32)
#define NW_HOOK_MAP(type, sym, fn) BASE_EXPORT type fn;
#else
#define NW_HOOK_MAP(type, sym, fn) extern type fn;
#endif
#include "content/nw/src/common/node_hooks.h"
#undef NW_HOOK_MAP

#include "base/command_line.h"

#include "services/network/public/mojom/fetch_api.mojom-blink.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/common/blob/blob_utils.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/public/mojom/script/script_type.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/dedicated_worker_host_factory.mojom-blink.h"
#include "third_party/blink/public/platform/web_content_settings_client.h"
#include "third_party/blink/public/platform/web_fetch_client_settings_object.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/post_message_helper.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_post_message_options.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/event_target_names.h"
#include "third_party/blink/renderer/core/events/message_event.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/fetch/request.h"
#include "third_party/blink/renderer/core/fileapi/public_url_manager.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_client.h"
#include "third_party/blink/renderer/core/frame/web_frame_widget_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/inspector/main_thread_debugger.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/loader/worker_fetch_context.h"
#include "third_party/blink/renderer/core/origin_trials/origin_trial_context.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/probe/core_probes.h"
#include "third_party/blink/renderer/core/script/script.h"
#include "third_party/blink/renderer/core/script_type_names.h"
#include "third_party/blink/renderer/core/workers/dedicated_worker_messaging_proxy.h"
#include "third_party/blink/renderer/core/workers/worker_classic_script_loader.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/platform/bindings/enumeration_base.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher_properties.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/dedicated_or_shared_worker_fetch_context_impl.h"
#include "third_party/blink/renderer/platform/weborigin/security_policy.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"

namespace blink {

DedicatedWorker* DedicatedWorker::Create(ExecutionContext* context,
                                         const String& url,
                                         const WorkerOptions* options,
                                         ExceptionState& exception_state) {
  DCHECK(context->IsContextThread());
  UseCounter::Count(context, WebFeature::kWorkerStart);
  if (context->IsContextDestroyed()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidAccessError,
                                      "The context provided is invalid.");
    return nullptr;
  }

  KURL script_request_url = ResolveURL(context, url, exception_state);
  if (!script_request_url.IsValid()) {
    // Don't throw an exception here because it's already thrown in
    // ResolveURL().
    return nullptr;
  }

  if (context->IsWorkerGlobalScope())
    UseCounter::Count(context, WebFeature::kNestedDedicatedWorker);

  DedicatedWorker* worker = MakeGarbageCollected<DedicatedWorker>(
      context, script_request_url, options);
  worker->UpdateStateIfNeeded();
  worker->Start();
  return worker;
}

DedicatedWorker::DedicatedWorker(ExecutionContext* context,
                                 const KURL& script_request_url,
                                 const WorkerOptions* options)
    : AbstractWorker(context),
      script_request_url_(script_request_url),
      options_(options),
      context_proxy_(
          MakeGarbageCollected<DedicatedWorkerMessagingProxy>(context, this)),
      factory_client_(
          Platform::Current()->CreateDedicatedWorkerHostFactoryClient(
              this,
              GetExecutionContext()->GetBrowserInterfaceBroker())) {
  DCHECK(context->IsContextThread());
  DCHECK(script_request_url_.IsValid());
  DCHECK(context_proxy_);

  outside_fetch_client_settings_object_ =
      MakeGarbageCollected<FetchClientSettingsObjectSnapshot>(
          context->Fetcher()->GetProperties().GetFetchClientSettingsObject());
}

DedicatedWorker::~DedicatedWorker() = default;

void DedicatedWorker::Dispose() {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());
  context_proxy_->ParentObjectDestroyed();
  factory_client_.reset();
}

void DedicatedWorker::postMessage(ScriptState* script_state,
                                  const ScriptValue& message,
                                  HeapVector<ScriptValue>& transfer,
                                  ExceptionState& exception_state) {
  PostMessageOptions* options = PostMessageOptions::Create();
  if (!transfer.IsEmpty())
    options->setTransfer(transfer);
  postMessage(script_state, message, options, exception_state);
}

void DedicatedWorker::postMessage(ScriptState* script_state,
                                  const ScriptValue& message,
                                  const PostMessageOptions* options,
                                  ExceptionState& exception_state) {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());
  if (!GetExecutionContext())
    return;

  BlinkTransferableMessage transferable_message;
  Transferables transferables;
  scoped_refptr<SerializedScriptValue> serialized_message =
      PostMessageHelper::SerializeMessageByMove(script_state->GetIsolate(),
                                                message, options, transferables,
                                                exception_state);
  if (exception_state.HadException())
    return;
  DCHECK(serialized_message);
  transferable_message.message = serialized_message;
  transferable_message.sender_origin =
      GetExecutionContext()->GetSecurityOrigin()->IsolatedCopy();

  // Disentangle the port in preparation for sending it to the remote context.
  transferable_message.ports = MessagePort::DisentanglePorts(
      ExecutionContext::From(script_state), transferables.message_ports,
      exception_state);
  if (exception_state.HadException())
    return;
  transferable_message.user_activation =
      PostMessageHelper::CreateUserActivationSnapshot(GetExecutionContext(),
                                                      options);

  transferable_message.sender_stack_trace_id =
      ThreadDebugger::From(script_state->GetIsolate())
          ->StoreCurrentStackTrace("Worker.postMessage");
  context_proxy_->PostMessageToWorkerGlobalScope(
      std::move(transferable_message));
}

// https://html.spec.whatwg.org/C/#worker-processing-model
void DedicatedWorker::Start() {
  DCHECK(GetExecutionContext()->IsContextThread());

  // This needs to be done after the UpdateStateIfNeeded is called as
  // calling into the debugger can cause a breakpoint.
  v8_stack_trace_id_ = ThreadDebugger::From(GetExecutionContext()->GetIsolate())
                           ->StoreCurrentStackTrace("Worker Created");
  if (base::FeatureList::IsEnabled(features::kPlzDedicatedWorker)) {
    // For classic script, always use "same-origin" credentials mode.
    // https://html.spec.whatwg.org/C/#fetch-a-classic-worker-script
    // For module script, respect the credentials mode specified by
    // WorkerOptions.
    // https://html.spec.whatwg.org/C/#workeroptions
    auto credentials_mode = network::mojom::CredentialsMode::kSameOrigin;
    if (options_->type() == script_type_names::kModule) {
      absl::optional<network::mojom::CredentialsMode> result =
          Request::ParseCredentialsMode(options_->credentials());
      DCHECK(result);
      credentials_mode = result.value();
    }

    mojo::PendingRemote<mojom::blink::BlobURLToken> blob_url_token;
    if (script_request_url_.ProtocolIs("blob")) {
      GetExecutionContext()->GetPublicURLManager().Resolve(
          script_request_url_, blob_url_token.InitWithNewPipeAndPassReceiver());
    }

    factory_client_->CreateWorkerHost(
        token_, script_request_url_, credentials_mode,
        WebFetchClientSettingsObject(*outside_fetch_client_settings_object_),
        std::move(blob_url_token));
    // Continue in OnScriptLoadStarted() or OnScriptLoadStartFailed().
    return;
  }

  mojo::PendingRemote<network::mojom::blink::URLLoaderFactory>
      blob_url_loader_factory;
  if (script_request_url_.ProtocolIs("blob")) {
    GetExecutionContext()->GetPublicURLManager().Resolve(
        script_request_url_,
        blob_url_loader_factory.InitWithNewPipeAndPassReceiver());
  }

  if (GetExecutionContext()->GetSecurityOrigin()->IsLocal()) {
    // Local resources always have empty COEP, and Worker creation
    // from a blob URL in a local resource cannot work with
    // asynchronous OnHostCreated call, so we call it directly here.
    // See https://crbug.com/1101603#c8.
    factory_client_->CreateWorkerHostDeprecated(token_, script_request_url_,
                                                base::DoNothing());
    OnHostCreated(std::move(blob_url_loader_factory),
                  network::CrossOriginEmbedderPolicy(), mojo::NullRemote());
    return;
  }

  factory_client_->CreateWorkerHostDeprecated(
      token_, script_request_url_,
      WTF::Bind(&DedicatedWorker::OnHostCreated, WrapWeakPersistent(this),
                std::move(blob_url_loader_factory)));
}

void DedicatedWorker::OnHostCreated(
    mojo::PendingRemote<network::mojom::blink::URLLoaderFactory>
        blob_url_loader_factory,
    const network::CrossOriginEmbedderPolicy& parent_coep,
    CrossVariantMojoRemote<
        mojom::blink::BackForwardCacheControllerHostInterfaceBase>
        back_forward_cache_controller_host) {
  DCHECK(!base::FeatureList::IsEnabled(features::kPlzDedicatedWorker));
  const RejectCoepUnsafeNone reject_coep_unsafe_none(
      network::CompatibleWithCrossOriginIsolated(parent_coep));
  if (options_->type() == script_type_names::kClassic) {
    // Legacy code path (to be deprecated, see https://crbug.com/835717):
    // A worker thread will start after scripts are fetched on the current
    // thread.
    classic_script_loader_ = MakeGarbageCollected<WorkerClassicScriptLoader>();
    classic_script_loader_->LoadTopLevelScriptAsynchronously(
        *GetExecutionContext(), GetExecutionContext()->Fetcher(),
        script_request_url_, nullptr /* worker_main_script_load_params */,
        mojom::blink::RequestContextType::WORKER,
        network::mojom::RequestDestination::kWorker,
        network::mojom::RequestMode::kSameOrigin,
        network::mojom::CredentialsMode::kSameOrigin,
        WTF::Bind(&DedicatedWorker::OnResponse, WrapPersistent(this)),
        WTF::Bind(&DedicatedWorker::OnFinished, WrapPersistent(this),
                  std::move(back_forward_cache_controller_host)),
        reject_coep_unsafe_none, std::move(blob_url_loader_factory));
    return;
  }
  if (options_->type() == script_type_names::kModule) {
    // Specify empty source code etc. here because scripts will be fetched on
    // the worker thread.
    ContinueStart(
        script_request_url_, nullptr /* worker_main_script_load_params */,
        network::mojom::ReferrerPolicy::kDefault,
        Vector<network::mojom::blink::ContentSecurityPolicyPtr>(),
        absl::nullopt /* response_address_space */, String() /* source_code */,
        reject_coep_unsafe_none, std::move(back_forward_cache_controller_host));
    return;
  }
  NOTREACHED() << "Invalid type: " << IDLEnumAsString(options_->type());
}

void DedicatedWorker::terminate() {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());
  context_proxy_->TerminateGlobalScope();
}

BeginFrameProviderParams DedicatedWorker::CreateBeginFrameProviderParams() {
  DCHECK(GetExecutionContext()->IsContextThread());
  // If we don't have a frame or we are not in window, some of the SinkIds
  // won't be initialized. If that's the case, the Worker will initialize it by
  // itself later.
  BeginFrameProviderParams begin_frame_provider_params;
  if (auto* window = DynamicTo<LocalDOMWindow>(GetExecutionContext())) {
    LocalFrame* frame = window->GetFrame();
    if (frame) {
      WebFrameWidgetImpl* widget =
          WebLocalFrameImpl::FromFrame(frame)->LocalRootFrameWidget();
      begin_frame_provider_params.parent_frame_sink_id =
          widget->GetFrameSinkId();
    }
    begin_frame_provider_params.frame_sink_id =
        Platform::Current()->GenerateFrameSinkId();
  }
  return begin_frame_provider_params;
}

void DedicatedWorker::ContextDestroyed() {
  DCHECK(GetExecutionContext()->IsContextThread());
  if (classic_script_loader_)
    classic_script_loader_->Cancel();
  factory_client_.reset();
  terminate();
}

bool DedicatedWorker::HasPendingActivity() const {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());
  // The worker context does not exist while loading, so we must ensure that the
  // worker object is not collected, nor are its event listeners.
  return context_proxy_->HasPendingActivity() || classic_script_loader_;
}

void DedicatedWorker::OnWorkerHostCreated(
    CrossVariantMojoRemote<mojom::blink::BrowserInterfaceBrokerInterfaceBase>
        browser_interface_broker,
    CrossVariantMojoRemote<mojom::blink::DedicatedWorkerHostInterfaceBase>
        dedicated_worker_host) {
  DCHECK(!browser_interface_broker_);
  browser_interface_broker_ = std::move(browser_interface_broker);
  pending_dedicated_worker_host_ = std::move(dedicated_worker_host);
}

void DedicatedWorker::OnScriptLoadStarted(
    std::unique_ptr<WorkerMainScriptLoadParameters>
        worker_main_script_load_params,
    CrossVariantMojoRemote<
        mojom::blink::BackForwardCacheControllerHostInterfaceBase>
        back_forward_cache_controller_host) {
  DCHECK(base::FeatureList::IsEnabled(features::kPlzDedicatedWorker));
  // Specify empty source code here because scripts will be fetched on the
  // worker thread.
  ContinueStart(script_request_url_, std::move(worker_main_script_load_params),
                network::mojom::ReferrerPolicy::kDefault,
                Vector<network::mojom::blink::ContentSecurityPolicyPtr>(),
                absl::nullopt /* response_address_space */,
                String() /* source_code */, RejectCoepUnsafeNone(false),
                std::move(back_forward_cache_controller_host));
}

void DedicatedWorker::OnScriptLoadStartFailed() {
  DCHECK(base::FeatureList::IsEnabled(features::kPlzDedicatedWorker));
  context_proxy_->DidFailToFetchScript();
  factory_client_.reset();
}

void DedicatedWorker::DispatchErrorEventForScriptFetchFailure() {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());
  // TODO(nhiroki): Add a console error message.
  DispatchEvent(*Event::CreateCancelable(event_type_names::kError));
}

std::unique_ptr<WebContentSettingsClient>
DedicatedWorker::CreateWebContentSettingsClient() {
  std::unique_ptr<WebContentSettingsClient> content_settings_client;
  if (auto* window = DynamicTo<LocalDOMWindow>(GetExecutionContext())) {
    return window->GetFrame()->Client()->CreateWorkerContentSettingsClient();
  } else if (GetExecutionContext()->IsWorkerGlobalScope()) {
    WebContentSettingsClient* web_worker_content_settings_client =
        To<WorkerGlobalScope>(GetExecutionContext())->ContentSettingsClient();
    if (web_worker_content_settings_client)
      return web_worker_content_settings_client->Clone();
  }
  return nullptr;
}

void DedicatedWorker::OnResponse() {
  DCHECK(GetExecutionContext()->IsContextThread());
  probe::DidReceiveScriptResponse(GetExecutionContext(),
                                  classic_script_loader_->Identifier());
}

void DedicatedWorker::OnFinished(
    mojo::PendingRemote<mojom::blink::BackForwardCacheControllerHost>
        back_forward_cache_controller_host) {
  DCHECK(GetExecutionContext()->IsContextThread());
  if (classic_script_loader_->Canceled()) {
    // Do nothing.
  } else if (classic_script_loader_->Failed()) {
    context_proxy_->DidFailToFetchScript();
  } else {
    network::mojom::ReferrerPolicy referrer_policy =
        network::mojom::ReferrerPolicy::kDefault;
    if (!classic_script_loader_->GetReferrerPolicy().IsNull()) {
      SecurityPolicy::ReferrerPolicyFromHeaderValue(
          classic_script_loader_->GetReferrerPolicy(),
          kDoNotSupportReferrerPolicyLegacyKeywords, &referrer_policy);
    }
    const KURL script_response_url = classic_script_loader_->ResponseURL();
    DCHECK(script_request_url_ == script_response_url ||
           SecurityOrigin::AreSameOrigin(script_request_url_,
                                         script_response_url));
    ContinueStart(
        script_response_url, nullptr /* worker_main_script_load_params */,
        referrer_policy,
        classic_script_loader_->GetContentSecurityPolicy()
            ? mojo::Clone(classic_script_loader_->GetContentSecurityPolicy()
                              ->GetParsedPolicies())
            : Vector<network::mojom::blink::ContentSecurityPolicyPtr>(),
        classic_script_loader_->ResponseAddressSpace(),
        classic_script_loader_->SourceText(), RejectCoepUnsafeNone(false),
        std::move(back_forward_cache_controller_host));
    probe::ScriptImported(GetExecutionContext(),
                          classic_script_loader_->Identifier(),
                          classic_script_loader_->SourceText());
  }
  classic_script_loader_ = nullptr;
}

void DedicatedWorker::ContinueStart(
    const KURL& script_url,
    std::unique_ptr<WorkerMainScriptLoadParameters>
        worker_main_script_load_params,
    network::mojom::ReferrerPolicy referrer_policy,
    Vector<network::mojom::blink::ContentSecurityPolicyPtr>
        response_content_security_policies,
    absl::optional<network::mojom::IPAddressSpace> response_address_space,
    const String& source_code,
    RejectCoepUnsafeNone reject_coep_unsafe_none,
    mojo::PendingRemote<mojom::blink::BackForwardCacheControllerHost>
        back_forward_cache_controller_host) {
  context_proxy_->StartWorkerGlobalScope(
      CreateGlobalScopeCreationParams(
          script_url, referrer_policy,
          std::move(response_content_security_policies),
          response_address_space),
      std::move(worker_main_script_load_params), options_, script_url,
      *outside_fetch_client_settings_object_, v8_stack_trace_id_, source_code,
      reject_coep_unsafe_none, token_,
      std::move(pending_dedicated_worker_host_),
      std::move(back_forward_cache_controller_host));
}

std::unique_ptr<GlobalScopeCreationParams>
DedicatedWorker::CreateGlobalScopeCreationParams(
    const KURL& script_url,
    network::mojom::ReferrerPolicy referrer_policy,
    Vector<network::mojom::blink::ContentSecurityPolicyPtr>
        response_content_security_policies,
    absl::optional<network::mojom::IPAddressSpace> response_address_space) {
  base::UnguessableToken parent_devtools_token;
  std::unique_ptr<WorkerSettings> settings;
  bool isNodeJS = false;
  std::string main_script;

  ExecutionContext* execution_context = GetExecutionContext();

  if (auto* window = DynamicTo<LocalDOMWindow>(execution_context)) {
    auto* frame = window->GetFrame();
    if (frame) {
      parent_devtools_token = frame->GetDevToolsFrameToken();
    }
    settings = std::make_unique<WorkerSettings>(frame->GetSettings());

    const base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();

    isNodeJS = window->GetFrame() && window->GetFrame()->isNodeJS() && command_line.HasSwitch("enable-node-worker");
    if (g_web_worker_start_thread_fn) {
      (*g_web_worker_start_thread_fn)(window->GetFrame(), (void*)script_url.GetPath().Utf8().data(), &main_script, &isNodeJS);
    }
  } else {
    WorkerGlobalScope* worker_global_scope =
        To<WorkerGlobalScope>(execution_context);
    parent_devtools_token =
        worker_global_scope->GetThread()->GetDevToolsWorkerToken();
    settings = WorkerSettings::Copy(worker_global_scope->GetWorkerSettings());
  }

  mojom::blink::ScriptType script_type =
      (options_->type() == script_type_names::kClassic)
          ? mojom::blink::ScriptType::kClassic
          : mojom::blink::ScriptType::kModule;

  return std::make_unique<GlobalScopeCreationParams>(isNodeJS, main_script,
      script_url, script_type, options_->name(), execution_context->UserAgent(),
      execution_context->GetUserAgentMetadata(), CreateWebWorkerFetchContext(),
      mojo::Clone(
          execution_context->GetContentSecurityPolicy()->GetParsedPolicies()),
      std::move(response_content_security_policies), referrer_policy,
      execution_context->GetSecurityOrigin(),
      execution_context->IsSecureContext(), execution_context->GetHttpsState(),
      MakeGarbageCollected<WorkerClients>(), CreateWebContentSettingsClient(),
      response_address_space,
      OriginTrialContext::GetInheritedTrialFeatures(execution_context).get(),
      parent_devtools_token, std::move(settings),
      mojom::blink::V8CacheOptions::kDefault,
      nullptr /* worklet_module_responses_map */,
      std::move(browser_interface_broker_),
      mojo::NullRemote() /* code_cache_host_interface */,
      CreateBeginFrameProviderParams(),
      execution_context->GetSecurityContext().GetPermissionsPolicy(),
      execution_context->GetAgentClusterID(), execution_context->UkmSourceID(),
      execution_context->GetExecutionContextToken(),
      execution_context->CrossOriginIsolatedCapability(),
      execution_context->DirectSocketCapability());
}

scoped_refptr<WebWorkerFetchContext>
DedicatedWorker::CreateWebWorkerFetchContext() {
  // This worker is being created by the window.
  if (auto* window = DynamicTo<LocalDOMWindow>(GetExecutionContext())) {
    scoped_refptr<WebWorkerFetchContext> web_worker_fetch_context;
    LocalFrame* frame = window->GetFrame();
    if (base::FeatureList::IsEnabled(features::kPlzDedicatedWorker)) {
      web_worker_fetch_context =
          frame->Client()->CreateWorkerFetchContextForPlzDedicatedWorker(
              factory_client_.get());
    } else {
      web_worker_fetch_context = frame->Client()->CreateWorkerFetchContext();
    }
    web_worker_fetch_context->SetIsOnSubframe(!frame->IsMainFrame());
    return web_worker_fetch_context;
  }

  // This worker is being created by an existing worker (i.e., nested workers).
  // Clone the worker fetch context from the parent's one.
  auto* scope = To<WorkerGlobalScope>(GetExecutionContext());
  auto& worker_fetch_context =
      static_cast<WorkerFetchContext&>(scope->Fetcher()->Context());

  return factory_client_->CloneWorkerFetchContext(
      To<DedicatedOrSharedWorkerFetchContextImpl>(
          worker_fetch_context.GetWebWorkerFetchContext()),
      scope->GetTaskRunner(TaskType::kNetworking));
}

const AtomicString& DedicatedWorker::InterfaceName() const {
  return event_target_names::kWorker;
}

void DedicatedWorker::ContextLifecycleStateChanged(
    mojom::FrameLifecycleState state) {
  DCHECK(GetExecutionContext()->IsContextThread());
  switch (state) {
    case mojom::FrameLifecycleState::kPaused:
      // Do not do anything in this case. kPaused is only used
      // for when the main thread is paused we shouldn't worry
      // about pausing the worker thread in this case.
      break;
    case mojom::FrameLifecycleState::kFrozen:
    case mojom::FrameLifecycleState::kFrozenAutoResumeMedia:
      if (!requested_frozen_) {
        requested_frozen_ = true;
        context_proxy_->Freeze(
            GetExecutionContext()->is_in_back_forward_cache());
      }
      break;
    case mojom::FrameLifecycleState::kRunning:
      if (requested_frozen_) {
        context_proxy_->Resume();
        requested_frozen_ = false;
      }
      break;
  }
}

void DedicatedWorker::Trace(Visitor* visitor) const {
  visitor->Trace(options_);
  visitor->Trace(outside_fetch_client_settings_object_);
  visitor->Trace(context_proxy_);
  visitor->Trace(classic_script_loader_);
  AbstractWorker::Trace(visitor);
}

}  // namespace blink
