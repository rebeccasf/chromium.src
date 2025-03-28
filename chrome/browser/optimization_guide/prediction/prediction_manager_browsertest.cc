// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "base/base64.h"
#include "base/callback_helpers.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/optimization_guide/browser_test_util.h"
#include "chrome/browser/optimization_guide/optimization_guide_keyed_service.h"
#include "chrome/browser/optimization_guide/optimization_guide_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_key.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/metrics/content/subprocess_metrics_provider.h"
#include "components/optimization_guide/core/optimization_guide_constants.h"
#include "components/optimization_guide/core/optimization_guide_features.h"
#include "components/optimization_guide/core/optimization_guide_prefs.h"
#include "components/optimization_guide/core/optimization_guide_store.h"
#include "components/optimization_guide/core/optimization_guide_switches.h"
#include "components/optimization_guide/core/optimization_guide_test_util.h"
#include "components/optimization_guide/core/prediction_manager.h"
#include "components/optimization_guide/core/prediction_model_download_manager.h"
#include "components/optimization_guide/core/store_update_data.h"
#include "components/optimization_guide/proto/models.pb.h"
#include "components/prefs/pref_service.h"
#include "components/variations/hashing.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/network_connection_change_simulator.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gmock/include/gmock/gmock.h"

#if BUILDFLAG(IS_CHROMEOS_ASH)
#include "ash/constants/ash_switches.h"
#endif

namespace {

constexpr int kSuccessfulModelVersion = 123;

std::unique_ptr<optimization_guide::proto::GetModelsResponse>
BuildGetModelsResponse() {
  std::unique_ptr<optimization_guide::proto::GetModelsResponse>
      get_models_response =
          std::make_unique<optimization_guide::proto::GetModelsResponse>();

  std::unique_ptr<optimization_guide::proto::PredictionModel> prediction_model =
      std::make_unique<optimization_guide::proto::PredictionModel>();
  optimization_guide::proto::ModelInfo* model_info =
      prediction_model->mutable_model_info();
  model_info->set_version(2);
  model_info->set_optimization_target(
      optimization_guide::proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
  model_info->add_supported_model_engine_versions(
      optimization_guide::proto::ModelEngineVersion::
          MODEL_ENGINE_VERSION_TFLITE_2_8);
  prediction_model->mutable_model()->set_download_url(
      "https://example.com/model");
  *get_models_response->add_models() = *prediction_model.get();

  return get_models_response;
}

enum class PredictionModelsFetcherRemoteResponseType {
  kSuccessfulWithValidModelFile = 0,
  kSuccessfulWithInvalidModelFile = 1,
  kSuccessfulWithValidModelFileAndInvalidAdditionalFiles = 2,
  kSuccessfulWithValidModelFileAndValidAdditionalFiles = 3,
  kUnsuccessful = 4,
};

}  // namespace

namespace optimization_guide {

class ModelFileObserver : public OptimizationTargetModelObserver {
 public:
  using ModelFileReceivedCallback =
      base::OnceCallback<void(proto::OptimizationTarget, const ModelInfo&)>;

  ModelFileObserver() = default;
  ~ModelFileObserver() override = default;

  void set_model_file_received_callback(ModelFileReceivedCallback callback) {
    file_received_callback_ = std::move(callback);
  }

  void OnModelUpdated(proto::OptimizationTarget optimization_target,
                      const ModelInfo& model_info) override {
    if (file_received_callback_)
      std::move(file_received_callback_).Run(optimization_target, model_info);
  }

 private:
  ModelFileReceivedCallback file_received_callback_;
};

// Abstract base class for browser testing Prediction Manager.
// Actual class fixtures should implement InitializeFeatureList to set up
// features used in tests.
class PredictionManagerBrowserTestBase : public InProcessBrowserTest {
 public:
  PredictionManagerBrowserTestBase() = default;
  ~PredictionManagerBrowserTestBase() override = default;

  PredictionManagerBrowserTestBase(const PredictionManagerBrowserTestBase&) =
      delete;
  PredictionManagerBrowserTestBase& operator=(
      const PredictionManagerBrowserTestBase&) = delete;

  void SetUp() override {
    InitializeFeatureList();

    models_server_ = std::make_unique<net::EmbeddedTestServer>(
        net::EmbeddedTestServer::TYPE_HTTPS);
    models_server_->ServeFilesFromSourceDirectory(
        "chrome/test/data/optimization_guide");
    models_server_->RegisterRequestHandler(base::BindRepeating(
        &PredictionManagerBrowserTestBase::HandleGetModelsRequest,
        base::Unretained(this)));

    ASSERT_TRUE(models_server_->Start());
    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    content::NetworkConnectionChangeSimulator().SetConnectionType(
        network::mojom::ConnectionType::CONNECTION_2G);
    https_server_ = std::make_unique<net::EmbeddedTestServer>(
        net::EmbeddedTestServer::TYPE_HTTPS);
    https_server_->ServeFilesFromSourceDirectory(GetChromeTestDataDir());
    ASSERT_TRUE(https_server_->Start());
    https_url_with_content_ = https_server_->GetURL("/english_page.html");
    https_url_without_content_ = https_server_->GetURL("/empty.html");
    model_file_url_ = models_server_->GetURL("/signed_valid_model.crx3");
    model_file_with_good_additional_file_url_ =
        models_server_->GetURL("/additional_file_exists.crx3");
    model_file_with_nonexistent_additional_file_url_ =
        models_server_->GetURL("/additional_file_doesnt_exist.crx3");

    InProcessBrowserTest::SetUpOnMainThread();
  }

  void TearDownOnMainThread() override {
    EXPECT_TRUE(https_server_->ShutdownAndWaitUntilComplete());
    EXPECT_TRUE(models_server_->ShutdownAndWaitUntilComplete());
    InProcessBrowserTest::TearDownOnMainThread();
  }

  void SetUpCommandLine(base::CommandLine* cmd) override {
    cmd->AppendSwitch("enable-spdy-proxy-auth");

    cmd->AppendSwitch(optimization_guide::switches::
                          kDisableCheckingUserPermissionsForTesting);
    cmd->AppendSwitchASCII(optimization_guide::switches::kFetchHintsOverride,
                           "whatever.com,somehost.com");
    cmd->AppendSwitchASCII(
        optimization_guide::switches::kOptimizationGuideServiceGetModelsURL,
        models_server_
            ->GetURL(GURL(optimization_guide::
                              kOptimizationGuideServiceGetModelsDefaultURL)
                         .host(),
                     "/")
            .spec());
    cmd->AppendSwitchASCII("host-rules", "MAP * 127.0.0.1");
    cmd->AppendSwitchASCII("force-variation-ids", "4");
  }

  void SetResponseType(
      PredictionModelsFetcherRemoteResponseType response_type) {
    response_type_ = response_type;
  }

  void RegisterWithKeyedService(ModelFileObserver* model_file_observer) {
    OptimizationGuideKeyedServiceFactory::GetForProfile(browser()->profile())
        ->AddObserverForOptimizationTargetModel(
            optimization_guide::proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD,
            absl::nullopt, model_file_observer);
  }

  PredictionManager* GetPredictionManager() {
    OptimizationGuideKeyedService* optimization_guide_keyed_service =
        OptimizationGuideKeyedServiceFactory::GetForProfile(
            browser()->profile());
    return optimization_guide_keyed_service->GetPredictionManager();
  }

  void SetExpectedFieldTrialNames(
      const base::flat_set<uint32_t>& expected_field_trial_name_hashes) {
    expected_field_trial_name_hashes_ = expected_field_trial_name_hashes;
  }

  GURL https_url_with_content() { return https_url_with_content_; }
  GURL https_url_without_content() { return https_url_without_content_; }

 protected:
  // Virtualize for testing different feature configurations.
  virtual void InitializeFeatureList() = 0;

  base::test::ScopedFeatureList scoped_feature_list_;

 private:
  std::unique_ptr<net::test_server::HttpResponse> HandleGetModelsRequest(
      const net::test_server::HttpRequest& request) {
    // Returning nullptr will cause the test server to fallback to serving the
    // file from the test data directory.
    if (request.GetURL() == model_file_url_)
      return nullptr;
    if (request.GetURL() == model_file_with_good_additional_file_url_)
      return nullptr;
    if (request.GetURL() == model_file_with_nonexistent_additional_file_url_)
      return nullptr;

    std::unique_ptr<net::test_server::BasicHttpResponse> response;

    response = std::make_unique<net::test_server::BasicHttpResponse>();
    // The request to the remote Optimization Guide Service should always be a
    // POST.
    EXPECT_EQ(request.method, net::test_server::METHOD_POST);
    EXPECT_NE(request.headers.end(), request.headers.find("X-Client-Data"));
    optimization_guide::proto::GetModelsRequest models_request;
    EXPECT_TRUE(models_request.ParseFromString(request.content));
    // Make sure we actually filter field trials appropriately.
    EXPECT_EQ(expected_field_trial_name_hashes_.size(),
              static_cast<size_t>(models_request.active_field_trials_size()));
    base::flat_set<uint32_t> seen_field_trial_name_hashes;
    for (const auto& field_trial : models_request.active_field_trials()) {
      EXPECT_TRUE(
          expected_field_trial_name_hashes_.find(field_trial.name_hash()) !=
          expected_field_trial_name_hashes_.end());
      seen_field_trial_name_hashes.insert(field_trial.name_hash());
    }
    EXPECT_EQ(seen_field_trial_name_hashes.size(),
              expected_field_trial_name_hashes_.size());

    response->set_code(net::HTTP_OK);
    std::unique_ptr<optimization_guide::proto::GetModelsResponse>
        get_models_response = BuildGetModelsResponse();
    if (response_type_ == PredictionModelsFetcherRemoteResponseType::
                              kSuccessfulWithInvalidModelFile) {
      get_models_response->mutable_models(0)->mutable_model()->set_download_url(
          https_url_with_content_.spec());
    } else if (response_type_ == PredictionModelsFetcherRemoteResponseType::
                                     kSuccessfulWithValidModelFile) {
      get_models_response->mutable_models(0)->mutable_model()->set_download_url(
          model_file_url_.spec());
    } else if (response_type_ ==
               PredictionModelsFetcherRemoteResponseType::
                   kSuccessfulWithValidModelFileAndInvalidAdditionalFiles) {
      get_models_response->mutable_models(0)->mutable_model()->set_download_url(
          model_file_with_nonexistent_additional_file_url_.spec());
    } else if (response_type_ ==
               PredictionModelsFetcherRemoteResponseType::
                   kSuccessfulWithValidModelFileAndValidAdditionalFiles) {
      get_models_response->mutable_models(0)->mutable_model()->set_download_url(
          model_file_with_good_additional_file_url_.spec());
    } else if (response_type_ ==
               PredictionModelsFetcherRemoteResponseType::kUnsuccessful) {
      response->set_code(net::HTTP_NOT_FOUND);
    }

    std::string serialized_response;
    get_models_response->SerializeToString(&serialized_response);
    response->set_content(serialized_response);
    return std::move(response);
  }

  GURL model_file_url_;
  GURL model_file_with_good_additional_file_url_;
  GURL model_file_with_nonexistent_additional_file_url_;
  GURL https_url_with_content_, https_url_without_content_;
  std::unique_ptr<net::EmbeddedTestServer> https_server_;
  std::unique_ptr<net::EmbeddedTestServer> models_server_;
  PredictionModelsFetcherRemoteResponseType response_type_ =
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile;
  base::flat_set<uint32_t> expected_field_trial_name_hashes_;
};

class PredictionManagerBrowserTest : public PredictionManagerBrowserTestBase {
 public:
  PredictionManagerBrowserTest() = default;
  ~PredictionManagerBrowserTest() override = default;

  PredictionManagerBrowserTest(const PredictionManagerBrowserTest&) = delete;
  PredictionManagerBrowserTest& operator=(const PredictionManagerBrowserTest&) =
      delete;

 private:
  void InitializeFeatureList() override {
    scoped_feature_list_.InitWithFeatures(
        {optimization_guide::features::kOptimizationHints,
         optimization_guide::features::kRemoteOptimizationGuideFetching,
         optimization_guide::features::kOptimizationTargetPrediction},
        {});
  }
};

IN_PROC_BROWSER_TEST_F(PredictionManagerBrowserTest,
                       RemoteFetchingPrefDisabled) {
  ModelFileObserver model_file_observer;
  SetResponseType(PredictionModelsFetcherRemoteResponseType::kUnsuccessful);
  browser()->profile()->GetPrefs()->SetBoolean(
      optimization_guide::prefs::kOptimizationGuideFetchingEnabled, false);
  base::HistogramTester histogram_tester;

  RegisterWithKeyedService(&model_file_observer);

  base::RunLoop().RunUntilIdle();

  // Should not have made fetch request.
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelFetcher.GetModelsResponse.Status", 0);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelFetcher.GetModelsResponse.Status."
      "PainfulPageLoad",
      0);
}

IN_PROC_BROWSER_TEST_F(PredictionManagerBrowserTest,
                       ModelsAndFeaturesStoreInitialized) {
  ModelFileObserver model_file_observer;
  SetResponseType(
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile);
  base::HistogramTester histogram_tester;
  content::NetworkConnectionChangeSimulator().SetConnectionType(
      network::mojom::ConnectionType::CONNECTION_2G);

  RegisterWithKeyedService(&model_file_observer);
  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionManager.PredictionModelsStored", 1);
  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad", 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionManager.PredictionModelsStored", true, 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
}

IN_PROC_BROWSER_TEST_F(PredictionManagerBrowserTest,
                       PredictionModelFetchFailed) {
  ModelFileObserver model_file_observer;
  SetResponseType(PredictionModelsFetcherRemoteResponseType::kUnsuccessful);
  base::HistogramTester histogram_tester;

  RegisterWithKeyedService(&model_file_observer);

  // Wait until histograms have been updated before performing checks for
  // correct behavior based on the response.
  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionModelFetcher.GetModelsResponse.Status", 1);

  histogram_tester.ExpectBucketCount(
      "OptimizationGuide.PredictionModelFetcher.GetModelsResponse.Status",
      net::HTTP_NOT_FOUND, 1);
  histogram_tester.ExpectBucketCount(
      "OptimizationGuide.PredictionModelFetcher.GetModelsResponse.Status."
      "PainfulPageLoad",
      net::HTTP_NOT_FOUND, 1);

  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionManager.PredictionModelsStored", 0);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad", 0);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad", 0);
}

class PredictionManagerNoUserPermissionsTest
    : public PredictionManagerBrowserTest {
 public:
  PredictionManagerNoUserPermissionsTest() {
    // Field trials should not be sent.
    SetExpectedFieldTrialNames({});
  }

  ~PredictionManagerNoUserPermissionsTest() override = default;

  void SetUpCommandLine(base::CommandLine* cmd) override {
    PredictionManagerBrowserTest::SetUpCommandLine(cmd);

    // Remove switches that enable user permissions.
    cmd->RemoveSwitch(switches::kDisableCheckingUserPermissionsForTesting);
  }

 private:
  void InitializeFeatureList() override {
    scoped_feature_list_.InitWithFeaturesAndParameters(
        {
            {features::kOptimizationHints, {}},
            {features::kRemoteOptimizationGuideFetching, {}},
            {features::kOptimizationTargetPrediction, {}},
            {features::kOptimizationHintsFieldTrials,
             {{"allowed_field_trial_names",
               "scoped_feature_list_trial_for_OptimizationHints,scoped_feature_"
               "list_trial_for_OptimizationHintsFetching"}}},
        },
        {});
  }
};

IN_PROC_BROWSER_TEST_F(PredictionManagerNoUserPermissionsTest,
                       FieldTrialsNotPassedWhenNoUserPermissions) {
  ModelFileObserver model_file_observer;
  base::HistogramTester histogram_tester;

  SetResponseType(
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile);
  RegisterWithKeyedService(&model_file_observer);

  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionManager.PredictionModelsStored", 1);

  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad", 1);
}

class PredictionManagerModelDownloadingBrowserTest
    : public PredictionManagerBrowserTest {
 public:
  PredictionManagerModelDownloadingBrowserTest() = default;
  ~PredictionManagerModelDownloadingBrowserTest() override = default;

  void SetUpOnMainThread() override {
    model_file_observer_ = std::make_unique<ModelFileObserver>();

    PredictionManagerBrowserTest::SetUpOnMainThread();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    PredictionManagerBrowserTest::SetUpCommandLine(command_line);
#if BUILDFLAG(IS_CHROMEOS_ASH)
    command_line->AppendSwitch(
        ash::switches::kIgnoreUserProfileMappingForTests);
#endif
  }

  void TearDownOnMainThread() override {
    PredictionManagerBrowserTest::TearDownOnMainThread();
  }

  ModelFileObserver* model_file_observer() {
    return model_file_observer_.get();
  }

  void RegisterModelFileObserverWithKeyedService(Profile* profile = nullptr) {
    OptimizationGuideKeyedServiceFactory::GetForProfile(
        profile ? profile : browser()->profile())
        ->AddObserverForOptimizationTargetModel(
            proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD,
            /*model_metadata=*/absl::nullopt, model_file_observer_.get());
  }

 private:
  void InitializeFeatureList() override {
    scoped_feature_list_.InitWithFeaturesAndParameters(
        {
            {features::kOptimizationHints, {}},
            {features::kRemoteOptimizationGuideFetching, {}},
            {features::kOptimizationTargetPrediction, {}},
            {features::kOptimizationGuideModelDownloading,
             {{"unrestricted_model_downloading", "true"}}},
            {features::kOptimizationHintsFieldTrials,
             {{"allowed_field_trial_names",
               "scoped_feature_list_trial_for_OptimizationHints,scoped_feature_"
               "list_trial_for_OptimizationHintsFetching"}}},
        },
        {});
    SetExpectedFieldTrialNames(base::flat_set<uint32_t>(
        {variations::HashName(
             "scoped_feature_list_trial_for_OptimizationHints"),
         variations::HashName(
             "scoped_feature_list_trial_for_OptimizationHintsFetching")}));
  }

  std::unique_ptr<ModelFileObserver> model_file_observer_;
};

// Flaky on various bots. See https://crbug.com/1266318
IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       DISABLED_TestIncognitoUsesModelFromRegularProfile) {
  SetResponseType(
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile);

  // Set up model download with regular profile.
  {
    base::HistogramTester histogram_tester;

    std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
    model_file_observer()->set_model_file_received_callback(base::BindOnce(
        [](base::RunLoop* run_loop,
           proto::OptimizationTarget optimization_target,
           const ModelInfo& model_info) {
          EXPECT_EQ(optimization_target,
                    proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
          run_loop->Quit();
        },
        run_loop.get()));
    RegisterModelFileObserverWithKeyedService();

    // Wait until the observer receives the file. We increase the timeout to 60
    // seconds here since the file is on the larger side.
    {
      base::test::ScopedRunLoopTimeout file_download_timeout(FROM_HERE,
                                                             base::Seconds(60));
      run_loop->Run();
    }

    histogram_tester.ExpectUniqueSample(
        "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
        PredictionModelDownloadStatus::kSuccess, 1);

    histogram_tester.ExpectUniqueSample(
        "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad",
        kSuccessfulModelVersion, 1);
    histogram_tester.ExpectUniqueSample(
        "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad",
        kSuccessfulModelVersion, 1);
  }

  // Now set up model download with incognito profile. Download should not
  // happen, but the OnModelUpdated callback should be triggered.
  {
    base::HistogramTester otr_histogram_tester;
    std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
    model_file_observer()->set_model_file_received_callback(base::BindOnce(
        [](base::RunLoop* run_loop,
           proto::OptimizationTarget optimization_target,
           const ModelInfo& model_info) {
          EXPECT_EQ(optimization_target,
                    proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
          run_loop->Quit();
        },
        run_loop.get()));
    Browser* otr_browser = CreateIncognitoBrowser(browser()->profile());
    RegisterModelFileObserverWithKeyedService(otr_browser->profile());

    run_loop->Run();

    otr_histogram_tester.ExpectTotalCount(
        "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus", 0);
    otr_histogram_tester.ExpectTotalCount(
        "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad", 0);
  }
}
// Flaky on multiple ASAN bots. See https://crbug.com/1266318
#if defined(ADDRESS_SANITIZER)
#define MAYBE_TestIncognitoDoesntFetchModels \
  DISABLED_TestIncognitoDoesntFetchModels
#else
#define MAYBE_TestIncognitoDoesntFetchModels TestIncognitoDoesntFetchModels
#endif
IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       MAYBE_TestIncognitoDoesntFetchModels) {
  base::HistogramTester histogram_tester;

  SetResponseType(PredictionModelsFetcherRemoteResponseType::
                      kSuccessfulWithInvalidModelFile);

  Browser* otr_browser = CreateIncognitoBrowser(browser()->profile());

  // Registering should not initiate the fetch and the model updated callback
  // should not be triggered too.
  RegisterModelFileObserverWithKeyedService(otr_browser->profile());

  model_file_observer()->set_model_file_received_callback(base::BindOnce(
      [](proto::OptimizationTarget optimization_target,
         const ModelInfo& model_info) { FAIL() << "Should not be called"; }));

  RetryForHistogramUntilCountReached(
      &histogram_tester, "OptimizationGuide.PredictionManager.StoreInitialized",
      1);

  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus", 0);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad", 0);
}

IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       TestDownloadUrlAcceptedByDownloadServiceButInvalid) {
  base::HistogramTester histogram_tester;

  SetResponseType(PredictionModelsFetcherRemoteResponseType::
                      kSuccessfulWithInvalidModelFile);

  // Registering should initiate the fetch and receive a response with a model
  // containing a download URL and then subsequently downloaded.
  RegisterModelFileObserverWithKeyedService();

  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus", 1);

  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelDownloadManager.State.PainfulPageLoad",
      2);
  histogram_tester.ExpectBucketCount(
      "OptimizationGuide.PredictionModelDownloadManager.State.PainfulPageLoad",
      PredictionModelDownloadManager::PredictionModelDownloadState::kRequested,
      1);
  histogram_tester.ExpectBucketCount(
      "OptimizationGuide.PredictionModelDownloadManager.State.PainfulPageLoad",
      PredictionModelDownloadManager::PredictionModelDownloadState::kStarted,
      1);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStartLatency."
      "PainfulPageLoad",
      1);

  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
      PredictionModelDownloadStatus::kFailedCrxVerification, 1);
  // An unverified file should not notify us that it's ready.
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad", 0);
}

IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       TestSuccessfulModelFileFlow) {
  base::HistogramTester histogram_tester;

  SetResponseType(
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile);

  std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
  model_file_observer()->set_model_file_received_callback(base::BindOnce(
      [](base::RunLoop* run_loop, proto::OptimizationTarget optimization_target,
         const ModelInfo& model_info) {
        EXPECT_EQ(optimization_target,
                  proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);

        // Regression test for crbug/1327975.
        // Make sure model file path downloaded into profile dir.
        base::FilePath profile_dir =
            g_browser_process->profile_manager()->GetLastUsedProfileDir();
        EXPECT_TRUE(profile_dir.IsParent(model_info.GetModelFilePath()));
        run_loop->Quit();
      },
      run_loop.get()));

  // Registering should initiate the fetch and receive a response with a model
  // containing a download URL and then subsequently downloaded.
  RegisterModelFileObserverWithKeyedService();

  // Wait until the observer receives the file. We increase the timeout to 60
  // seconds here since the file is on the larger side.
  {
    base::test::ScopedRunLoopTimeout file_download_timeout(FROM_HERE,
                                                           base::Seconds(60));
    run_loop->Run();
  }

  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
      PredictionModelDownloadStatus::kSuccess, 1);

  // No error when moving the file so there will be no record.
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelDownloadManager.ReplaceFileError", 0);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
  EXPECT_THAT(
      histogram_tester.GetAllSamples("OptimizationGuide.PredictionManager."
                                     "ModelDeliveryEvents.PainfulPageLoad"),
      testing::UnorderedElementsAre(
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kGetModelsRequest),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kDownloadServiceRequest),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDownloadStarted),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDownloaded),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDelivered),
                       1)));
}

IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       TestSuccessfulModelFileFlowWithAdditionalFile) {
  base::HistogramTester histogram_tester;

  SetResponseType(PredictionModelsFetcherRemoteResponseType::
                      kSuccessfulWithValidModelFileAndValidAdditionalFiles);

  std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
  model_file_observer()->set_model_file_received_callback(base::BindOnce(
      [](base::RunLoop* run_loop, proto::OptimizationTarget optimization_target,
         const ModelInfo& model_info) {
        EXPECT_EQ(optimization_target,
                  proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
        ASSERT_EQ(1U, model_info.GetAdditionalFiles().size());
        EXPECT_TRUE(model_info.GetAdditionalFiles().begin()->IsAbsolute());
        EXPECT_EQ(FILE_PATH_LITERAL("good_additional_file.txt"),
                  model_info.GetAdditionalFiles().begin()->BaseName().value());
        run_loop->Quit();
      },
      run_loop.get()));

  // Registering should initiate the fetch and receive a response with a model
  // containing a download URL and then subsequently downloaded.
  RegisterModelFileObserverWithKeyedService();

  // Wait until the observer receives the file. We increase the timeout to 60
  // seconds here since the file is on the larger side.
  {
    base::test::ScopedRunLoopTimeout file_download_timeout(FROM_HERE,
                                                           base::Seconds(60));
    run_loop->Run();
  }

  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
      PredictionModelDownloadStatus::kSuccess, 1);

  // No error when moving the file so there will be no record.
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelDownloadManager.ReplaceFileError", 0);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad",
      kSuccessfulModelVersion, 1);
  EXPECT_THAT(
      histogram_tester.GetAllSamples("OptimizationGuide.PredictionManager."
                                     "ModelDeliveryEvents.PainfulPageLoad"),
      testing::UnorderedElementsAre(
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kGetModelsRequest),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kDownloadServiceRequest),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDownloadStarted),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDownloaded),
                       1),
          base::Bucket(static_cast<base::HistogramBase::Sample>(
                           ModelDeliveryEvent::kModelDelivered),
                       1)));
}

IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       TestSuccessfulModelFileFlowWithInvalidAdditionalFile) {
  base::HistogramTester histogram_tester;

  SetResponseType(PredictionModelsFetcherRemoteResponseType::
                      kSuccessfulWithValidModelFileAndInvalidAdditionalFiles);

  std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
  model_file_observer()->set_model_file_received_callback(
      base::BindOnce([](proto::OptimizationTarget optimization_target,
                        const ModelInfo& model_info) {
        // Since the model's additional file is invalid, this callback should
        // never be run.
        FAIL();
      }));

  // Registering should initiate the fetch and receive a response with a model
  // containing a download URL and then subsequently downloaded.
  RegisterModelFileObserverWithKeyedService();

  RetryForHistogramUntilCountReached(
      &histogram_tester,
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus", 1);
  base::RunLoop().RunUntilIdle();

  // The additional file should not have been able to be moved, since it doesn't
  // exist.
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
      PredictionModelDownloadStatus::kFailedModelFileOtherError, 1);
  histogram_tester.ExpectUniqueSample(
      "OptimizationGuide.PredictionModelDownloadManager.ReplaceFileError."
      "PainfulPageLoad",
      std::abs(base::File::Error::FILE_ERROR_NOT_FOUND), 1);

  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelUpdateVersion.PainfulPageLoad", 0);
  histogram_tester.ExpectTotalCount(
      "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad", 0);
}

// Flaky on multiple ASAN bots. See https://crbug.com/1266318
#if defined(ADDRESS_SANITIZER)
#define MAYBE_TestSwitchProfileDoesntCrash DISABLED_TestSwitchProfileDoesntCrash
#else
#define MAYBE_TestSwitchProfileDoesntCrash TestSwitchProfileDoesntCrash
#endif
IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       MAYBE_TestSwitchProfileDoesntCrash) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  base::FilePath other_path =
      profile_manager->GenerateNextProfileDirectoryPath();

  base::RunLoop run_loop;

  // Create an additional profile.
  profile_manager->CreateProfileAsync(
      other_path,
      base::BindLambdaForTesting(
          [&run_loop](Profile* profile, Profile::CreateStatus status) {
            if (status == Profile::CREATE_STATUS_INITIALIZED)
              run_loop.Quit();
          }));

  run_loop.Run();

  Profile* profile = profile_manager->GetProfileByPath(other_path);
  ASSERT_TRUE(profile);
  CreateBrowser(profile);
}

#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_CHROMEOS_ASH)
// CreateGuestBrowser() is not supported for Android or ChromeOS out of the box.
IN_PROC_BROWSER_TEST_F(PredictionManagerModelDownloadingBrowserTest,
                       GuestProfileReceivesModel) {
  SetResponseType(
      PredictionModelsFetcherRemoteResponseType::kSuccessfulWithValidModelFile);

  {
    base::HistogramTester histogram_tester;
    // Register in the primary profile and ensure the model returns.
    RegisterModelFileObserverWithKeyedService(browser()->profile());

    std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
    model_file_observer()->set_model_file_received_callback(base::BindOnce(
        [](base::RunLoop* run_loop,
           proto::OptimizationTarget optimization_target,
           const ModelInfo& model_info) {
          EXPECT_EQ(optimization_target,
                    proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
          run_loop->Quit();
        },
        run_loop.get()));
    run_loop->Run();
    histogram_tester.ExpectUniqueSample(
        "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus",
        PredictionModelDownloadStatus::kSuccess, 1);
  }

  {
    base::HistogramTester histogram_tester;
    // Now hook everything up in the guest profile and we should still get the
    // model back but no additional fetches should be made.
    Browser* guest_browser = CreateGuestBrowser();

    // To prevent any race, ensure the store has be initialized.
    RetryForHistogramUntilCountReached(
        &histogram_tester,
        "OptimizationGuide.PredictionManager.StoreInitialized", 1);
    std::unique_ptr<base::RunLoop> run_loop = std::make_unique<base::RunLoop>();
    ModelFileObserver model_file_observer;
    model_file_observer.set_model_file_received_callback(base::BindOnce(
        [](base::RunLoop* run_loop,
           proto::OptimizationTarget optimization_target,
           const ModelInfo& model_info) {
          EXPECT_EQ(optimization_target,
                    proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD);
          run_loop->Quit();
        },
        run_loop.get()));
    OptimizationGuideKeyedServiceFactory::GetForProfile(
        guest_browser->profile())
        ->AddObserverForOptimizationTargetModel(
            proto::OPTIMIZATION_TARGET_PAINFUL_PAGE_LOAD,
            /*model_metadata=*/absl::nullopt, &model_file_observer);
    // Wait until the opt guide is up and the model is loaded as its shared
    // between profiles.
    RetryForHistogramUntilCountReached(
        &histogram_tester,
        "OptimizationGuide.PredictionModelLoadedVersion.PainfulPageLoad", 1);

    run_loop->Run();
    histogram_tester.ExpectTotalCount(
        "OptimizationGuide.PredictionModelDownloadManager.DownloadStatus", 0);
  }
}
#endif

}  // namespace optimization_guide
