// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/payments/content/payment_request.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/containers/contains.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "components/payments/content/can_make_payment_query_factory.h"
#include "components/payments/content/content_payment_request_delegate.h"
#include "components/payments/content/payment_app.h"
#include "components/payments/content/payment_details_converter.h"
#include "components/payments/content/payment_request_converter.h"
#include "components/payments/content/secure_payment_confirmation_no_creds.h"
#include "components/payments/core/can_make_payment_query.h"
#include "components/payments/core/error_message_util.h"
#include "components/payments/core/error_strings.h"
#include "components/payments/core/features.h"
#include "components/payments/core/method_strings.h"
#include "components/payments/core/native_error_strings.h"
#include "components/payments/core/payment_details.h"
#include "components/payments/core/payment_details_validation.h"
#include "components/payments/core/payment_prefs.h"
#include "components/payments/core/payment_request_delegate.h"
#include "components/payments/core/payments_experimental_features.h"
#include "components/payments/core/payments_validators.h"
#include "components/payments/core/url_util.h"
#include "components/prefs/pref_service.h"
#include "components/ukm/content/source_url_recorder.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"

namespace payments {
namespace {

using ::payments::mojom::CanMakePaymentQueryResult;
using ::payments::mojom::HasEnrolledInstrumentQueryResult;

// Redact shipping address before exposing it in ShippingAddressChangeEvent.
// https://w3c.github.io/payment-request/#shipping-address-changed-algorithm
mojom::PaymentAddressPtr RedactShippingAddress(
    mojom::PaymentAddressPtr address) {
  DCHECK(address);
  if (!PaymentsExperimentalFeatures::IsEnabled(
          features::kWebPaymentsRedactShippingAddress)) {
    return address;
  }
  address->organization.clear();
  address->phone.clear();
  address->recipient.clear();
  address->address_line.clear();
  return address;
}
}  // namespace

PaymentRequest::PaymentRequest(
    content::RenderFrameHost* render_frame_host,
    std::unique_ptr<ContentPaymentRequestDelegate> delegate,
    base::WeakPtr<PaymentRequestDisplayManager> display_manager,
    mojo::PendingReceiver<mojom::PaymentRequest> receiver,
    SPCTransactionMode spc_transaction_mode,
    base::WeakPtr<ObserverForTest> observer_for_testing)
    : DocumentService(render_frame_host, std::move(receiver)),
      WebContentsObserver(
          content::WebContents::FromRenderFrameHost(render_frame_host)),
      log_(web_contents()),
      delegate_(std::move(delegate)),
      display_manager_(display_manager),
      display_handle_(nullptr),
      top_level_origin_(url_formatter::FormatUrlForSecurityDisplay(
          web_contents()->GetLastCommittedURL())),
      frame_origin_(url_formatter::FormatUrlForSecurityDisplay(
          render_frame_host->GetLastCommittedURL())),
      frame_security_origin_(render_frame_host->GetLastCommittedOrigin()),
      spc_transaction_mode_(spc_transaction_mode),
      observer_for_testing_(observer_for_testing),
      journey_logger_(delegate_->IsOffTheRecord(),
                      ukm::GetSourceIdForWebContentsDocument(web_contents())) {
  payment_handler_host_ = std::make_unique<PaymentHandlerHost>(
      web_contents(), weak_ptr_factory_.GetWeakPtr());
}

PaymentRequest::~PaymentRequest() {
  client_.reset();
  payment_handler_host_->Disconnect();
  delegate_->CloseDialog();
  display_handle_.reset();
  if (observer_for_testing_)
    observer_for_testing_->OnConnectionTerminated();

  // If another reason wasn't recorded, we were self-deleted, along with closing
  // the mojo connection. We just self-delete immediately instead of waiting
  // for the round trip of reporting an error to the renderer, but we report it
  // as if we did wait for the round trip.
  RecordFirstAbortReason(JourneyLogger::ABORT_REASON_MOJO_CONNECTION_ERROR);
}

void PaymentRequest::Init(
    mojo::PendingRemote<mojom::PaymentRequestClient> client,
    std::vector<mojom::PaymentMethodDataPtr> method_data,
    mojom::PaymentDetailsPtr details,
    mojom::PaymentOptionsPtr options) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (is_initialized_) {
    log_.Error(errors::kAttemptedInitializationTwice);
    delete this;
    return;
  }

  journey_logger_.RecordCheckoutStep(
      JourneyLogger::CheckoutFunnelStep::kInitiated);
  is_initialized_ = true;
  client_.Bind(std::move(client));

  const GURL last_committed_url = delegate_->GetLastCommittedURL();
  if (!network::IsUrlPotentiallyTrustworthy(last_committed_url)) {
    log_.Error(errors::kNotInASecureOrigin);
    delete this;
    return;
  }

  bool allowed_origin = last_committed_url.SchemeIs("chrome-extension") ||
      UrlUtil::IsOriginAllowedToUseWebPaymentApis(last_committed_url);

  if (!allowed_origin) {
    reject_show_error_message_ = errors::kProhibitedOrigin;
  }

  bool invalid_ssl = false;
  if (last_committed_url.SchemeIsCryptographic()) {
    DCHECK(reject_show_error_message_.empty());
    reject_show_error_message_ =
        delegate_->GetInvalidSslCertificateErrorMessage();
    invalid_ssl = !reject_show_error_message_.empty();
  }

  if (!allowed_origin || invalid_ssl) {
    // Intentionally don't set |spec_| and |state_|, so the UI is never shown.
    log_.Error(reject_show_error_message_);
    log_.Error(errors::kProhibitedOriginOrInvalidSslExplanation);
    client_->OnError(
        mojom::PaymentErrorReason::NOT_SUPPORTED_FOR_INVALID_ORIGIN_OR_SSL,
        reject_show_error_message_);
    delete this;
    return;
  }

  if (method_data.empty()) {
    log_.Error(errors::kMethodDataRequired);
    delete this;
    return;
  }

  if (std::any_of(method_data.begin(), method_data.end(),
                  [](const auto& datum) {
                    return !datum || datum->supported_method.empty();
                  })) {
    log_.Error(errors::kMethodNameRequired);
    delete this;
    return;
  }

  if (!details || !details->id || !details->total) {
    log_.Error(errors::kInvalidPaymentDetails);
    delete this;
    return;
  }

  if (!options) {
    log_.Error(errors::kInvalidPaymentOptions);
    delete this;
    return;
  }

  std::string error;
  if (!ValidatePaymentDetails(ConvertPaymentDetails(details), &error)) {
    log_.Error(error);
    delete this;
    return;
  }

  spec_ = std::make_unique<PaymentRequestSpec>(
      std::move(options), std::move(details), std::move(method_data),
      /*observer=*/weak_ptr_factory_.GetWeakPtr(),
      delegate_->GetApplicationLocale());
  state_ = std::make_unique<PaymentRequestState>(
      render_frame_host(), top_level_origin_, frame_origin_,
      frame_security_origin_, spec(),
      /*delegate=*/weak_ptr_factory_.GetWeakPtr(),
      delegate_->GetApplicationLocale(), delegate_->GetPersonalDataManager(),
      delegate_->GetContentWeakPtr(), journey_logger_.GetWeakPtr());

  journey_logger_.SetRequestedInformation(
      spec_->request_shipping(), spec_->request_payer_email(),
      spec_->request_payer_phone(), spec_->request_payer_name());

  // Log metrics around which payment methods are requested by the merchant.
  GURL google_pay_url(methods::kGooglePay);
  GURL android_pay_url(methods::kAndroidPay);
  GURL google_play_billing_url(methods::kGooglePlayBilling);
  // Looking for payment methods that are NOT google-related payment methods.
  auto non_google_it =
      std::find_if(spec_->url_payment_method_identifiers().begin(),
                   spec_->url_payment_method_identifiers().end(),
                   [google_pay_url, android_pay_url,
                    google_play_billing_url](const GURL& url) {
                     return url != google_pay_url && url != android_pay_url &&
                            url != google_play_billing_url;
                   });
  std::vector<JourneyLogger::PaymentMethodCategory> method_categories;
  // Note that only a test can add autofill payment apps when basic-card
  // feature is disabled.
  if (base::FeatureList::IsEnabled(::features::kPaymentRequestBasicCard) &&
      !spec_->supported_card_networks().empty()) {
    method_categories.push_back(
        JourneyLogger::PaymentMethodCategory::kBasicCard);
  }
  if (base::Contains(spec_->url_payment_method_identifiers(), google_pay_url) ||
      base::Contains(spec_->url_payment_method_identifiers(),
                     android_pay_url)) {
    method_categories.push_back(JourneyLogger::PaymentMethodCategory::kGoogle);
  }
  if (base::Contains(spec_->url_payment_method_identifiers(),
                     google_play_billing_url)) {
    method_categories.push_back(
        JourneyLogger::PaymentMethodCategory::kPlayBilling);
  }
  if (spec_->IsSecurePaymentConfirmationRequested()) {
    method_categories.push_back(
        JourneyLogger::PaymentMethodCategory::kSecurePaymentConfirmation);
  }
  if (non_google_it != spec_->url_payment_method_identifiers().end()) {
    method_categories.push_back(JourneyLogger::PaymentMethodCategory::kOther);
  }
  journey_logger_.SetRequestedPaymentMethods(method_categories);

  payment_handler_host_->set_payment_request_id_for_logs(*spec_->details().id);

  if (spec_->IsSecurePaymentConfirmationRequested()) {
    delegate_->set_dialog_type(
        PaymentRequestDelegate::DialogType::SECURE_PAYMENT_CONFIRMATION);
  }

  if (VLOG_IS_ON(2)) {
    std::vector<std::string> payment_method_identifiers(
        spec_->payment_method_identifiers_set().begin(),
        spec_->payment_method_identifiers_set().end());
    std::string total = spec_->details().total
                            ? (spec_->details().total->amount->currency +
                               spec_->details().total->amount->value)
                            : "N/A";
    VLOG(2) << "Initialized PaymentRequest (" << *spec_->details().id << ")"
            << "\n    Top origin: " << top_level_origin_.spec()
            << "\n    Frame origin: " << frame_origin_.spec()
            << "\n    Requested methods: "
            << base::JoinString(payment_method_identifiers, ", ")
            << "\n    Total: " << total
            << "\n    Options: shipping = " << spec_->request_shipping()
            << ", name = " << spec_->request_payer_name()
            << ", phone = " << spec_->request_payer_phone()
            << ", email = " << spec_->request_payer_email();
  }
}

void PaymentRequest::Show(bool is_user_gesture, bool wait_for_updated_details) {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotShowWithoutInit);
    delete this;
    return;
  }

  if (is_show_called_) {
    log_.Error(errors::kCannotShowTwice);
    delete this;
    return;
  }

  journey_logger_.RecordCheckoutStep(
      JourneyLogger::CheckoutFunnelStep::kShowCalled);
  is_show_called_ = true;
  journey_logger_.SetTriggerTime();

  // A tab can display only one PaymentRequest UI at a time.
  if (display_manager_)
    display_handle_ = display_manager_->TryShow(delegate_->GetContentWeakPtr());

  if (!display_handle_) {
    log_.Error(errors::kAnotherUiShowing);
    DCHECK(!has_recorded_completion_);
    has_recorded_completion_ = true;
    journey_logger_.SetNotShown(
        JourneyLogger::NOT_SHOWN_REASON_CONCURRENT_REQUESTS);
    client_->OnError(mojom::PaymentErrorReason::ALREADY_SHOWING,
                     errors::kAnotherUiShowing);
    delete this;
    return;
  }

#if 0
  if (!delegate_->IsBrowserWindowActive()) {
    log_.Error(errors::kCannotShowInBackgroundTab);
    DCHECK(!has_recorded_completion_);
    has_recorded_completion_ = true;
    journey_logger_.SetNotShown(JourneyLogger::NOT_SHOWN_REASON_OTHER);
    client_->OnError(mojom::PaymentErrorReason::USER_CANCEL,
                     errors::kCannotShowInBackgroundTab);
    delete this;
    return;
  }
#endif

  is_show_user_gesture_ = is_user_gesture;

  if (wait_for_updated_details) {
    // Put |spec_| into uninitialized state, so the UI knows to show a spinner.
    // This method does not block.
    spec_->StartWaitingForUpdateWith(
        PaymentRequestSpec::UpdateReason::INITIAL_PAYMENT_DETAILS);
    spec_->AddInitializationObserver(this);
  } else {
    DCHECK(spec_->details().total);
    journey_logger_.RecordTransactionAmount(
        spec_->details().total->amount->currency,
        spec_->details().total->amount->value, false /*completed*/);
  }

  // If an app store billing payment method is one of the payment methods being
  // requested, then don't show any user interface until its known whether it's
  // possible to skip UI directly into an app store billing payment app.
  if (!spec_->IsAppStoreBillingAlsoRequested())
    display_handle_->Show(weak_ptr_factory_.GetWeakPtr());

  state_->set_is_show_user_gesture(is_show_user_gesture_);
  state_->AreRequestedMethodsSupported(
      base::BindOnce(&PaymentRequest::AreRequestedMethodsSupportedCallback,
                     weak_ptr_factory_.GetWeakPtr()));
}

void PaymentRequest::Retry(mojom::PaymentValidationErrorsPtr errors) {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotRetryWithoutInit);
    delete this;
    return;
  }

  if (!IsThisPaymentRequestShowing()) {
    log_.Error(errors::kCannotRetryWithoutShow);
    delete this;
    return;
  }

  std::string error;
  if (!PaymentsValidators::IsValidPaymentValidationErrorsFormat(errors,
                                                                &error)) {
    log_.Error(error);
    client_->OnError(mojom::PaymentErrorReason::USER_CANCEL, error);
    delete this;
    return;
  }

  VLOG(2) << "PaymentRequest (" << *spec_->details().id
          << ") retry with error: " << error;

  state()->SetAvailablePaymentAppForRetry();
  spec()->Retry(std::move(errors));
  display_handle_->Retry();
}

void PaymentRequest::UpdateWith(mojom::PaymentDetailsPtr details) {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotUpdateWithoutInit);
    delete this;
    return;
  }

  if (!IsThisPaymentRequestShowing()) {
    log_.Error(errors::kCannotUpdateWithoutShow);
    delete this;
    return;
  }

  // ID cannot be updated. Updating the total is optional.
  if (!details || details->id) {
    log_.Error(errors::kInvalidPaymentDetails);
    delete this;
    return;
  }

  std::string error;
  if (!ValidatePaymentDetails(ConvertPaymentDetails(details), &error)) {
    log_.Error(error);
    delete this;
    return;
  }

  if (details->shipping_address_errors &&
      !PaymentsValidators::IsValidAddressErrorsFormat(
          details->shipping_address_errors, &error)) {
    log_.Error(error);
    delete this;
    return;
  }

  if (state()->selected_app() && state()->IsPaymentAppInvoked() &&
      state()->selected_app()->IsWaitingForPaymentDetailsUpdate()) {
    state()->selected_app()->UpdateWith(
        PaymentDetailsConverter::ConvertToPaymentRequestDetailsUpdate(
            details, state()->selected_app()->HandlesShippingAddress(),
            base::BindRepeating(&PaymentApp::IsValidForPaymentMethodIdentifier,
                                state()->selected_app()->AsWeakPtr())));
  }

  bool is_resolving_promise_passed_into_show_method = !spec_->IsInitialized();

  spec_->UpdateWith(std::move(details));

  if (is_resolving_promise_passed_into_show_method) {
    DCHECK(spec_->details().total);
    journey_logger_.RecordTransactionAmount(
        spec_->details().total->amount->currency,
        spec_->details().total->amount->value, false /*completed*/);
    if (is_requested_methods_supported_invoked_) {
      if (SatisfiesSkipUIConstraints()) {
        Pay();
      } else {
        // If not skipping UI, then make sure that the browser payment sheet is
        // being displayed.
        if (!display_handle_->was_shown())
          display_handle_->Show(weak_ptr_factory_.GetWeakPtr());

        if (spec_->request_shipping())
          state_->SelectDefaultShippingAddressAndNotifyObservers();
      }
    }
  }
}

void PaymentRequest::OnPaymentDetailsNotUpdated() {
  // This Mojo call is triggered by the user of the API doing nothing in
  // response to a shipping address update event, so the error messages cannot
  // be more verbose.
  if (!IsInitialized()) {
    log_.Error(errors::kNotInitialized);
    delete this;
    return;
  }

  if (!IsThisPaymentRequestShowing()) {
    log_.Error(errors::kNotShown);
    delete this;
    return;
  }

  spec_->RecomputeSpecForDetails();

  if (state()->IsPaymentAppInvoked() && state()->selected_app() &&
      state()->selected_app()->IsWaitingForPaymentDetailsUpdate()) {
    state()->selected_app()->OnPaymentDetailsNotUpdated();
  }
}

void PaymentRequest::Abort() {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotAbortWithoutInit);
    delete this;
    return;
  }

  if (!IsThisPaymentRequestShowing()) {
    log_.Error(errors::kCannotAbortWithoutShow);
    delete this;
    return;
  }

  // The API user has decided to abort. If a successful abort message is
  // returned to the renderer, the Mojo message pipe is closed, which triggers
  // the destruction of this object.
  // Otherwise, the abort promise is rejected and the pipe is not closed.
  // The abort is only successful if the payment app wasn't yet invoked.
  // TODO(crbug.com/716546): Add a merchant abort metric

  if (observer_for_testing_)
    observer_for_testing_->OnAbortCalled();

  if (!state_->IsPaymentAppInvoked() || !state_->selected_app()) {
    OnAbortResult(/*aborted=*/true);
    return;
  }

  state_->selected_app()->AbortPaymentApp(base::BindOnce(
      &PaymentRequest::OnAbortResult, weak_ptr_factory_.GetWeakPtr()));
}

void PaymentRequest::Complete(mojom::PaymentComplete result) {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotCompleteWithoutInit);
    delete this;
    return;
  }

  if (!IsThisPaymentRequestShowing()) {
    log_.Error(errors::kCannotAbortWithoutShow);
    delete this;
    return;
  }

  if (observer_for_testing_) {
    observer_for_testing_->OnCompleteCalled();
  }

  // Failed transactions show an error. Successful and unknown-state
  // transactions don't show an error.
  if (result == mojom::PaymentComplete::FAIL) {
    ShowErrorMessageAndAbortPayment();
  } else {
    DCHECK(!has_recorded_completion_);
    journey_logger_.SetCompleted();
    has_recorded_completion_ = true;
    DCHECK(spec_->details().total);
    journey_logger_.RecordTransactionAmount(
        spec_->details().total->amount->currency,
        spec_->details().total->amount->value, true /*completed*/);

    delegate_->GetPrefService()->SetBoolean(kPaymentsFirstTransactionCompleted,
                                            true);
    // When the renderer closes the connection this object will be destroyed.
    client_->OnComplete();
    state_->RecordUseStats();
  }
}

void PaymentRequest::CanMakePayment() {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotCallCanMakePaymentWithoutInit);
    delete this;
    return;
  }

  // It's valid to call canMakePayment() without calling show() first.

  if (observer_for_testing_)
    observer_for_testing_->OnCanMakePaymentCalled();

  if (!delegate_->GetPrefService()->GetBoolean(kCanMakePaymentEnabled)) {
    CanMakePaymentCallback(/*can_make_payment=*/false);
  } else {
    state_->CanMakePayment(
        base::BindOnce(&PaymentRequest::CanMakePaymentCallback,
                       weak_ptr_factory_.GetWeakPtr()));
  }
}

void PaymentRequest::HasEnrolledInstrument() {
  if (!IsInitialized()) {
    log_.Error(errors::kCannotCallHasEnrolledInstrumentWithoutInit);
    delete this;
    return;
  }

  // It's valid to call hasEnrolledInstrument() without calling show() first.

  if (observer_for_testing_)
    observer_for_testing_->OnHasEnrolledInstrumentCalled();

  if (!delegate_->GetPrefService()->GetBoolean(kCanMakePaymentEnabled)) {
    HasEnrolledInstrumentCallback(/*has_enrolled_instrument=*/false);
  } else {
    state_->HasEnrolledInstrument(
        base::BindOnce(&PaymentRequest::HasEnrolledInstrumentCallback,
                       weak_ptr_factory_.GetWeakPtr()));
  }
}

bool PaymentRequest::ChangePaymentMethod(const std::string& method_name,
                                         const std::string& stringified_data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!method_name.empty());

  if (!state_ || !state_->IsPaymentAppInvoked() || !client_)
    return false;

  client_->OnPaymentMethodChange(method_name, stringified_data);
  return true;
}

bool PaymentRequest::ChangeShippingOption(
    const std::string& shipping_option_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!shipping_option_id.empty());

  bool is_valid_id = false;
  if (spec_->details().shipping_options) {
    for (const auto& option : spec_->GetShippingOptions()) {
      if (option->id == shipping_option_id) {
        is_valid_id = true;
        break;
      }
    }
  }

  if (!state_ || !state_->IsPaymentAppInvoked() || !client_ || !spec_ ||
      !spec_->request_shipping() || !is_valid_id) {
    return false;
  }

  client_->OnShippingOptionChange(shipping_option_id);
  return true;
}

bool PaymentRequest::ChangeShippingAddress(
    mojom::PaymentAddressPtr shipping_address) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(shipping_address);

  if (!state_ || !state_->IsPaymentAppInvoked() || !client_ || !spec_ ||
      !spec_->request_shipping()) {
    return false;
  }

  client_->OnShippingAddressChange(
      RedactShippingAddress(std::move(shipping_address)));
  return true;
}

void PaymentRequest::AreRequestedMethodsSupportedCallback(
    bool methods_supported,
    const std::string& error_message,
    AppCreationFailureReason error_reason) {
  is_requested_methods_supported_invoked_ = true;
  if (is_show_called_ && spec_ && spec_->IsInitialized() &&
      observer_for_testing_) {
    observer_for_testing_->OnAppListReady(weak_ptr_factory_.GetWeakPtr());
  }

  if (render_frame_host()->IsActive() &&
      spec_->IsSecurePaymentConfirmationRequested() &&
      state()->available_apps().empty() &&
      base::FeatureList::IsEnabled(::features::kSecurePaymentConfirmation) &&
      // In most cases, we show the 'No Matching Payment Credential' dialog in
      // order to preserve user privacy. An exception is failure to download the
      // card art icon - because we download it in all cases, revealing a
      // failure doesn't leak any information about the user to the site.
      error_reason != AppCreationFailureReason::ICON_DOWNLOAD_FAILED) {
    delegate_->ShowNoMatchingPaymentCredentialDialog(
        url_formatter::FormatUrlForSecurityDisplay(
            state_->GetTopOrigin(),
            url_formatter::SchemeDisplay::OMIT_CRYPTOGRAPHIC),
        base::BindOnce(&PaymentRequest::OnUserCancelled,
                       weak_ptr_factory_.GetWeakPtr()));
    if (observer_for_testing_)
      observer_for_testing_->OnErrorDisplayed();
    return;
  }

  if (methods_supported) {
    if (SatisfiesSkipUIConstraints()) {
      Pay();
    } else if (!display_handle_->was_shown()) {
      // If not skipping UI, then make sure that the browser payment sheet is
      // being displayed.
      display_handle_->Show(weak_ptr_factory_.GetWeakPtr());
    }
  } else {
    VLOG(2) << "PaymentRequest (" << *spec_->details().id
            << "): requested method not supported.";
    DCHECK(!has_recorded_completion_);
    has_recorded_completion_ = true;
    journey_logger_.SetNotShown(
        JourneyLogger::NOT_SHOWN_REASON_NO_SUPPORTED_PAYMENT_METHOD);
    client_->OnError(mojom::PaymentErrorReason::NOT_SUPPORTED,
                     GetNotSupportedErrorMessage(
                         spec_ ? spec_->payment_method_identifiers_set()
                               : std::set<std::string>()) +
                         (error_message.empty() ? "" : " " + error_message));
    if (observer_for_testing_)
      observer_for_testing_->OnNotSupportedError();
    delete this;
  }
}

base::WeakPtr<PaymentRequest> PaymentRequest::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

void PaymentRequest::OnInitialized(InitializationTask* initialization_task) {
  DCHECK_EQ(spec_.get(), initialization_task);
  DCHECK_EQ(PaymentRequestSpec::UpdateReason::INITIAL_PAYMENT_DETAILS,
            spec_->current_update_reason());
  if (is_show_called_ && state_ && state_->is_get_all_apps_finished() &&
      observer_for_testing_) {
    observer_for_testing_->OnAppListReady(weak_ptr_factory_.GetWeakPtr());
  }
}

bool PaymentRequest::IsInitialized() const {
  return is_initialized_ && client_ && client_.is_bound() && state_ && spec_;
}

bool PaymentRequest::IsThisPaymentRequestShowing() const {
  return is_show_called_ && display_handle_ && spec_ && state_;
}

bool PaymentRequest::OnlySingleAppCanProvideAllRequiredInformation() const {
  DCHECK(state()->IsInitialized());
  DCHECK(spec()->IsInitialized());

  if (!spec()->request_shipping() && !spec()->request_payer_name() &&
      !spec()->request_payer_phone() && !spec()->request_payer_email()) {
    return state()->available_apps().size() == 1 &&
           state()->available_apps().at(0)->type() !=
               PaymentApp::Type::AUTOFILL;
  }

  bool an_app_can_provide_all_info = false;
  for (const auto& app : state()->available_apps()) {
    if ((!spec()->request_shipping() || app->HandlesShippingAddress()) &&
        (!spec()->request_payer_name() || app->HandlesPayerName()) &&
        (!spec()->request_payer_phone() || app->HandlesPayerPhone()) &&
        (!spec()->request_payer_email() || app->HandlesPayerEmail())) {
      // There is another available app that can provide all merchant requested
      // information information.
      if (an_app_can_provide_all_info)
        return false;

      an_app_can_provide_all_info = true;
    }
  }
  return an_app_can_provide_all_info;
}

bool PaymentRequest::SatisfiesSkipUIConstraints() {
  // Only allowing URL based payment apps to skip the payment sheet.
  skipped_payment_request_ui_ =
      !spec()->IsSecurePaymentConfirmationRequested() &&
      (spec()->url_payment_method_identifiers().size() > 0 ||
       delegate_->SkipUiForBasicCard()) &&
      base::FeatureList::IsEnabled(features::kWebPaymentsSingleAppUiSkip) &&
      base::FeatureList::IsEnabled(::features::kServiceWorkerPaymentApps) &&
      is_show_user_gesture_ && state()->IsInitialized() &&
      spec()->IsInitialized() &&
      OnlySingleAppCanProvideAllRequiredInformation() &&
      // The available app should be preselectable.
      state()->selected_app() != nullptr;
  if (skipped_payment_request_ui_) {
    DCHECK(state()->IsInitialized() && spec()->IsInitialized());
    journey_logger_.SetSkippedShow();
  } else if (state()->IsInitialized() && spec()->IsInitialized()) {
    // Set "shown" only after state() and spec() initialization.
    journey_logger_.SetShown();
  }
  return skipped_payment_request_ui_;
}

void PaymentRequest::OnPaymentResponseAvailable(
    mojom::PaymentResponsePtr response) {
  DCHECK(!response->method_name.empty());
  DCHECK(!response->stringified_details.empty());

  journey_logger_.SetReceivedInstrumentDetails();

  // If currently interactive, show the processing spinner. Autofill payment
  // apps request a CVC, so they are always interactive at this point. A payment
  // handler may elect to be non-interactive by not showing a confirmation page
  // to the user.
  if (delegate_->IsInteractive())
    delegate_->ShowProcessingSpinner();

  client_->OnPaymentResponse(std::move(response));
}

void PaymentRequest::OnPaymentResponseError(const std::string& error_message) {
  journey_logger_.SetReceivedInstrumentDetails();
  RecordFirstAbortReason(JourneyLogger::ABORT_REASON_INSTRUMENT_DETAILS_ERROR);

  reject_show_error_message_ = error_message;
  ShowErrorMessageAndAbortPayment();
}

void PaymentRequest::OnShippingOptionIdSelected(
    std::string shipping_option_id) {
  client_->OnShippingOptionChange(shipping_option_id);
}

void PaymentRequest::OnShippingAddressSelected(
    mojom::PaymentAddressPtr address) {
  client_->OnShippingAddressChange(RedactShippingAddress(std::move(address)));
}

void PaymentRequest::OnPayerInfoSelected(mojom::PayerDetailPtr payer_info) {
  client_->OnPayerDetailChange(std::move(payer_info));
}

void PaymentRequest::OnUserCancelled() {
  // If |client_| is not bound, then the object is already being destroyed as
  // a result of a renderer event.
  if (!client_.is_bound())
    return;

  RecordFirstAbortReason(JourneyLogger::ABORT_REASON_ABORTED_BY_USER);

  // This sends an error to the renderer, which informs the API user.
  // If SPC flag is enabled, use NotAllowedError instead.
  bool is_spc_enabled = spec_->IsSecurePaymentConfirmationRequested();
  client_->OnError(
      is_spc_enabled ? mojom::PaymentErrorReason::NOT_ALLOWED_ERROR
                     : mojom::PaymentErrorReason::USER_CANCEL,
      is_spc_enabled
          ? errors::kWebAuthnOperationTimedOutOrNotAllowed
          : (!reject_show_error_message_.empty() ? reject_show_error_message_
                                                 : errors::kUserCancelled));
  delete this;
}

void PaymentRequest::ReadyToCommitNavigation(
    content::NavigationHandle* navigation_handle) {
  auto navigation_in_frame_will_destroy_or_cache_document_in_frame =
      [](content::GlobalRenderFrameHostId previous_frame_id,
         content::RenderFrameHost* frame) {
        if (!previous_frame_id)
          return false;

        // If a navigation to a new document is happening inside this frame, or
        // an ancestor, then the current document will be gone shortly. We have
        // to look at the `previous_frame_id` as the navigation may be occurring
        // in a new RenderFrameHost, replacing the current RenderFrameHost.
        for (; frame; frame = frame->GetParentOrOuterDocument()) {
          if (frame->GetGlobalId() == previous_frame_id)
            return true;
        }
        return false;
      };

  // This method watches for cross-document navigations that would lead to the
  // PaymentRequest being destroyed in the future; and it wants to track if such
  // a navigation is browser- or renderer-initiated.
  //
  // We could track that as a state on PaymentRequest that is used at time of
  // destruction, but we instead just record the metrics event here, which has a
  // slight chance of being incorrect - for instance if the tab is torn down
  // instead of completing the navigation.

  // This checks if the upcoming navigation would destroy (or put into the
  // BackForwardCache) the current document in `render_frame_host()`, which the
  // PaymentRequest is attached to.
  if (!navigation_in_frame_will_destroy_or_cache_document_in_frame(
          navigation_handle->GetPreviousRenderFrameHostId(),
          render_frame_host())) {
    return;
  }

  // Since the PaymentRequest dialog blocks the content of the WebContents,
  // the user cannot click on a link to navigate away. Therefore, if the
  // navigation is initiated in the renderer, it does not come from the user.
  bool is_user_initiated = !navigation_handle->IsRendererInitiated();
  RecordFirstAbortReason(is_user_initiated
                             ? JourneyLogger::ABORT_REASON_USER_NAVIGATION
                             : JourneyLogger::ABORT_REASON_MERCHANT_NAVIGATION);
}

void PaymentRequest::WillBeDestroyed(
    content::DocumentServiceDestructionReason reason) {
  switch (reason) {
    case content::DocumentServiceDestructionReason::kConnectionTerminated:
      RecordFirstAbortReason(JourneyLogger::ABORT_REASON_MOJO_CONNECTION_ERROR);
      break;
    case content::DocumentServiceDestructionReason::kEndOfDocumentLifetime:
      // RenderFrameHost is usually deleted explicitly before PaymentRequest
      // destruction if the user closes the tab or browser window without
      // closing the payment request dialog.
      RecordFirstAbortReason(JourneyLogger::ABORT_REASON_ABORTED_BY_USER);
      break;
  }
}

void PaymentRequest::Pay() {
  journey_logger_.SetPayClicked();
  journey_logger_.RecordCheckoutStep(
      JourneyLogger::CheckoutFunnelStep::kPaymentHandlerInvoked);
  DCHECK(state_->selected_app());
  VLOG(2) << "PaymentRequest (" << *spec_->details().id
          << "): paying with app: " << state_->selected_app()->GetLabel();

  if (!display_handle_->was_shown() &&
      state_->selected_app()->type() != PaymentApp::Type::NATIVE_MOBILE_APP) {
    // If not paying with a native mobile app (such as app store billing), then
    // make sure that the browser payment sheet is being displayed.
    display_handle_->Show(weak_ptr_factory_.GetWeakPtr());
  }

  // Log the correct "selected method".
  journey_logger_.SetSelectedMethod(GetSelectedMethodCategory());

  state_->selected_app()->SetPaymentHandlerHost(
      payment_handler_host_->AsWeakPtr());
  state_->GeneratePaymentResponse();
}

JourneyLogger::PaymentMethodCategory PaymentRequest::GetSelectedMethodCategory()
    const {
  const PaymentApp* selected_app = state_->selected_app();
  DCHECK(selected_app);
  switch (state_->selected_app()->type()) {
    case PaymentApp::Type::AUTOFILL:
      return JourneyLogger::PaymentMethodCategory::kBasicCard;
    case PaymentApp::Type::SERVICE_WORKER_APP:
      // Intentionally fall through.
    case PaymentApp::Type::NATIVE_MOBILE_APP: {
      for (const std::string& method : selected_app->GetAppMethodNames()) {
        if (method == methods::kGooglePay || method == methods::kAndroidPay) {
          return JourneyLogger::PaymentMethodCategory::kGoogle;
        } else if (method == methods::kGooglePlayBilling) {
          return JourneyLogger::PaymentMethodCategory::kPlayBilling;
        }
      }
      break;
    }
    case PaymentApp::Type::INTERNAL: {
      if (spec_->IsSecurePaymentConfirmationRequested())
        return JourneyLogger::PaymentMethodCategory::kSecurePaymentConfirmation;
      break;
    }
    case PaymentApp::Type::UNDEFINED:
      NOTREACHED();
      break;
  }
  return JourneyLogger::PaymentMethodCategory::kOther;
}

bool PaymentRequest::IsOffTheRecord() const {
  return delegate_->IsOffTheRecord();
}

void PaymentRequest::OnPaymentHandlerOpenWindowCalled() {
  DCHECK(state_->selected_app());
  // UKM for payment app origin should get recorded only when the origin of the
  // invoked payment app is shown to the user.
  journey_logger_.SetPaymentAppUkmSourceId(
      state_->selected_app()->UkmSourceId());
}

void PaymentRequest::RecordFirstAbortReason(
    JourneyLogger::AbortReason abort_reason) {
  if (!has_recorded_completion_) {
    has_recorded_completion_ = true;
    journey_logger_.SetAborted(abort_reason);
  }
}

void PaymentRequest::CanMakePaymentCallback(bool can_make_payment) {
  VLOG(2) << "PaymentRequest (" << *spec_->details().id
          << "): canMakePayment = " << can_make_payment;
  client_->OnCanMakePayment(
      can_make_payment ? mojom::CanMakePaymentQueryResult::CAN_MAKE_PAYMENT
                       : mojom::CanMakePaymentQueryResult::CANNOT_MAKE_PAYMENT);

  journey_logger_.SetCanMakePaymentValue(can_make_payment);

  if (observer_for_testing_)
    observer_for_testing_->OnCanMakePaymentReturned();
}

void PaymentRequest::HasEnrolledInstrumentCallback(
    bool has_enrolled_instrument) {
  VLOG(2) << "PaymentRequest (" << *spec_->details().id
          << "): hasEnrolledInstrument = " << has_enrolled_instrument;

  if (!spec_ || CanMakePaymentQueryFactory::GetInstance()
                    ->GetForContext(render_frame_host()->GetBrowserContext())
                    ->CanQuery(top_level_origin_, frame_origin_,
                               spec_->query_for_quota())) {
    RespondToHasEnrolledInstrumentQuery(has_enrolled_instrument,
                                        /*warn_local_development=*/false);
  } else if (UrlUtil::IsLocalDevelopmentUrl(frame_origin_)) {
    RespondToHasEnrolledInstrumentQuery(has_enrolled_instrument,
                                        /*warn_local_development=*/true);
  } else {
    client_->OnHasEnrolledInstrument(
        HasEnrolledInstrumentQueryResult::QUERY_QUOTA_EXCEEDED);
  }

  if (observer_for_testing_)
    observer_for_testing_->OnHasEnrolledInstrumentReturned();
}

void PaymentRequest::RespondToHasEnrolledInstrumentQuery(
    bool has_enrolled_instrument,
    bool warn_local_development) {
  HasEnrolledInstrumentQueryResult positive =
      warn_local_development
          ? HasEnrolledInstrumentQueryResult::WARNING_HAS_ENROLLED_INSTRUMENT
          : HasEnrolledInstrumentQueryResult::HAS_ENROLLED_INSTRUMENT;
  HasEnrolledInstrumentQueryResult negative =
      warn_local_development
          ? HasEnrolledInstrumentQueryResult::WARNING_HAS_NO_ENROLLED_INSTRUMENT
          : HasEnrolledInstrumentQueryResult::HAS_NO_ENROLLED_INSTRUMENT;

  client_->OnHasEnrolledInstrument(has_enrolled_instrument ? positive
                                                           : negative);
  journey_logger_.SetHasEnrolledInstrumentValue(has_enrolled_instrument);
}

void PaymentRequest::OnAbortResult(bool aborted) {
  VLOG(2) << "PaymentRequest (" << *spec_->details().id
          << "): abort = " << aborted;
  if (client_.is_bound())
    client_->OnAbort(aborted);

  if (aborted) {
    RecordFirstAbortReason(JourneyLogger::ABORT_REASON_ABORTED_BY_MERCHANT);
    state_->OnAbort();
  }
}

void PaymentRequest::ShowErrorMessageAndAbortPayment() {
  // Note that both branches of the if-else will invoke the OnUserCancelled()
  // method.
  if (display_handle_ && display_handle_->was_shown()) {
    // Will invoke OnUserCancelled() asynchronously when the user closes the
    // error message UI.
    delegate_->ShowErrorMessage();
    if (observer_for_testing_)
      observer_for_testing_->OnErrorDisplayed();
  } else {
    // Only app store billing apps do not display any browser payment UI.
    DCHECK(spec_->IsAppStoreBillingAlsoRequested());
    OnUserCancelled();
  }
}

}  // namespace payments
