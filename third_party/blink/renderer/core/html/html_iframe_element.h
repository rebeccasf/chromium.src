/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2000 Simon Hausmann <hausmann@kde.org>
 * Copyright (C) 2004, 2006, 2008, 2009 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_IFRAME_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_IFRAME_ELEMENT_H_

#include "services/network/public/mojom/trust_tokens.mojom-blink-forward.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-blink.h"
#include "third_party/blink/public/common/permissions_policy/permissions_policy.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/html/html_frame_element_base.h"
#include "third_party/blink/renderer/core/html/html_iframe_element_sandbox.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {
class DOMFeaturePolicy;

class CORE_EXPORT HTMLIFrameElement : public HTMLFrameElementBase,
                                      public Supplementable<HTMLIFrameElement> {
  DEFINE_WRAPPERTYPEINFO();

 public:
  void Trace(Visitor*) const override;

  explicit HTMLIFrameElement(Document&);
  ~HTMLIFrameElement() override;

  DOMTokenList* sandbox() const;
  // Support JS introspection of frame policy (e.g. permissions policy)
  DOMFeaturePolicy* featurePolicy();

  // Returns attributes that should be checked against Trusted Types
  const AttrNameToTrustedType& GetCheckedAttributeTypes() const override;

  ParsedPermissionsPolicy ConstructContainerPolicy() const override;
  DocumentPolicyFeatureState ConstructRequiredPolicy() const override;

  FrameOwnerElementType OwnerType() const final {
    return FrameOwnerElementType::kIframe;
  }

 private:
  void SetCollapsed(bool) override;

  void ParseAttribute(const AttributeModificationParams&) override;
  bool IsPresentationAttribute(const QualifiedName&) const override;
  void CollectStyleForPresentationAttribute(
      const QualifiedName&,
      const AtomicString&,
      MutableCSSPropertyValueSet*) override;

  InsertionNotificationRequest InsertedInto(ContainerNode&) override;
  void RemovedFrom(ContainerNode&) override;

  bool LayoutObjectIsNeeded(const ComputedStyle&) const override;
  LayoutObject* CreateLayoutObject(const ComputedStyle&, LegacyLayout) override;

  bool IsInteractiveContent() const override;

  network::mojom::ReferrerPolicy ReferrerPolicyAttribute() override;

  network::mojom::blink::TrustTokenParamsPtr ConstructTrustTokenParams()
      const override;

  // FrameOwner overrides:
  bool AllowFullscreen() const override { return allow_fullscreen_; }
  bool AllowPaymentRequest() const override { return allow_payment_request_; }
  void DidChangeAttributes() override;
  AtomicString nwuseragent() const override { return nwuseragent_; }
  bool nwfaketop() const override { return nwfaketop_; }

  AtomicString nwuseragent_;
  AtomicString name_;
  AtomicString required_csp_;
  AtomicString allow_;
  AtomicString required_policy_;  // policy attribute
  // String attribute storing a JSON representation of the Trust Token
  // parameters (in order to align with the fetch interface to the Trust Token
  // API). If present, this is parsed in ConstructTrustTokenParams.
  AtomicString trust_token_;
  bool allow_fullscreen_;
  bool nwfaketop_;
  bool allow_payment_request_;
  bool collapsed_by_client_;
  bool anonymous_ = false;
  Member<HTMLIFrameElementSandbox> sandbox_;
  Member<DOMFeaturePolicy> policy_;

  network::mojom::ReferrerPolicy referrer_policy_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_IFRAME_ELEMENT_H_
