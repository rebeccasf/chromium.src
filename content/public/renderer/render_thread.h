// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_RENDERER_RENDER_THREAD_H_
#define CONTENT_PUBLIC_RENDERER_RENDER_THREAD_H_

#include <stddef.h>
#include <stdint.h>
#include <memory>

#include "base/callback.h"
#include "base/metrics/user_metrics_action.h"
#include "base/task/single_thread_task_runner.h"
#include "base/tracing/protos/chrome_track_event.pbzero.h"
#include "content/common/content_export.h"
#include "content/public/child/child_thread.h"
#include "ipc/ipc_channel_proxy.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/perfetto/include/perfetto/tracing/traced_proto.h"

class GURL;

namespace base {
class UnguessableToken;
class WaitableEvent;
}

namespace blink {
class WebResourceRequestSenderDelegate;
struct UserAgentMetadata;

namespace scheduler {
enum class WebRendererProcessType;
}
}  // namespace blink

namespace IPC {
class MessageFilter;
class SyncChannel;
class SyncMessageFilter;
}  // namespace IPC

namespace v8 {
class Extension;
}  // namespace v8

namespace content {
class RenderThreadObserver;

class CONTENT_EXPORT RenderThread : virtual public ChildThread {
 public:
  // Returns the one render thread for this process.  Note that this can only
  // be accessed when running on the render thread itself.
  static RenderThread* Get();

  // Returns true if the current thread is the main thread.
  static bool IsMainThread();

  RenderThread();
  ~RenderThread() override;

  virtual IPC::SyncChannel* GetChannel() = 0;
  virtual std::string GetLocale() = 0;
  virtual IPC::SyncMessageFilter* GetSyncMessageFilter() = 0;

  // Called to add or remove a listener for a particular message routing ID.
  // These methods normally get delegated to a MessageRouter.
  virtual void AddRoute(int32_t routing_id, IPC::Listener* listener) = 0;
  // Attach a task runner to run received IPC tasks on for the given routing ID.
  // This must be called after the route has already been added via AddRoute(),
  // but it is optional. The default main thread task runner would be used if
  // this method is not called.
  virtual void AttachTaskRunnerToRoute(
      int32_t routing_id,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner) = 0;
  virtual void RemoveRoute(int32_t routing_id) = 0;
  virtual int GenerateRoutingID() = 0;
  virtual bool GenerateFrameRoutingID(
      int32_t& routing_id,
      blink::LocalFrameToken& frame_token,
      base::UnguessableToken& devtools_frame_token) = 0;

  // These map to IPC::ChannelProxy methods.
  virtual void AddFilter(IPC::MessageFilter* filter) = 0;
  virtual void RemoveFilter(IPC::MessageFilter* filter) = 0;

  // Add/remove observers for the process.
  virtual void AddObserver(RenderThreadObserver* observer) = 0;
  virtual void RemoveObserver(RenderThreadObserver* observer) = 0;

  // Set the WebResourceRequestSender delegate object for this process.
  // This does not take the ownership of the delegate. It is expected that the
  // delegate is kept alive while a request may be dispatched.
  virtual void SetResourceRequestSenderDelegate(
      blink::WebResourceRequestSenderDelegate* delegate) = 0;

  // Registers the given V8 extension with WebKit.
  virtual void RegisterExtension(std::unique_ptr<v8::Extension> extension) = 0;

  // Post task to all worker threads. Returns number of workers.
  virtual int PostTaskToAllWebWorkers(base::RepeatingClosure closure) = 0;

  // Resolve the proxy servers to use for a given url. On success true is
  // returned and |proxy_list| is set to a PAC string containing a list of
  // proxy servers.
  virtual bool ResolveProxy(const GURL& url, std::string* proxy_list) = 0;

  // Gets the shutdown event for the process.
  virtual base::WaitableEvent* GetShutdownEvent() = 0;

  // Retrieve the process ID of the browser process.
  virtual int32_t GetClientId() = 0;

  // Set the renderer process type.
  virtual void SetRendererProcessType(
      blink::scheduler::WebRendererProcessType type) = 0;

  // Returns the user-agent string.
  virtual blink::WebString GetUserAgent() = 0;
  virtual blink::WebString GetFullUserAgent() = 0;
  virtual blink::WebString GetReducedUserAgent() = 0;
  virtual const blink::UserAgentMetadata& GetUserAgentMetadata() = 0;

  // Write a representation of the current Renderer process into a trace.
  virtual void WriteIntoTrace(
      perfetto::TracedProto<perfetto::protos::pbzero::RenderProcessHost>
          proto) = 0;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_RENDERER_RENDER_THREAD_H_
