/*
 * Copyright (C) 2011, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "third_party/blink/renderer/modules/gamepad/navigator_gamepad.h"

#include "base/auto_reset.h"
#include "device/gamepad/public/cpp/gamepad_features.h"
#include "device/gamepad/public/cpp/gamepads.h"
#include "third_party/blink/public/common/privacy_budget/identifiability_metric_builder.h"
#include "third_party/blink/public/common/privacy_budget/identifiability_study_settings.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/navigator.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/modules/gamepad/gamepad.h"
#include "third_party/blink/renderer/modules/gamepad/gamepad_comparisons.h"
#include "third_party/blink/renderer/modules/gamepad/gamepad_dispatcher.h"
#include "third_party/blink/renderer/modules/gamepad/gamepad_event.h"
#include "third_party/blink/renderer/platform/privacy_budget/identifiability_digest_helpers.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"

#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/dom/document.h"

namespace blink {
void fix_gamepad_nw(WebLocalFrame* frame) {
  Document* doc = frame->GetDocument();
  NavigatorGamepad* gamepad = &NavigatorGamepad::From(*(doc->domWindow()->navigator()));
  gamepad->SetWindow(doc->domWindow());
  gamepad->SampleAndCompareGamepadState();
}

namespace {

bool IsGamepadConnectionEvent(const AtomicString& event_type) {
  return event_type == event_type_names::kGamepadconnected ||
         event_type == event_type_names::kGamepaddisconnected;
}

bool HasConnectionEventListeners(LocalDOMWindow* window) {
  return window->HasEventListeners(event_type_names::kGamepadconnected) ||
         window->HasEventListeners(event_type_names::kGamepaddisconnected);
}

}  // namespace

// static
const char NavigatorGamepad::kSupplementName[] = "NavigatorGamepad";
const char kSecureContextBlocked[] =
    "Access to the feature \"gamepad\" requires a secure context";
const char kFeaturePolicyBlocked[] =
    "Access to the feature \"gamepad\" is disallowed by permissions policy.";
const char kFencedFrameBlocked[] =
    "getGamepad is not allowed in a fenced frame tree.";

NavigatorGamepad& NavigatorGamepad::From(Navigator& navigator) {
  NavigatorGamepad* supplement =
      Supplement<Navigator>::From<NavigatorGamepad>(navigator);
  if (!supplement) {
    supplement = MakeGarbageCollected<NavigatorGamepad>(navigator);
    ProvideTo(navigator, supplement);
  }
  return *supplement;
}

namespace {

void RecordGamepadsForIdentifiabilityStudy(
    ExecutionContext* context,
    HeapVector<Member<Gamepad>> gamepads) {
  if (!context || !IdentifiabilityStudySettings::Get()->ShouldSampleSurface(
                      IdentifiableSurface::FromTypeAndToken(
                          IdentifiableSurface::Type::kWebFeature,
                          WebFeature::kGetGamepads)))
    return;
  IdentifiableTokenBuilder builder;
  for (Gamepad* gp : gamepads) {
    if (gp) {
      builder.AddValue(gp->axes().size())
          .AddValue(gp->buttons().size())
          .AddValue(gp->connected())
          .AddToken(IdentifiabilityBenignStringToken(gp->id()))
          .AddToken(IdentifiabilityBenignStringToken(gp->mapping()))
          .AddValue(gp->timestamp());
      if (auto* vb = gp->vibrationActuator()) {
        builder.AddToken(IdentifiabilityBenignStringToken(vb->type()));
      }
    }
  }
  IdentifiabilityMetricBuilder(context->UkmSourceID())
      .AddWebFeature(WebFeature::kGetGamepads, builder.GetToken())
      .Record(context->UkmRecorder());
}

}  // namespace

// static
HeapVector<Member<Gamepad>> NavigatorGamepad::getGamepads(
    Navigator& navigator,
    ExceptionState& exception_state) {
  if (!navigator.DomWindow()) {
    // Using an existing NavigatorGamepad if one exists, but don't create one
    // for a detached window, as its subclasses depend on a non-null window.
    auto* gamepad = Supplement<Navigator>::From<NavigatorGamepad>(navigator);
    if (gamepad) {
      HeapVector<Member<Gamepad>> result = gamepad->Gamepads();
      RecordGamepadsForIdentifiabilityStudy(gamepad->GetExecutionContext(),
                                            result);
      return result;
    }
    return HeapVector<Member<Gamepad>>();
  }

  auto* navigator_gamepad = &NavigatorGamepad::From(navigator);

  // TODO(https://crbug.com/1011006): Remove fenced frame specific code when
  // permission policy implements the Gamepad API support.
  if (navigator.DomWindow()->GetFrame()->IsInFencedFrameTree()) {
    exception_state.ThrowSecurityError(kFencedFrameBlocked);
    return HeapVector<Member<Gamepad>>();
  }

  ExecutionContext* context = navigator_gamepad->GetExecutionContext();
  if (!context || !context->IsSecureContext()) {
    if (base::FeatureList::IsEnabled(::features::kRestrictGamepadAccess)) {
      exception_state.ThrowSecurityError(kSecureContextBlocked);
      return HeapVector<Member<Gamepad>>();
    } else {
      context->AddConsoleMessage(
          MakeGarbageCollected<ConsoleMessage>(
              mojom::blink::ConsoleMessageSource::kJavaScript,
              mojom::blink::ConsoleMessageLevel::kWarning,
              "getGamepad will now require Secure Context. "
              "Please update your application accordingly. "
              "For more information see "
              "https://github.com/w3c/gamepad/pull/120"),
          /*discard_duplicates=*/true);
    }
  }

  if (!context->IsFeatureEnabled(
          mojom::blink::PermissionsPolicyFeature::kGamepad)) {
    if (base::FeatureList::IsEnabled(::features::kRestrictGamepadAccess)) {
      exception_state.ThrowSecurityError(kFeaturePolicyBlocked);
      return HeapVector<Member<Gamepad>>();
    } else {
      context->AddConsoleMessage(
          MakeGarbageCollected<ConsoleMessage>(
              mojom::blink::ConsoleMessageSource::kJavaScript,
              mojom::blink::ConsoleMessageLevel::kWarning,
              "getGamepad will now require a Permission Policy. "
              "Please update your application accordingly. "
              "For more information see "
              "https://github.com/w3c/gamepad/pull/112"),
          /*discard_duplicates=*/true);
    }
  }

  HeapVector<Member<Gamepad>> result =
      NavigatorGamepad::From(navigator).Gamepads();
  RecordGamepadsForIdentifiabilityStudy(context, result);
  return result;
}

HeapVector<Member<Gamepad>> NavigatorGamepad::Gamepads() {
  SampleAndCompareGamepadState();

  // Ensure |gamepads_| is not null.
  if (gamepads_.size() == 0)
    gamepads_.resize(device::Gamepads::kItemsLengthCap);

  // Allow gamepad button presses to qualify as user activations if the page is
  // visible.
  if (DomWindow() && DomWindow()->GetFrame()->GetPage()->IsPageVisible() &&
      GamepadComparisons::HasUserActivation(gamepads_)) {
    LocalFrame::NotifyUserActivation(
        DomWindow()->GetFrame(),
        mojom::blink::UserActivationNotificationType::kInteraction);
  }
  is_gamepads_exposed_ = true;

  ExecutionContext* context = DomWindow();

  if (DomWindow() && DomWindow()->GetFrame()->IsCrossOriginToMainFrame()) {
    UseCounter::Count(context, WebFeature::kGetGamepadsFromCrossOriginSubframe);
  }

  if (context && !context->IsSecureContext()) {
    UseCounter::Count(context, WebFeature::kGetGamepadsFromInsecureContext);
  }

  return gamepads_;
}

void NavigatorGamepad::SampleGamepads() {
  device::Gamepads gamepads;
  gamepad_dispatcher_->SampleGamepads(gamepads);

  for (uint32_t i = 0; i < device::Gamepads::kItemsLengthCap; ++i) {
    device::Gamepad& device_gamepad = gamepads.items[i];

    // All WebXR gamepads should be hidden
    if (device_gamepad.is_xr) {
      gamepads_back_[i] = nullptr;
    } else if (device_gamepad.connected) {
      Gamepad* gamepad = gamepads_back_[i];
      if (!gamepad) {
        gamepad = MakeGarbageCollected<Gamepad>(this, i, navigation_start_,
                                                gamepads_start_);
      }
      bool cross_origin_isolated_capability =
          DomWindow() ? DomWindow()->CrossOriginIsolatedCapability() : false;
      gamepad->UpdateFromDeviceState(device_gamepad,
                                     cross_origin_isolated_capability);
      gamepads_back_[i] = gamepad;
    } else {
      gamepads_back_[i] = nullptr;
    }
  }
}

GamepadHapticActuator* NavigatorGamepad::GetVibrationActuatorForGamepad(
    const Gamepad& gamepad) {
  if (!gamepad.connected()) {
    return nullptr;
  }

  if (!gamepad.HasVibrationActuator()) {
    return nullptr;
  }

  int pad_index = gamepad.index();
  DCHECK_GE(pad_index, 0);
  if (!vibration_actuators_[pad_index]) {
    auto* actuator = MakeGarbageCollected<GamepadHapticActuator>(
        *DomWindow(), pad_index, gamepad.GetVibrationActuatorType());
    vibration_actuators_[pad_index] = actuator;
  }
  return vibration_actuators_[pad_index].Get();
}

void NavigatorGamepad::Trace(Visitor* visitor) const {
  visitor->Trace(gamepads_);
  visitor->Trace(gamepads_back_);
  visitor->Trace(vibration_actuators_);
  visitor->Trace(gamepad_dispatcher_);
  Supplement<Navigator>::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
  PlatformEventController::Trace(visitor);
  Gamepad::Client::Trace(visitor);
}

bool NavigatorGamepad::StartUpdatingIfAttached() {
  // The frame must be attached to start updating.
  if (!DomWindow()) {
    return false;
  }

  // TODO(https://crbug.com/1011006): Remove fenced frame specific code when
  // permission policy implements the Gamepad API support.
  if (DomWindow()->GetFrame()->IsInFencedFrameTree()) {
    return false;
  }

  StartUpdating();
  return true;
}

void NavigatorGamepad::DidUpdateData() {
  // We should stop listening once we detached.
  DCHECK(DomWindow());

  // Record when gamepad data was first made available to the page.
  if (gamepads_start_.is_null())
    gamepads_start_ = base::TimeTicks::Now();

  // Fetch the new gamepad state and dispatch gamepad events.
  if (has_event_listener_)
    SampleAndCompareGamepadState();
}

NavigatorGamepad::NavigatorGamepad(Navigator& navigator)
    : Supplement<Navigator>(navigator),
      ExecutionContextClient(navigator.DomWindow()),
      PlatformEventController(*navigator.DomWindow()),
      gamepad_dispatcher_(
          MakeGarbageCollected<GamepadDispatcher>(*navigator.DomWindow())) {
  navigator.DomWindow()->RegisterEventListenerObserver(this);

  // Fetch |window.performance.timing.navigationStart|. Gamepad timestamps are
  // reported relative to this value.
  auto& timing = DomWindow()->document()->Loader()->GetTiming();
  navigation_start_ = timing.NavigationStart();

  vibration_actuators_.resize(device::Gamepads::kItemsLengthCap);
}

NavigatorGamepad::~NavigatorGamepad() = default;

void NavigatorGamepad::RegisterWithDispatcher() {
  gamepad_dispatcher_->AddController(this, DomWindow());
}

void NavigatorGamepad::UnregisterWithDispatcher() {
  gamepad_dispatcher_->RemoveController(this);
}

bool NavigatorGamepad::HasLastData() {
  // Gamepad data is polled instead of pushed.
  return false;
}

void NavigatorGamepad::DidAddEventListener(LocalDOMWindow*,
                                           const AtomicString& event_type) {
  if (IsGamepadConnectionEvent(event_type)) {
    has_connection_event_listener_ = true;
    bool first_event_listener = !has_event_listener_;
    has_event_listener_ = true;

    if (GetPage() && GetPage()->IsPageVisible()) {
      StartUpdatingIfAttached();
      if (first_event_listener)
        SampleAndCompareGamepadState();
    }
  }
}

void NavigatorGamepad::DidRemoveEventListener(LocalDOMWindow* window,
                                              const AtomicString& event_type) {
  if (IsGamepadConnectionEvent(event_type)) {
    has_connection_event_listener_ = HasConnectionEventListeners(window);
    if (!has_connection_event_listener_)
      DidRemoveGamepadEventListeners();
  }
}

void NavigatorGamepad::DidRemoveAllEventListeners(LocalDOMWindow*) {
  DidRemoveGamepadEventListeners();
}

void NavigatorGamepad::DidRemoveGamepadEventListeners() {
  has_event_listener_ = false;
  StopUpdating();
}

void NavigatorGamepad::SampleAndCompareGamepadState() {
  // Avoid re-entry. Do not fetch a new sample until we are finished dispatching
  // events from the previous sample.
  if (processing_events_)
    return;

  base::AutoReset<bool> processing_events_reset(&processing_events_, true);
  if (StartUpdatingIfAttached()) {
    if (GetPage()->IsPageVisible()) {
      // Allocate a buffer to hold the new gamepad state, if needed.
      if (gamepads_back_.size() == 0)
        gamepads_back_.resize(device::Gamepads::kItemsLengthCap);
      SampleGamepads();

      // Compare the new sample with the previous sample and record which
      // gamepad events should be dispatched. Swap buffers if the gamepad
      // state changed. We must swap buffers before dispatching events to
      // ensure |gamepads_| holds the correct data when getGamepads is called
      // from inside a gamepad event listener.
      auto compare_result =
          GamepadComparisons::Compare(gamepads_, gamepads_back_, false, false);
      if (compare_result.IsDifferent()) {
        std::swap(gamepads_, gamepads_back_);
        bool is_gamepads_back_exposed = is_gamepads_exposed_;
        is_gamepads_exposed_ = false;

        // Dispatch gamepad events. Dispatching an event calls the event
        // listeners synchronously.
        //
        // Note: In some instances the gamepad connection state may change while
        // inside an event listener. This is most common when using test APIs
        // that allow the gamepad state to be changed from javascript. The set
        // of event listeners may also change if listeners are added or removed
        // by another listener.
        for (uint32_t i = 0; i < device::Gamepads::kItemsLengthCap; ++i) {
          bool is_connected = compare_result.IsGamepadConnected(i);
          bool is_disconnected = compare_result.IsGamepadDisconnected(i);

          // When a gamepad is disconnected and connected in the same update,
          // dispatch the gamepaddisconnected event first.
          if (has_connection_event_listener_ && is_disconnected) {
            // Reset the vibration state associated with the disconnected
            // gamepad to prevent it from being associated with a
            // newly-connected gamepad at the same index.
            vibration_actuators_[i] = nullptr;

            Gamepad* pad = gamepads_back_[i];
            DCHECK(pad);
            pad->SetConnected(false);
            is_gamepads_back_exposed = true;
            DispatchGamepadEvent(event_type_names::kGamepaddisconnected, pad);
          }
          if (has_connection_event_listener_ && is_connected) {
            Gamepad* pad = gamepads_[i];
            DCHECK(pad);
            is_gamepads_exposed_ = true;
            DispatchGamepadEvent(event_type_names::kGamepadconnected, pad);
          }
        }

        // Clear |gamepads_back_| if it was ever exposed to the page so it can
        // be garbage collected when no active references remain. If it was
        // never exposed, retain the buffer so it can be reused.
        if (is_gamepads_back_exposed)
          gamepads_back_.clear();
      }
    }
  }
}

void NavigatorGamepad::DispatchGamepadEvent(const AtomicString& event_name,
                                            Gamepad* gamepad) {
  // Ensure that we're blocking re-entrancy.
  DCHECK(processing_events_);
  DCHECK(has_connection_event_listener_);
  DCHECK(gamepad);
  DomWindow()->DispatchEvent(*GamepadEvent::Create(
      event_name, Event::Bubbles::kNo, Event::Cancelable::kYes, gamepad));
}

void NavigatorGamepad::PageVisibilityChanged() {
  // Inform the embedder whether it needs to provide gamepad data for us.
  bool visible = GetPage()->IsPageVisible();
  if (visible && (has_event_listener_ || gamepads_.size())) {
    StartUpdatingIfAttached();
  } else {
    StopUpdating();
  }

  if (visible && has_event_listener_)
    SampleAndCompareGamepadState();
}

}  // namespace blink
