// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tabmodel;

import android.app.Activity;
import android.content.Intent;
import android.text.TextUtils;

import androidx.annotation.Nullable;

import org.chromium.base.Callback;
import org.chromium.base.IntentUtils;
import org.chromium.base.SysUtils;
import org.chromium.base.TraceEvent;
import org.chromium.base.supplier.Supplier;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.ServiceTabLauncher;
import org.chromium.chrome.browser.app.tab_activity_glue.ReparentingDelegateFactory;
import org.chromium.chrome.browser.app.tab_activity_glue.ReparentingTask;
import org.chromium.chrome.browser.compositor.CompositorViewHolder;
import org.chromium.chrome.browser.init.StartupTabPreloader;
import org.chromium.chrome.browser.ntp.NewTabPageLaunchOrigin;
import org.chromium.chrome.browser.ntp.NewTabPageUtils;
import org.chromium.chrome.browser.tab.RedirectHandlerTabHelper;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabAssociatedApp;
import org.chromium.chrome.browser.tab.TabBuilder;
import org.chromium.chrome.browser.tab.TabCreationState;
import org.chromium.chrome.browser.tab.TabDelegateFactory;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabParentIntent;
import org.chromium.chrome.browser.tab.TabResolver;
import org.chromium.chrome.browser.tab.TabState;
import org.chromium.chrome.browser.tab.state.SerializedCriticalPersistedTabData;
import org.chromium.components.embedder_support.util.UrlConstants;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.components.url_formatter.UrlFormatter;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.WindowAndroid;
import org.chromium.url.GURL;

/**
 * This class creates various kinds of new tabs and adds them to the right {@link TabModel}.
 */
public class ChromeTabCreator extends TabCreator {
    /**Interface to handle showing overview instead of NTP if needed. */
    public interface OverviewNTPCreator {
        /**
         * Handles showing the StartSurface instead of the NTP if needed.
         * @param isNTP Whether tab with NTP should be created.
         * @param isIncognito Whether tab is created in incognito.
         * @param parentTab The parent tab of the tab creation.
         * @param launchOrigin The {@link NewTabPageLaunchOrigin} that launched the NTP.
         * @return Whether NTP creation was handled.
         */
        boolean handleCreateNTPIfNeeded(boolean isNTP, boolean isIncognito, Tab parentTab,
                @NewTabPageLaunchOrigin int launchOrigin);

        /**
         * Called before the Tab's initialization.
         * @param tab The newly created Tab.
         * @param url The URL to load.
         */
        void preTabInitialization(Tab tab, String url);
    }

    private static final String TAG = "ChromeTabCreator";

    private final Activity mActivity;
    private final StartupTabPreloader mStartupTabPreloader;
    private final boolean mIncognito;

    private WindowAndroid mNativeWindow;
    private TabModel mTabModel;
    private TabModelOrderController mOrderController;
    private Supplier<TabDelegateFactory> mTabDelegateFactorySupplier;
    @Nullable
    private final OverviewNTPCreator mOverviewNTPCreator;
    private final AsyncTabParamsManager mAsyncTabParamsManager;
    private final Supplier<TabModelSelector> mTabModelSelectorSupplier;
    private final Supplier<CompositorViewHolder> mCompositorViewHolderSupplier;

    public ChromeTabCreator(Activity activity, WindowAndroid nativeWindow,
            StartupTabPreloader startupTabPreloader,
            Supplier<TabDelegateFactory> tabDelegateFactory, boolean incognito,
            OverviewNTPCreator overviewNTPCreator, AsyncTabParamsManager asyncTabParamsManager,
            Supplier<TabModelSelector> tabModelSelectorSupplier,
            Supplier<CompositorViewHolder> compositorViewHolderSupplier) {
        mActivity = activity;
        mStartupTabPreloader = startupTabPreloader;
        mNativeWindow = nativeWindow;
        mTabDelegateFactorySupplier = tabDelegateFactory;
        mIncognito = incognito;
        mOverviewNTPCreator = overviewNTPCreator;
        mAsyncTabParamsManager = asyncTabParamsManager;
        mTabModelSelectorSupplier = tabModelSelectorSupplier;
        mCompositorViewHolderSupplier = compositorViewHolderSupplier;
    }

    @Override
    public boolean createsTabsAsynchronously() {
        return false;
    }

    /**
     * Creates a new tab and posts to UI.
     * @param loadUrlParams parameters of the url load.
     * @param type Information about how the tab was launched.
     * @param parent the parent tab, if present.
     * @return The new tab.
     */
    @Override
    public Tab createNewTab(LoadUrlParams loadUrlParams, @TabLaunchType int type, Tab parent) {
        return createNewTab(loadUrlParams, type, parent, null);
    }

    /**
     * Creates a new tab and posts to UI.
     * @param loadUrlParams parameters of the url load.
     * @param type Information about how the tab was launched.
     * @param parent the parent tab, if present.
     * @param intent the source of the url if it isn't null.
     * @return The new tab.
     */
    public Tab createNewTab(
            LoadUrlParams loadUrlParams, @TabLaunchType int type, Tab parent, Intent intent) {
        // If parent is in the same tab model, place the new tab next to it.
        int position = TabModel.INVALID_TAB_INDEX;
        int index = mTabModel.indexOf(parent);
        if (index != TabModel.INVALID_TAB_INDEX) position = index + 1;

        return createNewTab(loadUrlParams, type, parent, position, intent);
    }

    /**
     * Creates a new tab and posts to UI.
     * @param loadUrlParams parameters of the url load.
     * @param type Information about how the tab was launched.
     * @param parent the parent tab, if present.
     * @param position the requested position (index in the tab model)
     * @param intent the source of the url if it isn't null.
     * @return The new tab.
     */
    private Tab createNewTab(LoadUrlParams loadUrlParams, @TabLaunchType int type, Tab parent,
            int position, Intent intent) {
        if (mOverviewNTPCreator != null
                && mOverviewNTPCreator.handleCreateNTPIfNeeded(
                        UrlUtilities.isNTPUrl(loadUrlParams.getUrl()), mIncognito, parent,
                        NewTabPageUtils.decodeOriginFromNtpUrl(loadUrlParams.getUrl()))) {
            return null;
        }
        try {
            TraceEvent.begin("ChromeTabCreator.createNewTab");
            int parentId = parent != null ? parent.getId() : Tab.INVALID_TAB_ID;

            GURL url = UrlFormatter.fixupUrl(loadUrlParams.getUrl());

            // Sanitize the url.
            loadUrlParams.setUrl(url.getValidSpecOrEmpty());
            loadUrlParams.setTransitionType(
                    getTransitionType(type, intent, loadUrlParams.getTransitionType()));

            // Check if the tab is being created asynchronously.
            int assignedTabId = IntentHandler.getTabId(intent);
            AsyncTabParams asyncParams = mAsyncTabParamsManager.remove(assignedTabId);

            boolean openInForeground = mOrderController.willOpenInForeground(type, mIncognito);
            TabDelegateFactory delegateFactory =
                    parent == null ? createDefaultTabDelegateFactory() : null;
            Tab tab;
            @TabCreationState
            int creationState = TabCreationState.LIVE_IN_FOREGROUND;
            if (asyncParams != null && asyncParams.getTabToReparent() != null) {
                type = TabLaunchType.FROM_REPARENTING;

                TabReparentingParams params = (TabReparentingParams) asyncParams;
                tab = params.getTabToReparent();
                ReparentingTask.from(tab).finish(
                        ReparentingDelegateFactory.createReparentingTaskDelegate(
                                mCompositorViewHolderSupplier.get(), mNativeWindow,
                                createDefaultTabDelegateFactory()),
                        params.getFinalizeCallback());
            } else if (asyncParams != null && asyncParams.getWebContents() != null) {
                openInForeground = true;
                WebContents webContents = asyncParams.getWebContents();
                // A WebContents was passed through the Intent.  Create a new Tab to hold it.
                Intent parentIntent = IntentUtils.safeGetParcelableExtra(
                        intent, IntentHandler.EXTRA_PARENT_INTENT);
                parentId = IntentUtils.safeGetIntExtra(
                        intent, IntentHandler.EXTRA_PARENT_TAB_ID, parentId);
                TabModelSelector selector = mTabModelSelectorSupplier.get();
                parent = selector != null ? selector.getTabById(parentId) : null;
                assert TabModelUtils.getTabIndexById(mTabModel, assignedTabId)
                        == TabModel.INVALID_TAB_INDEX;
                tab = TabBuilder.createLiveTab(!openInForeground)
                              .setId(assignedTabId)
                              .setParent(parent)
                              .setIncognito(mIncognito)
                              .setWindow(mNativeWindow)
                              .setLaunchType(type)
                              .setWebContents(webContents)
                              .setDelegateFactory(delegateFactory)
                              .setInitiallyHidden(!openInForeground)
                              .build();
                TabParentIntent.from(tab).set(parentIntent).setCurrentTab(selector::getCurrentTab);
                webContents.resumeLoadingCreatedWebContents();
            } else if (!openInForeground && SysUtils.isLowEndDevice()) {
                // On low memory devices the tabs opened in background are not loaded automatically
                // to preserve resources (cpu, memory, strong renderer binding) for the foreground
                // tab.
                tab = TabBuilder.createForLazyLoad(loadUrlParams)
                              .setParent(parent)
                              .setIncognito(mIncognito)
                              .setWindow(mNativeWindow)
                              .setLaunchType(type)
                              .setDelegateFactory(delegateFactory)
                              .setInitiallyHidden(!openInForeground)
                              .build();
                creationState = TabCreationState.FROZEN_FOR_LAZY_LOAD;
            } else {
                tab = (mStartupTabPreloader != null)
                        ? mStartupTabPreloader.takeTabIfMatchingOrDestroy(loadUrlParams, type)
                        : null;

                if (tab == null) {
                    TraceEvent.begin("ChromeTabCreator.loadUrl");
                    Callback<Tab> action = null;
                    if (mOverviewNTPCreator != null) {
                        action = (newTab) -> {
                            mOverviewNTPCreator.preTabInitialization(
                                    newTab, loadUrlParams.getUrl());
                        };
                    }
                    tab = TabBuilder.createLiveTab(!openInForeground)
                                  .setParent(parent)
                                  .setIncognito(mIncognito)
                                  .setWindow(mNativeWindow)
                                  .setLaunchType(type)
                                  .setDelegateFactory(delegateFactory)
                                  .setInitiallyHidden(!openInForeground)
                                  .setPreInitializeAction(action)
                                  .build();
                    tab.loadUrl(loadUrlParams);
                    TraceEvent.end("ChromeTabCreator.loadUrl");
                }
            }
            // When tab reparenting the |intent| is the reparenting intent, not the intent that
            // created the tab.
            if (type != TabLaunchType.FROM_REPARENTING) {
                RedirectHandlerTabHelper.updateIntentInTab(tab, intent);
            }
            if (intent != null && intent.hasExtra(ServiceTabLauncher.LAUNCH_REQUEST_ID_EXTRA)) {
                ServiceTabLauncher.onWebContentsForRequestAvailable(
                        intent.getIntExtra(ServiceTabLauncher.LAUNCH_REQUEST_ID_EXTRA, 0),
                        tab.getWebContents());
            }

            if (creationState == TabCreationState.LIVE_IN_FOREGROUND && !openInForeground) {
                creationState = TabCreationState.LIVE_IN_BACKGROUND;
            }
            mTabModel.addTab(tab, position, type, creationState);
            return tab;
        } finally {
            TraceEvent.end("ChromeTabCreator.createNewTab");
        }
    }

    @Override
    public boolean createTabWithWebContents(
            @Nullable Tab parent, WebContents webContents, @TabLaunchType int type, GURL url) {
        // The parent tab was already closed.  Do not open child tabs.
        int parentId = parent != null ? parent.getId() : Tab.INVALID_TAB_ID;
        if (mTabModel.isClosurePending(parentId)) return false;

        // If parent is in the same tab model, place the new tab next to it.
        int position = TabModel.INVALID_TAB_INDEX;
        int index = TabModelUtils.getTabIndexById(mTabModel, parentId);
        if (index != TabModel.INVALID_TAB_INDEX) position = index + 1;

        boolean openInForeground = mOrderController.willOpenInForeground(type, mIncognito);
        TabDelegateFactory delegateFactory =
                parent == null ? createDefaultTabDelegateFactory() : null;
        Tab tab = TabBuilder.createLiveTab(!openInForeground)
                          .setParent(parent)
                          .setIncognito(mIncognito)
                          .setWindow(mNativeWindow)
                          .setLaunchType(type)
                          .setWebContents(webContents)
                          .setDelegateFactory(delegateFactory)
                          .setInitiallyHidden(!openInForeground)
                          .build();
        @TabCreationState
        int creationState = openInForeground ? TabCreationState.LIVE_IN_FOREGROUND
                                             : TabCreationState.LIVE_IN_BACKGROUND;
        mTabModel.addTab(tab, position, type, creationState);
        return true;
    }

    @Override
    public Tab launchUrl(String url, @TabLaunchType int type) {
        return launchUrl(url, type, null, 0);
    }

    /**
     * Creates a new tab and loads the specified URL in it. This is a convenience method for
     * {@link #createNewTab} with the default {@link LoadUrlParams} and no parent tab.
     *
     * @param url the URL to open.
     * @param type the type of action that triggered that launch. Determines how the tab is opened
     *             (for example, in the foreground or background).
     * @param intent the source of url if it isn't null.
     * @param intentTimestamp the time the intent was received.
     * @return the created tab.
     */
    public Tab launchUrl(String url, @TabLaunchType int type, Intent intent, long intentTimestamp) {
        LoadUrlParams loadUrlParams = new LoadUrlParams(url);
        loadUrlParams.setIntentReceivedTimestamp(intentTimestamp);
        return createNewTab(loadUrlParams, type, null, intent);
    }

    /**
     * Opens the specified URL into a tab, potentially reusing a tab. Typically if a user opens
     * several link from the same application, we reuse the same tab so as to not open too many
     * tabs.
     * @param url the URL to open
     * @param appId the ID of the application that triggered that URL navigation.
     * @param forceNewTab whether the URL should be opened in a new tab. If false, an existing tab
     *                    already opened by the same app will be reused.
     * @param intent the source of url if it isn't null.
     * @return the tab the URL was opened in, could be a new tab or a reused one.
     */
    // TODO(crbug.com/1081924): Clean up the launches from SearchActivity/Chrome.
    public Tab launchUrlFromExternalApp(
            LoadUrlParams loadUrlParams, String appId, boolean forceNewTab, Intent intent) {
        assert !mIncognito;
        // Don't re-use tabs for intents from Chrome. Note that this can be spoofed so shouldn't be
        // relied on for anything security sensitive.
        boolean isLaunchedFromChrome = TextUtils.equals(appId, mActivity.getPackageName());

        if (forceNewTab || isLaunchedFromChrome) {
            // We don't associate the tab with that app ID, as it is assumed that if the
            // application wanted to open this tab as a new tab, it probably does not want it
            // reused either.

            // Using FROM_LINK ensures the tab is parented to the current tab, which allows
            // the back button to close these tabs and restore selection to the previous
            // tab.
            @TabLaunchType
            int launchType = isLaunchedFromChrome ? TabLaunchType.FROM_LINK
                                                  : TabLaunchType.FROM_EXTERNAL_APP;
            return createNewTab(loadUrlParams, launchType, null, intent);
        }

        if (appId == null) {
            // If we have no application ID, we use a made-up one so that these tabs can be
            // reused.
            appId = TabModelImpl.UNKNOWN_APP_ID;
        }
        // Let's try to find an existing tab that was started by that app.
        for (int i = 0; i < mTabModel.getCount(); i++) {
            Tab tab = mTabModel.getTabAt(i);
            if (appId.equals(TabAssociatedApp.getAppId(tab))) {
                // We don't reuse the tab, we create a new one at the same index instead.
                // Reusing a tab would require clearing the navigation history and clearing the
                // contents (we would not want the previous content to show).
                Tab newTab = createNewTab(
                        loadUrlParams, TabLaunchType.FROM_EXTERNAL_APP, null, i, intent);
                TabAssociatedApp.from(newTab).setAppId(appId);
                mTabModel.closeTab(tab, false, false, false);
                return newTab;
            }
        }

        // No tab for that app, we'll have to create a new one.
        Tab tab = createNewTab(loadUrlParams, TabLaunchType.FROM_EXTERNAL_APP, null, intent);
        TabAssociatedApp.from(tab).setAppId(appId);
        return tab;
    }

    @Override
    public Tab createFrozenTab(TabState state,
            SerializedCriticalPersistedTabData serializedCriticalPersistedTabData, int id,
            boolean isIncognito, int index) {
        TabModelSelector selector = mTabModelSelectorSupplier.get();
        TabResolver resolver = (tabId) -> {
            return selector != null ? selector.getTabById(tabId) : null;
        };
        boolean selectTab =
                mOrderController.willOpenInForeground(TabLaunchType.FROM_RESTORE, isIncognito);
        AsyncTabParams asyncParams = mAsyncTabParamsManager.remove(id);
        Tab tab = null;
        @TabLaunchType
        int launchType = TabLaunchType.FROM_RESTORE;
        @TabCreationState
        int creationState = TabCreationState.FROZEN_ON_RESTORE;
        if (asyncParams != null && asyncParams.getTabToReparent() != null) {
            creationState = TabCreationState.LIVE_IN_BACKGROUND;

            TabReparentingParams params = (TabReparentingParams) asyncParams;
            tab = params.getTabToReparent();
            if (tab.isIncognito() != isIncognito) {
                throw new IllegalStateException("Incognito state mismatch. TabState: " + isIncognito
                        + ". Tab: " + tab.isIncognito());
            }
            ReparentingTask.from(tab).finish(
                    ReparentingDelegateFactory.createReparentingTaskDelegate(
                            mCompositorViewHolderSupplier.get(), mNativeWindow,
                            createDefaultTabDelegateFactory()),
                    params.getFinalizeCallback());
            // TODO(crbug.com/1108562): Photos/videos viewed in custom tabs aren't displayed
            // properly after reparenting. This is a temporary fix for RBS issue crbug.com/1105810,
            // investigate and fix the root cause.
            if (tab.getUrl().getScheme().equals(UrlConstants.FILE_SCHEME)) {
                tab.reloadIgnoringCache();
            } else if (tab.needsReload()) {
                tab.reload();
            }
        }
        if (tab == null) {
            tab = TabBuilder.createFromFrozenState()
                          .setId(id)
                          .setTabResolver(resolver)
                          .setIncognito(isIncognito)
                          .setWindow(mNativeWindow)
                          .setDelegateFactory(createDefaultTabDelegateFactory())
                          .setInitiallyHidden(!selectTab)
                          .setTabState(state)
                          .setSerializedCriticalPersistedTabData(serializedCriticalPersistedTabData)
                          .build();
        }

        if (isIncognito != mIncognito) {
            throw new IllegalStateException("Incognito state mismatch. TabState: "
                    + state.isIncognito() + ". Creator: " + mIncognito);
        }

        mTabModel.addTab(tab, index, launchType, creationState);
        return tab;
    }

    /**
     * @param tabLaunchType Type of the tab launch.
     * @param intent The intent causing the tab launch.
     * @param originalTransitionType The original transition type.
     * @return The page transition type constant.
     */
    private int getTransitionType(@TabLaunchType int tabLaunchType, Intent intent,
            @PageTransition int originalTransitionType) {
        int transition = PageTransition.LINK;
        switch (tabLaunchType) {
            case TabLaunchType.FROM_START_SURFACE:
                transition = originalTransitionType;
                break;
            case TabLaunchType.FROM_RESTORE:
            case TabLaunchType.FROM_LINK:
            case TabLaunchType.FROM_EXTERNAL_APP:
            case TabLaunchType.FROM_BROWSER_ACTIONS:
                // FROM_API ensures intent handling isn't used.
                transition = PageTransition.LINK | PageTransition.FROM_API;
                break;
            case TabLaunchType.FROM_CHROME_UI:
            case TabLaunchType.FROM_TAB_GROUP_UI:
            case TabLaunchType.FROM_STARTUP:
            case TabLaunchType.FROM_LAUNCHER_SHORTCUT:
            case TabLaunchType.FROM_LAUNCH_NEW_INCOGNITO_TAB:
            case TabLaunchType.FROM_APP_WIDGET:
                transition = PageTransition.AUTO_TOPLEVEL;
                break;
            case TabLaunchType.FROM_LONGPRESS_FOREGROUND:
            case TabLaunchType.FROM_LONGPRESS_INCOGNITO:
                transition = PageTransition.LINK;
                break;
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND:
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND_IN_GROUP:
            case TabLaunchType.FROM_RECENT_TABS:
                // On low end devices tabs are backgrounded in a frozen state, so we set the
                // transition type to RELOAD to avoid handling intents when the tab is foregrounded.
                // (https://crbug.com/758027)
                transition =
                        SysUtils.isLowEndDevice() ? PageTransition.RELOAD : PageTransition.LINK;
                break;
            default:
                assert false;
                break;
        }

        return IntentHandler.getTransitionTypeFromIntent(intent, transition);
    }

    /**
     * Sets the tab model and tab content manager to use.
     * @param model           The new {@link TabModel} to use.
     * @param orderController The controller for determining the order of tabs.
     */
    public void setTabModel(TabModel model, TabModelOrderController orderController) {
        mTabModel = model;
        mOrderController = orderController;
    }

    /**
     * @return The default tab delegate factory to be used if creating new tabs w/o parents.
     */
    public TabDelegateFactory createDefaultTabDelegateFactory() {
        return mTabDelegateFactorySupplier != null ? mTabDelegateFactorySupplier.get() : null;
    }

    /**
     * Sets the window to create tabs for.
     */
    public void setWindowAndroid(WindowAndroid window) {
        mNativeWindow = window;
    }
}
