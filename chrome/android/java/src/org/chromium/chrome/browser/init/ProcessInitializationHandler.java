// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.init;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.SystemClock;
import android.text.format.DateUtils;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.InputMethodSubtype;

import androidx.annotation.RequiresApi;
import androidx.annotation.WorkerThread;

import org.chromium.base.CommandLine;
import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.SysUtils;
import org.chromium.base.ThreadUtils;
import org.chromium.base.TraceEvent;
import org.chromium.base.compat.ApiHelperForR;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.task.AsyncTask;
import org.chromium.base.task.PostTask;
import org.chromium.base.task.TaskTraits;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.AppHooks;
import org.chromium.chrome.browser.ChromeActivitySessionTracker;
import org.chromium.chrome.browser.ChromeApplicationImpl;
import org.chromium.chrome.browser.ChromeBackupAgentImpl;
import org.chromium.chrome.browser.DefaultBrowserInfo;
import org.chromium.chrome.browser.DeferredStartupHandler;
import org.chromium.chrome.browser.DevToolsServer;
import org.chromium.chrome.browser.app.feature_guide.notifications.FeatureNotificationGuideDelegate;
import org.chromium.chrome.browser.app.video_tutorials.VideoTutorialShareHelper;
import org.chromium.chrome.browser.autofill_assistant.AutofillAssistantHistoryDeletionObserver;
import org.chromium.chrome.browser.bookmarkswidget.BookmarkWidgetProvider;
import org.chromium.chrome.browser.contacts_picker.ChromePickerAdapter;
import org.chromium.chrome.browser.content_capture.ContentCaptureHistoryDeletionObserver;
import org.chromium.chrome.browser.crash.CrashUploadCountStore;
import org.chromium.chrome.browser.crash.LogcatExtractionRunnable;
import org.chromium.chrome.browser.crash.MinidumpUploadServiceImpl;
import org.chromium.chrome.browser.download.DownloadController;
import org.chromium.chrome.browser.download.DownloadManagerService;
import org.chromium.chrome.browser.download.OfflineContentAvailabilityStatusProvider;
import org.chromium.chrome.browser.enterprise.util.EnterpriseInfo;
import org.chromium.chrome.browser.feature_guide.notifications.FeatureNotificationGuideService;
import org.chromium.chrome.browser.feature_guide.notifications.FeatureNotificationGuideServiceFactory;
import org.chromium.chrome.browser.firstrun.TosDialogBehaviorSharedPrefInvalidator;
import org.chromium.chrome.browser.flags.CachedFeatureFlags;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.history.HistoryDeletionBridge;
import org.chromium.chrome.browser.homepage.HomepageManager;
import org.chromium.chrome.browser.incognito.IncognitoTabLauncher;
import org.chromium.chrome.browser.language.GlobalAppLocaleController;
import org.chromium.chrome.browser.locale.LocaleManager;
import org.chromium.chrome.browser.media.MediaCaptureNotificationServiceImpl;
import org.chromium.chrome.browser.media.MediaViewerUtils;
import org.chromium.chrome.browser.metrics.LaunchMetrics;
import org.chromium.chrome.browser.metrics.PackageMetrics;
import org.chromium.chrome.browser.metrics.WebApkUninstallUmaTracker;
import org.chromium.chrome.browser.notifications.channels.ChannelsUpdater;
import org.chromium.chrome.browser.offlinepages.measurements.OfflineMeasurementsBackgroundTask;
import org.chromium.chrome.browser.omnibox.voice.AssistantVoiceSearchService;
import org.chromium.chrome.browser.optimization_guide.OptimizationGuideBridgeFactory;
import org.chromium.chrome.browser.partnercustomizations.PartnerBrowserCustomizations;
import org.chromium.chrome.browser.photo_picker.DecoderService;
import org.chromium.chrome.browser.preferences.ChromePreferenceKeys;
import org.chromium.chrome.browser.preferences.SharedPreferencesManager;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.profiles.ProfileManagerUtils;
import org.chromium.chrome.browser.query_tiles.QueryTileUtils;
import org.chromium.chrome.browser.quickactionsearchwidget.QuickActionSearchWidgetProvider;
import org.chromium.chrome.browser.rlz.RevenueStats;
import org.chromium.chrome.browser.searchwidget.SearchWidgetProvider;
import org.chromium.chrome.browser.sharing.shared_clipboard.SharedClipboardShareActivity;
import org.chromium.chrome.browser.signin.SigninCheckerProvider;
import org.chromium.chrome.browser.tab.state.ShoppingPersistedTabData;
import org.chromium.chrome.browser.tasks.tab_management.PriceTrackingUtilities;
import org.chromium.chrome.browser.ui.searchactivityutils.SearchActivityPreferencesManager;
import org.chromium.chrome.browser.util.AfterStartupTaskUtils;
import org.chromium.chrome.browser.webapps.WebappRegistry;
import org.chromium.components.background_task_scheduler.BackgroundTaskSchedulerFactory;
import org.chromium.components.browser_ui.contacts_picker.ContactsPickerDialog;
import org.chromium.components.browser_ui.photo_picker.DecoderServiceHost;
import org.chromium.components.browser_ui.photo_picker.PhotoPickerDelegateBase;
import org.chromium.components.browser_ui.photo_picker.PhotoPickerDialog;
import org.chromium.components.browser_ui.share.ClipboardImageFileProvider;
import org.chromium.components.browser_ui.share.ShareImageFileUtils;
import org.chromium.components.browser_ui.util.ConversionUtils;
import org.chromium.components.content_capture.PlatformContentCaptureController;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.components.minidump_uploader.CrashFileManager;
import org.chromium.components.optimization_guide.proto.HintsProto;
import org.chromium.components.signin.AccountManagerFacadeImpl;
import org.chromium.components.signin.AccountManagerFacadeProvider;
import org.chromium.components.version_info.Channel;
import org.chromium.components.version_info.VersionConstants;
import org.chromium.components.version_info.VersionInfo;
import org.chromium.components.viz.common.VizSwitches;
import org.chromium.components.viz.common.display.DeJellyUtils;
import org.chromium.components.webapps.AppBannerManager;
import org.chromium.content_public.browser.BrowserTaskExecutor;
import org.chromium.content_public.browser.ChildProcessLauncherHelper;
import org.chromium.content_public.browser.ContactsPicker;
import org.chromium.content_public.browser.ContactsPickerListener;
import org.chromium.content_public.browser.UiThreadTaskTraits;
import org.chromium.content_public.common.ContentSwitches;
import org.chromium.ui.base.Clipboard;
import org.chromium.ui.base.PhotoPicker;
import org.chromium.ui.base.PhotoPickerListener;
import org.chromium.ui.base.SelectFileDialog;
import org.chromium.ui.base.WindowAndroid;

import java.io.File;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Handles the initialization dependences of the browser process.  This is meant to handle the
 * initialization that is not tied to any particular Activity, and the logic that should only be
 * triggered a single time for the lifetime of the browser process.
 */
public class ProcessInitializationHandler {
    private static final String TAG = "ProcessInitHandler";

    private static final String DEV_TOOLS_SERVER_SOCKET_PREFIX = "chrome";

    /** Prevents race conditions when deleting snapshot database. */
    private static final Object SNAPSHOT_DATABASE_LOCK = new Object();
    private static final String SNAPSHOT_DATABASE_NAME = "snapshots.db";

    private static ProcessInitializationHandler sInstance;

    private boolean mInitializedPreNative;
    private boolean mInitializedPostNative;
    private boolean mInitializedDeferredStartupTasks;
    private DevToolsServer mDevToolsServer;

    /**
     * @return The ProcessInitializationHandler for use during the lifetime of the browser process.
     */
    public static ProcessInitializationHandler getInstance() {
        ThreadUtils.checkUiThread();
        if (sInstance == null) {
            sInstance = AppHooks.get().createProcessInitializationHandler();
        }
        return sInstance;
    }

    /**
     * Initializes the any dependencies that must occur before native library has been loaded.
     * <p>
     * Adding anything expensive to this must be avoided as it would delay the Chrome startup path.
     * <p>
     * All entry points that do not rely on {@link ChromeBrowserInitializer} must call this on
     * startup.
     */
    public final void initializePreNative() {
        try (TraceEvent e =
                        TraceEvent.scoped("ProcessInitializationHandler.initializePreNative()")) {
            ThreadUtils.checkUiThread();
            if (mInitializedPreNative) return;
            handlePreNativeInitialization();
            mInitializedPreNative = true;
        }
    }

    /**
     * Performs the shared class initialization.
     */
    protected void handlePreNativeInitialization() {
        if (CachedFeatureFlags.isEnabled(
                    ChromeFeatureList
                            .GIVE_JAVA_UI_THREAD_DEFAULT_TASK_TRAITS_USER_BLOCKING_PRIORITY)) {
            UiThreadTaskTraits.DEFAULT
                    .setTaskPriorityToUserBlockingForUiThreadDefaultTaskPriorityExperiment();
        }

        BrowserTaskExecutor.register();
        // This function controls whether BrowserTaskExecutor posts pre-native bootstrap tasks at
        // the front or back of the Looper's queue.
        BrowserTaskExecutor.setShouldPrioritizePreNativeBootstrapTasks(
                !CachedFeatureFlags.isEnabled(
                        ChromeFeatureList.ELIDE_PRIORITIZATION_OF_PRE_NATIVE_BOOTSTRAP_TASKS));

        Context application = ContextUtils.getApplicationContext();

        // Initialize the AccountManagerFacade with the correct AccountManagerDelegate. Must be done
        // only once and before AccountMangerHelper.get(...) is called to avoid using the
        // default AccountManagerDelegate.
        AccountManagerFacadeProvider.setInstance(
                new AccountManagerFacadeImpl(AppHooks.get().createAccountManagerDelegate()));

        // For ANR uploading - we set the version number so that when we ask Android for our ANRs,
        // it can also give us the version it happened on. This helps in the case that before we can
        // report the ANR, our app gets updated.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            ActivityManager am =
                    (ActivityManager) ContextUtils.getApplicationContext().getSystemService(
                            Context.ACTIVITY_SERVICE);
            // We can only do 128 bytes in ProcessStateSummary, so only storing the most important
            // thing that could change between the ANR happening and upload (when the rest of the
            // metadata is gathered) - the version number. Other fields either won't change (eg.
            // which channel) or don't matter as much (eg. what experiments are running).
            String productVersion = VersionInfo.getProductVersion();
            ApiHelperForR.setProcessStateSummary(am, productVersion.getBytes());
        }

        // De-jelly can also be controlled by a system property. As sandboxed processes can't
        // read this property directly, convert it to the equivalent command line flag.
        if (DeJellyUtils.externallyEnableDeJelly()) {
            CommandLine.getInstance().appendSwitch(VizSwitches.ENABLE_DE_JELLY);
        }
    }

    /**
     * Initializes any dependencies that must occur after the native library has been loaded.
     */
    public final void initializePostNative() {
        ThreadUtils.checkUiThread();
        if (mInitializedPostNative) return;
        handlePostNativeInitialization();
        mInitializedPostNative = true;
    }

    /**
     * @return Whether post native initialization has been completed.
     */
    public final boolean postNativeInitializationComplete() {
        return mInitializedPostNative;
    }

    /**
     * Performs the post native initialization.
     */
    protected void handlePostNativeInitialization() {
        ChromeActivitySessionTracker.getInstance().initializeWithNative();
        ProfileManagerUtils.removeSessionCookiesForAllProfiles();
        AppBannerManager.setAppDetailsDelegate(AppHooks.get().createAppDetailsDelegate());
        ChromeLifetimeController.initialize();
        Clipboard.getInstance().setImageFileProvider(new ClipboardImageFileProvider());

        DecoderServiceHost.setIntentSupplier(() -> {
            return new Intent(ContextUtils.getApplicationContext(), DecoderService.class);
        });

        SelectFileDialog.setPhotoPickerDelegate(new PhotoPickerDelegateBase() {
            @Override
            public PhotoPicker showPhotoPicker(WindowAndroid windowAndroid,
                    PhotoPickerListener listener, boolean allowMultiple, List<String> mimeTypes) {
                PhotoPickerDialog dialog = new PhotoPickerDialog(windowAndroid,
                        windowAndroid.getContext().get().getContentResolver(), listener,
                        allowMultiple,
                        mimeTypes);
                dialog.getWindow().getAttributes().windowAnimations = R.style.PickerDialogAnimation;
                dialog.show();
                return dialog;
            }
        });

        ContactsPicker.setContactsPickerDelegate(
                (WindowAndroid windowAndroid, ContactsPickerListener listener,
                        boolean allowMultiple, boolean includeNames, boolean includeEmails,
                        boolean includeTel, boolean includeAddresses, boolean includeIcons,
                        String formattedOrigin) -> {
                    ContactsPickerDialog dialog = new ContactsPickerDialog(windowAndroid,
                            new ChromePickerAdapter(windowAndroid.getContext().get()), listener,
                            allowMultiple, includeNames, includeEmails, includeTel,
                            includeAddresses, includeIcons, formattedOrigin);
                    dialog.getWindow().getAttributes().windowAnimations =
                            R.style.PickerDialogAnimation;
                    dialog.show();
                    return dialog;
                });

        SearchActivityPreferencesManager.onNativeLibraryReady();
        SearchWidgetProvider.initialize();
        QuickActionSearchWidgetProvider.initialize();

        HistoryDeletionBridge.getInstance().addObserver(new ContentCaptureHistoryDeletionObserver(
                () -> PlatformContentCaptureController.getInstance()));
        HistoryDeletionBridge.getInstance().addObserver(
                new AutofillAssistantHistoryDeletionObserver());
        FeatureNotificationGuideService.setDelegate(new FeatureNotificationGuideDelegate());
    }

    /**
     * Handle application level deferred startup tasks that can be lazily done after all
     * the necessary initialization has been completed. Should only be triggered once per browser
     * process lifetime. Any calls requiring network access should probably go here.
     *
     * Keep these tasks short and break up long tasks into multiple smaller tasks, as they run on
     * the UI thread and are blocking. Remember to follow RAIL guidelines, as much as possible, and
     * that most devices are quite slow, so leave enough buffer.
     */
    public final void initializeDeferredStartupTasks() {
        ThreadUtils.checkUiThread();
        if (mInitializedDeferredStartupTasks) return;
        mInitializedDeferredStartupTasks = true;

        handleDeferredStartupTasksInitialization();
    }

    /**
     * Performs the deferred startup task initialization.
     */
    protected void handleDeferredStartupTasksInitialization() {
        DeferredStartupHandler deferredStartupHandler = DeferredStartupHandler.getInstance();

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                // Punt all tasks that may block on disk off onto a background thread.
                initAsyncDiskTask();

                DefaultBrowserInfo.initBrowserFetcher();

                AfterStartupTaskUtils.setStartupComplete();

                PartnerBrowserCustomizations.getInstance().setOnInitializeAsyncFinished(
                        new Runnable() {
                            @Override
                            public void run() {
                                String homepageUrl = HomepageManager.getHomepageUri();
                                LaunchMetrics.recordHomePageLaunchMetrics(
                                        HomepageManager.isHomepageEnabled(),
                                        UrlUtilities.isNTPUrl(homepageUrl), homepageUrl);
                            }
                        });

                ShareImageFileUtils.clearSharedImages();

                SelectFileDialog.clearCapturedCameraFiles();

                if (ChannelsUpdater.getInstance().shouldUpdateChannels()) {
                    initChannelsAsync();
                }
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                // Clear any media notifications that existed when Chrome was last killed.
                MediaCaptureNotificationServiceImpl.clearMediaNotifications();

                startModerateBindingManagementIfNeeded();

                recordKeyboardLocaleUma();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                LocaleManager.getInstance().recordStartupMetrics();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                HomepageManager.recordHomepageLocationTypeIfEnabled();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                // Starts syncing with GSA.
                AppHooks.get().createGsaHelper().startSync();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                // Record the saved restore state in a histogram
                ChromeBackupAgentImpl.recordRestoreHistogram();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                SigninCheckerProvider.get().onMainActivityStart();
                RevenueStats.getInstance();
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                mDevToolsServer = new DevToolsServer(DEV_TOOLS_SERVER_SOCKET_PREFIX);
                mDevToolsServer.setRemoteDebuggingEnabled(
                        true, DevToolsServer.Security.ALLOW_DEBUG_PERMISSION);
            }
        });

        deferredStartupHandler.addDeferredTask(new Runnable() {
            @Override
            public void run() {
                // Add process check to diagnose http://crbug.com/606309. Remove this after the bug
                // is fixed.
                assert !CommandLine.getInstance().hasSwitch(ContentSwitches.SWITCH_PROCESS_TYPE);
                if (!CommandLine.getInstance().hasSwitch(ContentSwitches.SWITCH_PROCESS_TYPE)) {
                    DownloadController.setDownloadNotificationService(
                            DownloadManagerService.getDownloadManagerService());
                }
            }
        });

        deferredStartupHandler.addDeferredTask(
                ()
                        -> BackgroundTaskSchedulerFactory.getScheduler().checkForOSUpgrade(
                                ContextUtils.getApplicationContext()));

        deferredStartupHandler.addDeferredTask(
                ProcessInitializationHandler::logEGLShaderCacheSizeHistogram);

        deferredStartupHandler.addDeferredTask(
                () -> MediaViewerUtils.updateMediaLauncherActivityEnabled());

        deferredStartupHandler.addDeferredTask(
                ChromeApplicationImpl.getComponent()
                        .resolveTwaClearDataDialogRecorder()::makeDeferredRecordings);
        deferredStartupHandler.addDeferredTask(WebApkUninstallUmaTracker::recordDeferredUma);

        deferredStartupHandler.addDeferredTask(
                () -> IncognitoTabLauncher.updateComponentEnabledState());

        deferredStartupHandler.addDeferredTask(
                () -> SharedClipboardShareActivity.updateComponentEnabledState());
        deferredStartupHandler.addDeferredTask(
                () -> OfflineContentAvailabilityStatusProvider.getInstance());
        deferredStartupHandler.addDeferredTask(
                () -> EnterpriseInfo.getInstance().logDeviceEnterpriseInfo());
        deferredStartupHandler.addDeferredTask(
                () -> VideoTutorialShareHelper.saveUrlsToSharedPrefs());
        deferredStartupHandler.addDeferredTask(
                () -> TosDialogBehaviorSharedPrefInvalidator.refreshSharedPreferenceIfTosSkipped());
        deferredStartupHandler.addDeferredTask(
                () -> OfflineMeasurementsBackgroundTask.clearPersistedDataFromPrefs());
        deferredStartupHandler.addDeferredTask(() -> QueryTileUtils.isQueryTilesEnabledOnNTP());
        deferredStartupHandler.addDeferredTask(
                ()
                        -> AssistantVoiceSearchService.reportStartupUserEligibility(
                                ContextUtils.getApplicationContext()));
        deferredStartupHandler.addDeferredTask(
                () -> GlobalAppLocaleController.getInstance().recordOverrideLanguageMetrics());
        deferredStartupHandler.addDeferredTask(() -> {
            // OptimizationTypes which we give a guarantee will be registered when we pass the
            // onDeferredStartup() signal to OptimizationGuide.
            List<HintsProto.OptimizationType> registeredTypesAllowList = new ArrayList<>();
            registeredTypesAllowList.addAll(
                    ShoppingPersistedTabData.getShoppingHintsToRegisterOnDeferredStartup());
            new OptimizationGuideBridgeFactory(registeredTypesAllowList)
                    .create()
                    .onDeferredStartup();
            if (PriceTrackingUtilities.isPriceTrackingEligible()
                    && ShoppingPersistedTabData.isPriceTrackingWithOptimizationGuideEnabled()) {
                ShoppingPersistedTabData.onDeferredStartup();
            }
        });
        deferredStartupHandler.addDeferredTask(() -> {
            if (ChromeFeatureList.isEnabled(ChromeFeatureList.FEATURE_NOTIFICATION_GUIDE)) {
                FeatureNotificationGuideServiceFactory.getForProfile(
                        Profile.getLastUsedRegularProfile());
            }
        });
    }

    private void initChannelsAsync() {
        PostTask.postTask(TaskTraits.BEST_EFFORT_MAY_BLOCK,
                () -> ChannelsUpdater.getInstance().updateChannels());
    }

    private void initAsyncDiskTask() {
        new AsyncTask<Void>() {
            /**
             * The threshold after which it's no longer appropriate to try to attach logcat output
             * to a minidump file.
             * Note: This threshold of 12 hours was chosen fairly imprecisely, based on the
             * following intuition: On the one hand, Chrome can only access its own logcat output,
             * so the most recent lines should be relevant when available. On a typical device,
             * multiple hours of logcat output are available. On the other hand, it's important to
             * provide an escape hatch in case the logcat extraction code itself crashes, as
             * described in the doesCrashMinidumpNeedLogcat() documentation. Since this is a fairly
             * small and relatively frequently-executed piece of code, crashes are expected to be
             * unlikely; so it's okay for the escape hatch to be hard to use -- it's intended as an
             * extreme last resort.
             */
            private static final long LOGCAT_RELEVANCE_THRESHOLD_IN_HOURS = 12;

            private long mAsyncTaskStartTime;

            @Override
            protected Void doInBackground() {
                try {
                    TraceEvent.begin("ChromeBrowserInitializer.onDeferredStartup.doInBackground");
                    mAsyncTaskStartTime = SystemClock.uptimeMillis();

                    initCrashReporting();

                    // Initialize the WebappRegistry if it's not already initialized. Must be in
                    // async task due to shared preferences disk access on N.
                    WebappRegistry.getInstance();

                    // Force a widget refresh in order to wake up any possible zombie widgets.
                    // This is needed to ensure the right behavior when the process is suddenly
                    // killed.
                    BookmarkWidgetProvider.refreshAllWidgets();

                    removeSnapshotDatabase();

                    // Warm up all web app shared prefs. This must be run after the WebappRegistry
                    // instance is initialized.
                    WebappRegistry.warmUpSharedPrefs();

                    PackageMetrics.recordPackageStats();
                    return null;
                } finally {
                    TraceEvent.end("ChromeBrowserInitializer.onDeferredStartup.doInBackground");
                }
            }

            @Override
            protected void onPostExecute(Void params) {
                // Must be run on the UI thread after the WebappRegistry has been completely warmed.
                WebappRegistry.getInstance().unregisterOldWebapps(System.currentTimeMillis());

                RecordHistogram.recordLongTimesHistogram(
                        "UMA.Debug.EnableCrashUpload.DeferredStartUpAsyncTaskDuration",
                        SystemClock.uptimeMillis() - mAsyncTaskStartTime);
            }

            /**
             * Initializes the crash reporting system. More specifically, enables the crash
             * reporting system if it is user-permitted, and initiates uploading of any pending
             * crash reports. Also updates some UMA metrics and performs cleanup in the local crash
             * minidump storage directory.
             */
            private void initCrashReporting() {
                // Crash reports can be uploaded as part of a background service even while the main
                // Chrome activity is not running, and hence regular metrics reporting is not
                // possible. Instead, metrics are temporarily written to prefs; export those prefs
                // to UMA metrics here.
                MinidumpUploadServiceImpl.storeBreakpadUploadStatsInUma(
                        CrashUploadCountStore.getInstance());

                // Likewise, this is a good time to process and clean up any pending or stale crash
                // reports left behind by previous runs.
                CrashFileManager crashFileManager =
                        new CrashFileManager(ContextUtils.getApplicationContext().getCacheDir());
                crashFileManager.cleanOutAllNonFreshMinidumpFiles();

                // Restricting ANR collection to Canary until we are totally happy with it.
                // ANR collection is only available on R+.
                if (VersionConstants.CHANNEL == Channel.CANARY
                        && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    crashFileManager.collectAndWriteAnrs();
                }
                // Next, identify any minidumps that lack logcat output, and are too old to add
                // logcat output to. Mark these as ready for upload. If there is a fresh minidump
                // that still needs logcat output to be attached, stash it for now.
                File minidumpMissingLogcat = processMinidumpsSansLogcat(crashFileManager);

                // Now, upload all pending crash reports that are not still in need of logcat data.
                File[] minidumps = crashFileManager.getMinidumpsReadyForUpload(
                        MinidumpUploadServiceImpl.MAX_TRIES_ALLOWED);
                if (minidumps.length > 0) {
                    Log.i(TAG, "Attempting to upload %d accumulated crash dumps.",
                            minidumps.length);
                    if (MinidumpUploadServiceImpl.shouldUseJobSchedulerForUploads()) {
                        MinidumpUploadServiceImpl.scheduleUploadJob();
                    } else {
                        MinidumpUploadServiceImpl.tryUploadAllCrashDumps();
                    }
                }

                // Finally, if there is a minidump that still needs logcat output to be attached, do
                // so now. Note: It's important to do this strictly after calling
                // |crashFileManager.getMinidumpsReadyForUpload()|. Otherwise, there is a race
                // between appending the logcat and getting the results from that call, as the
                // minidump will be renamed to be a valid file for upload upon logcat extraction
                // success.
                if (minidumpMissingLogcat != null) {
                    // Note: When using the JobScheduler API to schedule uploads, this call might
                    // result in a duplicate request to schedule minidump uploads -- if the call
                    // succeeds, and there were also other pending minidumps found above. This is
                    // fine; the job scheduler is robust to such duplicate calls.
                    AsyncTask.THREAD_POOL_EXECUTOR.execute(
                            new LogcatExtractionRunnable(minidumpMissingLogcat));
                }
            }

            /**
             * Process all pending minidump files that lack logcat output. As a simplifying
             * assumption, assume that logcat output would only be relevant to the most recent
             * pending minidump, if there are multiple. As of Chrome version 58, about 50% of
             * startups that had *any* pending minidumps had at least one pending minidump without
             * any logcat output. About 5% had multiple minidumps without any logcat output.
             *
             * TODO(isherman): This is the simplest approach to resolving the complexity of
             * correctly attributing logcat output to the correct crash. However, it would be better
             * to attach logcat output to each minidump file that lacks it, if the relevant output
             * is still available. We can look at timestamps to correlate logcat lines with the
             * minidumps they correspond to.
             *
             * @return A single fresh minidump that should have logcat attached to it, or null if no
             *     such minidump exists.
             */
            private File processMinidumpsSansLogcat(CrashFileManager crashFileManager) {
                File[] minidumpsSansLogcat = crashFileManager.getMinidumpsSansLogcat();

                // If there are multiple minidumps present that are missing logcat output, only
                // append it to the most recent one. Upload the rest as-is.
                if (minidumpsSansLogcat.length > 1) {
                    for (int i = 1; i < minidumpsSansLogcat.length; ++i) {
                        CrashFileManager.trySetReadyForUpload(minidumpsSansLogcat[i]);
                    }
                }

                // Try to identify a single fresh minidump that should have logcat output appended
                // to it.
                if (minidumpsSansLogcat.length > 0) {
                    File mostRecentMinidumpSansLogcat = minidumpsSansLogcat[0];
                    if (doesCrashMinidumpNeedLogcat(mostRecentMinidumpSansLogcat)) {
                        return mostRecentMinidumpSansLogcat;
                    } else {
                        CrashFileManager.trySetReadyForUpload(mostRecentMinidumpSansLogcat);
                    }
                }
                return null;
            }

            /**
             * Returns whether or not it's appropriate to try to extract recent logcat output and
             * include that logcat output alongside the given {@param minidump} in a crash report.
             * Logcat output should only be extracted if (a) it hasn't already been extracted for
             * this minidump file, and (b) the minidump is fairly fresh. The freshness check is
             * important for two reasons: (1) First of all, it helps avoid including irrelevant
             * logcat output for a crash report. (2) Secondly, it provides an escape hatch that can
             * help circumvent a possible infinite crash loop, if the code responsible for
             * extracting and appending the logcat content is itself crashing. That is, the user can
             * wait 12 hours prior to relaunching Chrome, at which point this potential crash loop
             * would be circumvented.
             * @return Whether to try to include logcat output in the crash report corresponding to
             *     the given minidump.
             */
            private boolean doesCrashMinidumpNeedLogcat(File minidump) {
                if (!CrashFileManager.isMinidumpSansLogcat(minidump.getName())) return false;

                long ageInMillis = new Date().getTime() - minidump.lastModified();
                long ageInHours = ageInMillis / DateUtils.HOUR_IN_MILLIS;
                return ageInHours < LOGCAT_RELEVANCE_THRESHOLD_IN_HOURS;
            }
        }
                .executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR);
    }

    /**
     * Deletes the snapshot database which is no longer used because the feature has been removed
     * in Chrome M41.
     */
    @WorkerThread
    private void removeSnapshotDatabase() {
        synchronized (SNAPSHOT_DATABASE_LOCK) {
            SharedPreferencesManager prefs = SharedPreferencesManager.getInstance();
            if (!prefs.readBoolean(ChromePreferenceKeys.SNAPSHOT_DATABASE_REMOVED, false)) {
                ContextUtils.getApplicationContext().deleteDatabase(SNAPSHOT_DATABASE_NAME);
                prefs.writeBoolean(ChromePreferenceKeys.SNAPSHOT_DATABASE_REMOVED, true);
            }
        }
    }

    private void startModerateBindingManagementIfNeeded() {
        // Moderate binding doesn't apply to low end devices.
        if (SysUtils.isLowEndDevice()) return;
        ChildProcessLauncherHelper.startModerateBindingManagement(
                ContextUtils.getApplicationContext());
    }

    @SuppressWarnings("deprecation") // InputMethodSubtype.getLocale() deprecated in API 24
    private void recordKeyboardLocaleUma() {
        InputMethodManager imm =
                (InputMethodManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.INPUT_METHOD_SERVICE);
        List<InputMethodInfo> ims = imm.getEnabledInputMethodList();
        ArrayList<String> uniqueLanguages = new ArrayList<>();
        for (InputMethodInfo method : ims) {
            List<InputMethodSubtype> submethods =
                    imm.getEnabledInputMethodSubtypeList(method, true);
            for (InputMethodSubtype submethod : submethods) {
                if (submethod.getMode().equals("keyboard")) {
                    String language = submethod.getLocale().split("_")[0];
                    if (!uniqueLanguages.contains(language)) {
                        uniqueLanguages.add(language);
                    }
                }
            }
        }
        RecordHistogram.recordCount1MHistogram("InputMethod.ActiveCount", uniqueLanguages.size());

        InputMethodSubtype currentSubtype = imm.getCurrentInputMethodSubtype();
        Locale systemLocale = Locale.getDefault();
        if (currentSubtype != null && currentSubtype.getLocale() != null && systemLocale != null) {
            String keyboardLanguage = currentSubtype.getLocale().split("_")[0];
            boolean match = systemLocale.getLanguage().equalsIgnoreCase(keyboardLanguage);
            RecordHistogram.recordBooleanHistogram("InputMethod.MatchesSystemLanguage", match);
        }
    }

    /**
     * Logs a histogram with the size of the Android EGL shader cache.
     */
    @RequiresApi(Build.VERSION_CODES.N)
    private static void logEGLShaderCacheSizeHistogram() {
        // To simplify logic, only log this value on Android N+.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
            return;
        }
        final Context cacheContext =
                ContextUtils.getApplicationContext().createDeviceProtectedStorageContext();

        // Must log async, as we're doing a file access.
        PostTask.postTask(TaskTraits.BEST_EFFORT_MAY_BLOCK, () -> {
            // Record file sizes between 1-2560KB. Expected range is 1-2048KB, so this gives
            // us a bit of buffer. These values cannot be changed, as doing so will alter
            // histogram bucketing and confuse the dashboard.
            final int minCacheFileSizeKb = 1;
            final int maxCacheFileSizeKb = 2560;

            File codeCacheDir = cacheContext.getCodeCacheDir();
            if (codeCacheDir == null) {
                return;
            }
            // This filename is defined in core/java/android/view/HardwareRenderer.java,
            // and has been located in the codeCacheDir since Android M.
            File cacheFile = new File(codeCacheDir, "com.android.opengl.shaders_cache");
            if (!cacheFile.exists()) {
                return;
            }
            long cacheFileSizeKb = ConversionUtils.bytesToKilobytes(cacheFile.length());
            // Clamp size to [minFileSizeKb, maxFileSizeKb). This also guarantees that the
            // int-cast below is safe.
            if (cacheFileSizeKb < minCacheFileSizeKb) {
                cacheFileSizeKb = minCacheFileSizeKb;
            }
            if (cacheFileSizeKb >= maxCacheFileSizeKb) {
                cacheFileSizeKb = maxCacheFileSizeKb - 1;
            }
            String histogramName = "Memory.Experimental.Browser.EGLShaderCacheSize.Android";
            RecordHistogram.recordCustomCountHistogram(histogramName, (int) cacheFileSizeKb,
                    minCacheFileSizeKb, maxCacheFileSizeKb, 50);
        });
    }
}
