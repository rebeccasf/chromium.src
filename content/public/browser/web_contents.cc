// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/web_contents.h"

#include <utility>

#include "content/public/common/child_process_host.h"
#include "ipc/ipc_message.h"

namespace content {

bool WebContents::is_silent_printing() {
  return silent_printing_;
}

void WebContents::set_silent_printing(bool flag) {
  silent_printing_ = flag;
}

WebContents::CreateParams::CreateParams(BrowserContext* context,
                                        base::Location creator_location)
    : CreateParams(context, nullptr, creator_location) {}

WebContents::CreateParams::CreateParams(BrowserContext* context,
                                        scoped_refptr<SiteInstance> site,
                                        base::Location creator_location)
    : browser_context(context),
      site_instance(std::move(site)),
      creator_location(creator_location) {}

WebContents::CreateParams::CreateParams(const CreateParams& other) = default;

WebContents::CreateParams::~CreateParams() = default;

}  // namespace content
