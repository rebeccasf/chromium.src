// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTED_FRAMES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTED_FRAMES_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "third_party/blink/renderer/core/frame/local_frame.h"

namespace blink {

class LocalFrame;

class CORE_EXPORT InspectedFrames final
    : public GarbageCollected<InspectedFrames> {
 public:
  class CORE_EXPORT Iterator {
    STACK_ALLOCATED();

   public:
    Iterator operator++(int);
    Iterator& operator++();
    bool operator==(const Iterator& other);
    bool operator!=(const Iterator& other);
    LocalFrame* operator*() { return current_; }
    LocalFrame* operator->() { return current_; }

   private:
    friend class InspectedFrames;
    Iterator(LocalFrame* root, LocalFrame* current);
    LocalFrame* root_;
    LocalFrame* current_;
  };

  explicit InspectedFrames(LocalFrame*);
  InspectedFrames(const InspectedFrames&) = delete;
  InspectedFrames& operator=(const InspectedFrames&) = delete;

  LocalFrame* Root() {
    LocalFrame* f = root_;
    LocalFrame* jail = (LocalFrame*)f->getDevtoolsJail();
    return jail ? jail : f;
  }
  bool Contains(LocalFrame*) const;
  LocalFrame* FrameWithSecurityOrigin(const String& origin_raw_string);
  Iterator begin();
  Iterator end();

  virtual void Trace(Visitor*) const;

 private:
  Member<LocalFrame> root_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTED_FRAMES_H_
