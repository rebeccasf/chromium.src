// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.features.start_surface;

import static android.os.Build.VERSION_CODES.O_MR1;

import static androidx.test.espresso.Espresso.onView;
import static androidx.test.espresso.action.ViewActions.click;
import static androidx.test.espresso.assertion.ViewAssertions.doesNotExist;
import static androidx.test.espresso.assertion.ViewAssertions.matches;
import static androidx.test.espresso.contrib.RecyclerViewActions.actionOnItemAtPosition;
import static androidx.test.espresso.matcher.RootMatchers.withDecorView;
import static androidx.test.espresso.matcher.ViewMatchers.Visibility.GONE;
import static androidx.test.espresso.matcher.ViewMatchers.Visibility.VISIBLE;
import static androidx.test.espresso.matcher.ViewMatchers.isDescendantOfA;
import static androidx.test.espresso.matcher.ViewMatchers.isDisplayed;
import static androidx.test.espresso.matcher.ViewMatchers.isEnabled;
import static androidx.test.espresso.matcher.ViewMatchers.withEffectiveVisibility;
import static androidx.test.espresso.matcher.ViewMatchers.withId;
import static androidx.test.espresso.matcher.ViewMatchers.withParent;
import static androidx.test.espresso.matcher.ViewMatchers.withText;

import static org.hamcrest.CoreMatchers.not;
import static org.hamcrest.core.AllOf.allOf;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import static org.chromium.base.test.util.CriteriaHelper.DEFAULT_MAX_TIME_TO_POLL;
import static org.chromium.base.test.util.CriteriaHelper.DEFAULT_POLLING_INTERVAL;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.areAnimatorsEnabled;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.closeFirstTabInTabSwitcher;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.createTabGroup;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.createTabs;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.enterTabSwitcher;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.finishActivity;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.getSwipeToDismissAction;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.leaveTabSwitcher;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.mergeAllNormalTabsToAGroup;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.switchTabModel;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.verifyTabModelTabCount;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.verifyTabSwitcherCardCount;
import static org.chromium.components.embedder_support.util.UrlConstants.NTP_URL;
import static org.chromium.ui.test.util.ViewUtils.onViewWaiting;
import static org.chromium.ui.test.util.ViewUtils.waitForView;

import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.support.test.InstrumentationRegistry;
import android.support.test.uiautomator.UiDevice;
import android.view.View;
import android.widget.ImageView;

import androidx.annotation.Nullable;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.test.espresso.Espresso;
import androidx.test.espresso.NoMatchingViewException;
import androidx.test.espresso.ViewAssertion;
import androidx.test.espresso.contrib.AccessibilityChecks;
import androidx.test.espresso.contrib.RecyclerViewActions;
import androidx.test.filters.MediumTest;

import org.hamcrest.Matcher;
import org.hamcrest.Matchers;
import org.junit.After;
import org.junit.Before;
import org.junit.ClassRule;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.Callback;
import org.chromium.base.GarbageCollectionTestUtils;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.test.util.ApplicationTestUtils;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Criteria;
import org.chromium.base.test.util.CriteriaHelper;
import org.chromium.base.test.util.DisableIf;
import org.chromium.base.test.util.DisabledTest;
import org.chromium.base.test.util.Feature;
import org.chromium.base.test.util.FlakyTest;
import org.chromium.base.test.util.MinAndroidSdkLevel;
import org.chromium.base.test.util.Restriction;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.compositor.layouts.Layout;
import org.chromium.chrome.browser.compositor.layouts.StaticLayout;
import org.chromium.chrome.browser.compositor.layouts.content.TabContentManager;
import org.chromium.chrome.browser.flags.CachedFeatureFlags;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.flags.ChromeSwitches;
import org.chromium.chrome.browser.layouts.LayoutTestUtils;
import org.chromium.chrome.browser.layouts.LayoutType;
import org.chromium.chrome.browser.night_mode.ChromeNightModeTestUtils;
import org.chromium.chrome.browser.search_engines.TemplateUrlServiceFactory;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabStateExtractor;
import org.chromium.chrome.browser.tabmodel.IncognitoTabHostUtils;
import org.chromium.chrome.browser.tabmodel.TabCreator;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelSelectorTabModelObserver;
import org.chromium.chrome.browser.tasks.pseudotab.PseudoTab;
import org.chromium.chrome.browser.tasks.pseudotab.TabAttributeCache;
import org.chromium.chrome.browser.tasks.tab_groups.TabGroupModelFilter;
import org.chromium.chrome.browser.tasks.tab_management.TabProperties;
import org.chromium.chrome.browser.tasks.tab_management.TabSelectionEditorTestingRobot;
import org.chromium.chrome.browser.tasks.tab_management.TabSuggestionMessageService;
import org.chromium.chrome.browser.tasks.tab_management.TabSwitcher;
import org.chromium.chrome.browser.tasks.tab_management.TabSwitcherCoordinator;
import org.chromium.chrome.browser.tasks.tab_management.TabUiFeatureUtilities;
import org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper;
import org.chromium.chrome.browser.tasks.tab_management.TabUiThemeProvider;
import org.chromium.chrome.browser.ui.messages.snackbar.SnackbarManager;
import org.chromium.chrome.browser.undo_tab_close_snackbar.UndoBarController;
import org.chromium.chrome.browser.util.ChromeAccessibilityUtil;
import org.chromium.chrome.tab_ui.R;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.ChromeTabbedActivityTestRule;
import org.chromium.chrome.test.util.ActivityTestUtils;
import org.chromium.chrome.test.util.ChromeRenderTestRule;
import org.chromium.chrome.test.util.ChromeTabUtils;
import org.chromium.chrome.test.util.MenuUtils;
import org.chromium.chrome.test.util.browser.Features;
import org.chromium.chrome.test.util.browser.Features.DisableFeatures;
import org.chromium.chrome.test.util.browser.Features.EnableFeatures;
import org.chromium.components.browser_ui.widget.chips.ChipView;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.test.util.TestThreadUtils;
import org.chromium.content_public.browser.test.util.WebContentsUtils;
import org.chromium.net.test.EmbeddedTestServer;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.test.util.DisableAnimationsTestRule;
import org.chromium.ui.test.util.UiRestriction;
import org.chromium.ui.util.ColorUtils;
import org.chromium.ui.widget.ChromeImageView;
import org.chromium.ui.widget.ViewLookupCachingFrameLayout;

import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

// clang-format off
/** Tests for the {@link StartSurfaceLayout} */
@SuppressWarnings("ConstantConditions")
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE,
        "force-fieldtrials=Study/Group"})
@EnableFeatures({ChromeFeatureList.TAB_GRID_LAYOUT_ANDROID + "<Study"})
@Restriction(
        {UiRestriction.RESTRICTION_TYPE_PHONE, Restriction.RESTRICTION_TYPE_NON_LOW_END_DEVICE})
public class StartSurfaceLayoutTest {
    // clang-format on
    private static final String BASE_PARAMS = "force-fieldtrial-params="
            + "Study.Group:soft-cleanup-delay/0/cleanup-delay/0/skip-slow-zooming/false"
            + "/zooming-min-sdk-version/19/zooming-min-memory-mb/512";

    // Tests need animation on.
    @ClassRule
    public static DisableAnimationsTestRule sEnableAnimationsRule =
            new DisableAnimationsTestRule(true);

    @Rule
    public ChromeTabbedActivityTestRule mActivityTestRule = new ChromeTabbedActivityTestRule();

    @Rule
    public ChromeRenderTestRule mRenderTestRule =
            ChromeRenderTestRule.Builder.withPublicCorpus()
                    .setRevision(1)
                    .setBugComponent(ChromeRenderTestRule.Component.UI_BROWSER_MOBILE_START)
                    .build();

    @SuppressWarnings("FieldCanBeLocal")
    private EmbeddedTestServer mTestServer;
    private StartSurfaceLayout mStartSurfaceLayout;
    private String mUrl;
    private int mRepeat;
    private List<WeakReference<Bitmap>> mAllBitmaps = new LinkedList<>();
    private Callback<Bitmap> mBitmapListener =
            (bitmap) -> mAllBitmaps.add(new WeakReference<>(bitmap));
    private TabSwitcher.TabListDelegate mTabListDelegate;

    @Before
    public void setUp() {
        AccessibilityChecks.enable();
        mTestServer = EmbeddedTestServer.createAndStartServer(InstrumentationRegistry.getContext());
        // After setUp, Chrome is launched and has one NTP.
        mActivityTestRule.startMainActivityFromLauncher();

        Layout layout = mActivityTestRule.getActivity().getLayoutManager().getOverviewLayout();
        assertTrue(layout instanceof StartSurfaceLayout);
        mStartSurfaceLayout = (StartSurfaceLayout) layout;
        mUrl = mTestServer.getURL("/chrome/test/data/android/navigate/simple.html");
        mRepeat = 1;

        mTabListDelegate = mStartSurfaceLayout.getStartSurfaceForTesting().getGridTabListDelegate();
        mTabListDelegate.setBitmapCallbackForTesting(mBitmapListener);
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting());

        mActivityTestRule.getActivity().getTabContentManager().setCaptureMinRequestTimeForTesting(
                0);

        CriteriaHelper.pollUiThread(
                mActivityTestRule.getActivity().getTabModelSelector()::isTabStateInitialized);

        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting());
    }

    @After
    public void tearDown() {
        ActivityTestUtils.clearActivityOrientation(mActivityTestRule.getActivity());
        TestThreadUtils.runOnUiThreadBlocking(
                ChromeNightModeTestUtils::tearDownNightModeAfterChromeActivityDestroyed);
        TestThreadUtils.runOnUiThreadBlocking(
                () -> ChromeAccessibilityUtil.get().setAccessibilityEnabledForTesting(null));
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisableIf.Build(message = "Flaky on emulators; see https://crbug.com/1313747",
        supported_abis_includes = "x86")
    public void testRenderGrid_3WebTabs() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(3, 0, "about:blank");
        // Make sure all thumbnails are there before switching tabs.
        enterGTSWithThumbnailRetry();
        leaveTabSwitcher(cta);

        ChromeTabUtils.switchTabInCurrentTabModel(cta, 0);
        enterTabSwitcher(cta);
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "3_web_tabs");
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @DisabledTest(message = "https://crbug.com/1300962")
    @DisableIf.Build(sdk_is_greater_than = O_MR1, message = "crbug.com/1077552")
    public void testRenderGrid_10WebTabs() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(10, 0, "about:blank");
        // Make sure all thumbnails are there before switching tabs.
        enterGTSWithThumbnailRetry();
        leaveTabSwitcher(cta);

        ChromeTabUtils.switchTabInCurrentTabModel(cta, 0);
        enterTabSwitcher(cta);
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "10_web_tabs");
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @DisabledTest(message = "https://crbug.com/1139807")
    @DisableIf.Build(sdk_is_greater_than = O_MR1, message = "crbug.com/1077552")
    public void testRenderGrid_10WebTabs_InitialScroll() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(10, 0, "about:blank");
        assertEquals(9, cta.getTabModelSelector().getCurrentModel().index());
        enterGTSWithThumbnailRetry();
        // Make sure the grid tab switcher is scrolled down to show the selected tab.
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "10_web_tabs-select_last");
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testSwitchTabModel_ScrollToSelectedTab() throws IOException {
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(10, 0, "about:blank");
        assertEquals(9, cta.getCurrentTabModel().index());
        createTabs(cta, true, 1);
        CriteriaHelper.pollUiThread(() -> cta.getCurrentTabModel().isIncognito());
        enterTabSwitcher(cta);
        switchTabModel(cta, false);
        TabUiTestHelper.verifyAllTabsHaveThumbnail(cta.getCurrentTabModel());
        // Make sure the grid tab switcher is scrolled down to show the selected tab.
        onView(tabSwitcherViewMatcher()).check((v, noMatchException) -> {
            if (noMatchException != null) throw noMatchException;
            assertTrue(v instanceof RecyclerView);
            LinearLayoutManager layoutManager =
                    (LinearLayoutManager) ((RecyclerView) v).getLayoutManager();
            assertEquals(9, layoutManager.findLastVisibleItemPosition());
        });
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testRenderGrid_Incognito() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        // Prepare some incognito tabs and enter tab switcher.
        prepareTabs(1, 3, "about:blank");
        assertTrue(cta.getCurrentTabModel().isIncognito());
        // Make sure all thumbnails are there before switching tabs.
        enterGTSWithThumbnailRetry();
        leaveTabSwitcher(cta);

        ChromeTabUtils.switchTabInCurrentTabModel(cta, 0);
        enterTabSwitcher(cta);
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "3_incognito_web_tabs");
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    @DisableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION})
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisableIf.Build(message = "Flaky on emulators; see https://crbug.com/1313747",
        supported_abis_includes = "x86")
    public void testRenderGrid_3NativeTabs() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        // Prepare some incognito native tabs and enter tab switcher.
        // NTP in incognito mode is chosen for its consistency in look, and we don't have to mock
        // away the MV tiles, login promo, feed, etc.
        prepareTabs(1, 3, null);
        assertTrue(cta.getCurrentTabModel().isIncognito());
        // Make sure all thumbnails are there before switching tabs.
        enterGTSWithThumbnailRetry();
        leaveTabSwitcher(cta);
        // Espresso.pressBack();

        ChromeTabUtils.switchTabInCurrentTabModel(cta, 0);
        enterTabSwitcher(cta);
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "3_incognito_ntps");
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testTabToGridFromLiveTab() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        assertEquals(0, mTabListDelegate.getSoftCleanupDelayForTesting());
        assertEquals(0, mTabListDelegate.getCleanupDelayForTesting());

        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @EnableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study")
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisabledTest(message = "crbug.com/991852 This test is flaky")
    public void testTabToGridFromLiveTabAnimation() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());

        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS + "/soft-cleanup-delay/2000/cleanup-delay/10000"})
    public void testTabToGridFromLiveTabWarm() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        assertEquals(2000, mTabListDelegate.getSoftCleanupDelayForTesting());
        assertEquals(10000, mTabListDelegate.getCleanupDelayForTesting());

        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @EnableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study")
    @CommandLineFlags.Add({BASE_PARAMS + "/soft-cleanup-delay/2000/cleanup-delay/10000"})
    @MinAndroidSdkLevel(Build.VERSION_CODES.M) // TODO(crbug.com/997065#c8): remove SDK restriction.
    @DisabledTest(message = "https://crbug.com/1207875")
    public void testTabToGridFromLiveTabWarmAnimation() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS + "/cleanup-delay/10000"})
    public void testTabToGridFromLiveTabSoft() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        assertEquals(0, mTabListDelegate.getSoftCleanupDelayForTesting());
        assertEquals(10000, mTabListDelegate.getCleanupDelayForTesting());

        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @EnableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study")
    @CommandLineFlags.Add({BASE_PARAMS + "/cleanup-delay/10000"})
    @MinAndroidSdkLevel(Build.VERSION_CODES.M) // TODO(crbug.com/997065#c8): remove SDK restriction.
    @DisabledTest(message = "https://crbug.com/1272561")
    public void testTabToGridFromLiveTabSoftAnimation() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(mUrl);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    @FlakyTest(message = "crbug.com/1075804")
    public void testTabToGridFromNtp() throws InterruptedException {
        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(NTP_URL);
    }

    /**
     * Make Chrome have {@code numTabs} of regular Tabs and {@code numIncognitoTabs} of incognito
     * tabs with {@code url} loaded, and assert no bitmap fetching occurred.
     *
     * @param numTabs The number of regular tabs.
     * @param numIncognitoTabs The number of incognito tabs.
     * @param url The URL to load.
     */
    private void prepareTabs(int numTabs, int numIncognitoTabs, @Nullable String url) {
        int oldCount = mTabListDelegate.getBitmapFetchCountForTesting();
        TabUiTestHelper.prepareTabsWithThumbnail(mActivityTestRule, numTabs, numIncognitoTabs, url);
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting() - oldCount);
    }

    private void testTabToGrid(String fromUrl) throws InterruptedException {
        mActivityTestRule.loadUrl(fromUrl);

        final int initCount = getCaptureCount();

        for (int i = 0; i < mRepeat; i++) {
            enterGTSWithThumbnailChecking();
            leaveGTSAndVerifyThumbnailsAreReleased();
        }
        checkFinalCaptureCount(false, initCount);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testGridToTabToCurrentNTP() throws InterruptedException {
        prepareTabs(1, 0, NTP_URL);
        testGridToTab(false, false);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testGridToTabToOtherNTP() throws InterruptedException {
        prepareTabs(2, 0, NTP_URL);
        testGridToTab(true, false);
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    public void testGridToTabToCurrentLive() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(1, 0, mUrl);
        testGridToTab(false, false);
    }

    // From https://stackoverflow.com/a/21505193
    private static boolean isEmulator() {
        return Build.FINGERPRINT.startsWith("generic") || Build.FINGERPRINT.startsWith("unknown")
                || Build.MODEL.contains("google_sdk") || Build.MODEL.contains("Emulator")
                || Build.MODEL.contains("Android SDK built for x86")
                || Build.MANUFACTURER.contains("Genymotion")
                || (Build.BRAND.startsWith("generic") && Build.DEVICE.startsWith("generic"))
                || "google_sdk".equals(Build.PRODUCT);
    }

    /**
     * Test that even if there are tabs with stuck pending thumbnail readback, it doesn't block
     * thumbnail readback for the current tab.
     */
    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @DisableIf.Build(message = "Flaky on emulators; see https://crbug.com/1094492",
            supported_abis_includes = "x86")
    public void
    testGridToTabToCurrentLiveDetached() throws Exception {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        // This works on emulators but not on real devices. See crbug.com/986047.
        if (!isEmulator()) return;

        for (int i = 0; i < 10; i++) {
            ChromeTabbedActivity cta = mActivityTestRule.getActivity();
            // Quickly create some tabs, navigate to web pages, and don't wait for thumbnail
            // capturing.
            mActivityTestRule.loadUrl(mUrl);
            ChromeTabUtils.newTabFromMenu(
                    InstrumentationRegistry.getInstrumentation(), cta, false, false);
            mActivityTestRule.loadUrl(mUrl);
            // Hopefully we are in a state where some pending readbacks are stuck because their tab
            // is not attached to the view.
            if (cta.getTabContentManager().getPendingReadbacksForTesting() > 0) {
                break;
            }

            // Restart Chrome.
            // Although we're destroying the activity, the Application will still live on since its
            // in the same process as this test.
            TestThreadUtils.runOnUiThreadBlocking(() -> cta.getTabModelSelector().closeAllTabs());
            TabUiTestHelper.finishActivity(cta);
            mActivityTestRule.startMainActivityOnBlankPage();
            assertEquals(1, mActivityTestRule.tabsCount(false));
        }
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        assertNotEquals(0, cta.getTabContentManager().getPendingReadbacksForTesting());
        assertEquals(1, cta.getCurrentTabModel().index());

        // The last tab should still get thumbnail even though readbacks for other tabs are stuck.
        enterTabSwitcher(cta);
        TabUiTestHelper.checkThumbnailsExist(cta.getTabModelSelector().getCurrentTab());
    }

    @Test
    @MediumTest
    @EnableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study")
    @DisabledTest(message = "crbug.com/993201 This test fails deterministically on Nexus 5X")
    public void testGridToTabToCurrentLiveWithAnimation() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(1, 0, mUrl);
        testGridToTab(false, false);
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @DisabledTest(message = "crbug.com/1313972")
    public void testGridToTabToOtherLive() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(2, 0, mUrl);
        testGridToTab(true, false);
    }

    @Test
    @MediumTest
    @EnableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study")
    @DisabledTest(message = "crbug.com/993201 This test fails deterministically on Nexus 5X")
    public void testGridToTabToOtherLiveWithAnimation() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(2, 0, mUrl);
        testGridToTab(true, false);
    }

    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @DisabledTest(message = "crbug.com/1237623 test is flaky")
    public void testGridToTabToOtherFrozen() throws InterruptedException {
        assertFalse(TabUiFeatureUtilities.isTabToGtsAnimationEnabled());
        prepareTabs(2, 0, mUrl);
        testGridToTab(true, true);
    }

    private void testGridToTab(boolean switchToAnotherTab, boolean killBeforeSwitching)
            throws InterruptedException {
        final int initCount = getCaptureCount();

        for (int i = 0; i < mRepeat; i++) {
            enterGTSWithThumbnailChecking();

            final int index = mActivityTestRule.getActivity().getCurrentTabModel().index();
            final int targetIndex = switchToAnotherTab ? 1 - index : index;
            Tab targetTab =
                    mActivityTestRule.getActivity().getCurrentTabModel().getTabAt(targetIndex);
            if (killBeforeSwitching) {
                WebContentsUtils.simulateRendererKilled(targetTab.getWebContents());
            }

            if (switchToAnotherTab) {
                waitForCaptureRateControl();
            }
            int count = getCaptureCount();
            onView(tabSwitcherViewMatcher()).perform(actionOnItemAtPosition(targetIndex, click()));
            CriteriaHelper.pollUiThread(() -> {
                boolean doneHiding =
                        !mActivityTestRule.getActivity().getLayoutManager().overviewVisible();
                if (!doneHiding) {
                    // Before overview hiding animation is done, the tab index should not change.
                    Criteria.checkThat(mActivityTestRule.getActivity().getCurrentTabModel().index(),
                            Matchers.is(index));
                }
                return doneHiding;
            }, "Overview not hidden yet");
            int delta;
            if (switchToAnotherTab
                    && !UrlUtilities.isNTPUrl(mActivityTestRule.getActivity()
                                                      .getCurrentWebContents()
                                                      .getLastCommittedUrl())) {
                // Capture the original tab.
                delta = 1;
            } else {
                delta = 0;
            }
            checkCaptureCount(delta, count);
        }
        checkFinalCaptureCount(switchToAnotherTab, initCount);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testRestoredTabsDontFetch() throws Exception {
        prepareTabs(2, 0, mUrl);
        int oldCount = mTabListDelegate.getBitmapFetchCountForTesting();

        // Restart Chrome.
        // Although we're destroying the activity, the Application will still live on since its in
        // the same process as this test.
        TabUiTestHelper.finishActivity(mActivityTestRule.getActivity());
        mActivityTestRule.startMainActivityOnBlankPage();
        assertEquals(3, mActivityTestRule.tabsCount(false));

        Layout layout = mActivityTestRule.getActivity().getLayoutManager().getOverviewLayout();
        assertTrue(layout instanceof StartSurfaceLayout);
        mStartSurfaceLayout = (StartSurfaceLayout) layout;
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting() - oldCount);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testInvisibleTabsDontFetch() throws InterruptedException {
        // Open a few new tabs.
        final int count = mTabListDelegate.getBitmapFetchCountForTesting();
        for (int i = 0; i < 3; i++) {
            MenuUtils.invokeCustomMenuActionSync(InstrumentationRegistry.getInstrumentation(),
                    mActivityTestRule.getActivity(), org.chromium.chrome.R.id.new_tab_menu_id);
        }
        // Fetching might not happen instantly.
        Thread.sleep(1000);

        // No fetching should happen.
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting() - count);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/soft-cleanup-delay/2000/cleanup-delay/10000"})
    @FlakyTest(message = "crbug.com/1075804")
    public void testInvisibleTabsDontFetchWarm() throws InterruptedException {
        // Get the GTS in the warm state.
        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(NTP_URL);

        Thread.sleep(1000);

        // Open a few new tabs.
        final int count = mTabListDelegate.getBitmapFetchCountForTesting();
        for (int i = 0; i < 3; i++) {
            MenuUtils.invokeCustomMenuActionSync(InstrumentationRegistry.getInstrumentation(),
                    mActivityTestRule.getActivity(), org.chromium.chrome.R.id.new_tab_menu_id);
        }
        // Fetching might not happen instantly.
        Thread.sleep(1000);

        // No fetching should happen.
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting() - count);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/cleanup-delay/10000"})
    @FlakyTest(message = "crbug.com/1075804")
    public void testInvisibleTabsDontFetchSoft() throws InterruptedException {
        // Get the GTS in the soft cleaned up state.
        prepareTabs(2, 0, NTP_URL);
        testTabToGrid(NTP_URL);

        Thread.sleep(1000);

        // Open a few new tabs.
        final int count = mTabListDelegate.getBitmapFetchCountForTesting();
        for (int i = 0; i < 3; i++) {
            MenuUtils.invokeCustomMenuActionSync(InstrumentationRegistry.getInstrumentation(),
                    mActivityTestRule.getActivity(), org.chromium.chrome.R.id.new_tab_menu_id);
        }
        // Fetching might not happen instantly.
        Thread.sleep(1000);

        // No fetching should happen.
        assertEquals(0, mTabListDelegate.getBitmapFetchCountForTesting() - count);
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisabledTest(message = "http://crbug/1005865 - Test was previously flaky but only on bots."
            + "Was not locally reproducible. Disabling until verified that it's deflaked on bots.")
    public void testIncognitoEnterGts() throws InterruptedException {
        // clang-format on
        prepareTabs(1, 1, null);
        enterGTSWithThumbnailChecking();
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));

        onView(tabSwitcherViewMatcher()).perform(actionOnItemAtPosition(0, click()));
        CriteriaHelper.pollUiThread(
                () -> !mActivityTestRule.getActivity().getLayoutManager().overviewVisible());

        enterGTSWithThumbnailChecking();
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testIncognitoToggle_tabCount() {
        mActivityTestRule.loadUrl(mUrl);
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();

        // Prepare two incognito tabs and enter tab switcher.
        createTabs(cta, true, 2);
        enterTabSwitcher(cta);
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(2));

        for (int i = 0; i < mRepeat; i++) {
            switchTabModel(cta, false);
            onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));

            switchTabModel(cta, true);
            onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(2));
        }
        leaveGTSAndVerifyThumbnailsAreReleased();
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @DisableIf.Build(sdk_is_less_than = Build.VERSION_CODES.M,
            message = "https://crbug.com/1023833")
    @DisabledTest(message = "https://crbug.com/1233169")
    public void testIncognitoToggle_thumbnailFetchCount() throws InterruptedException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        int oldFetchCount = mTabListDelegate.getBitmapFetchCountForTesting();

        // Prepare two incognito tabs and enter tab switcher.
        prepareTabs(1, 2, mUrl);
        enterGTSWithThumbnailChecking();

        int currentFetchCount = mTabListDelegate.getBitmapFetchCountForTesting();
        assertEquals(2, currentFetchCount - oldFetchCount);
        oldFetchCount = currentFetchCount;
        int oldHistogramRecord = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_JPEG);

        for (int i = 0; i < mRepeat; i++) {
            switchTabModel(cta, false);
            currentFetchCount = mTabListDelegate.getBitmapFetchCountForTesting();
            int currentHistogramRecord = RecordHistogram.getHistogramValueCountForTesting(
                    TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                    TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
            assertEquals(1, currentFetchCount - oldFetchCount);
            assertEquals(1, currentHistogramRecord - oldHistogramRecord);
            oldFetchCount = currentFetchCount;
            oldHistogramRecord = currentHistogramRecord;

            switchTabModel(cta, true);
            currentFetchCount = mTabListDelegate.getBitmapFetchCountForTesting();
            currentHistogramRecord = RecordHistogram.getHistogramValueCountForTesting(
                    TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                    TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
            assertEquals(2, currentFetchCount - oldFetchCount);
            assertEquals(2, currentHistogramRecord - oldHistogramRecord);
            oldFetchCount = currentFetchCount;
            oldHistogramRecord = currentHistogramRecord;
        }
        leaveGTSAndVerifyThumbnailsAreReleased();
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
                    ChromeFeatureList.TAB_GROUPS_CONTINUATION_ANDROID,
                    ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testUrlUpdatedNotCrashing_ForUndoableClosedTab() throws Exception {
        // clang-format on
        mActivityTestRule.getActivity().getSnackbarManager().disableForTesting();
        prepareTabs(2, 0, null);
        enterGTSWithThumbnailChecking();

        Tab tab = mActivityTestRule.getActivity().getTabModelSelector().getCurrentTab();
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            mActivityTestRule.getActivity().getTabModelSelector().getCurrentModel().closeTab(
                    tab, false, false, true);
        });
        mActivityTestRule.loadUrlInTab(
                mUrl, PageTransition.TYPED | PageTransition.FROM_ADDRESS_BAR, tab);
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_GROUPS_CONTINUATION_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testUrlUpdatedNotCrashing_ForTabNotInCurrentModel() throws Exception {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(1, 1, null);
        enterGTSWithThumbnailChecking();

        Tab tab = cta.getTabModelSelector().getCurrentTab();
        switchTabModel(cta, false);

        mActivityTestRule.loadUrlInTab(
                mUrl, PageTransition.TYPED | PageTransition.FROM_ADDRESS_BAR, tab);
    }

    private int getTabCountInCurrentTabModel() {
        return mActivityTestRule.getActivity().getTabModelSelector().getCurrentModel().getCount();
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0/"
        + "thumbnail_aspect_ratio/1.0"})
    public void testTabSuggestionMessageCard_dismiss() throws InterruptedException {
        // clang-format on
        prepareTabs(3, 0, null);

        // TODO(meiliang): Avoid using static variable for tracking state,
        // TabSuggestionMessageService.isSuggestionAvailableForTesting(). Instead, we can add a
        // dummy MessageObserver to track the availability of the suggestions.
        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        enterGTSWithThumbnailChecking();

        // TODO(meiliang): Avoid using static variable for tracking state,
        // TabSwitcherCoordinator::hasAppendedMessagesForTesting. Instead, we can query the number
        // of items that the inner model of the TabSwitcher has.
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        onView(withId(R.id.tab_grid_message_item)).check(matches(isDisplayed()));
        onView(allOf(withId(R.id.close_button), withParent(withId(R.id.tab_grid_message_item))))
                .perform(click());
        onView(withId(R.id.tab_grid_message_item)).check(doesNotExist());
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0"
        + "/thumbnail_aspect_ratio/1.0"})
    public void testTabSuggestionMessageCard_review() throws InterruptedException {
        // clang-format on
        prepareTabs(3, 0, null);

        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        enterGTSWithThumbnailChecking();

        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        onView(withId(R.id.tab_grid_message_item)).check(matches(isDisplayed()));
        onView(allOf(withId(R.id.action_button), withParent(withId(R.id.tab_grid_message_item))))
                .perform(click());

        TabSelectionEditorTestingRobot tabSelectionEditorTestingRobot =
                new TabSelectionEditorTestingRobot();
        tabSelectionEditorTestingRobot.resultRobot.verifyTabSelectionEditorIsVisible();

        Espresso.pressBack();
        tabSelectionEditorTestingRobot.resultRobot.verifyTabSelectionEditorIsHidden();
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    @FlakyTest(message = "https://crbug.com/1230107")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0/"
        + "thumbnail_aspect_ratio/1.0"})
    public void testShowOnlyOneTabSuggestionMessageCard_withSoftCleanup()
            throws InterruptedException {
        // clang-format on
        verifyOnlyOneTabSuggestionMessageCardIsShowing();
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0/"
        + "thumbnail_aspect_ratio/1.0"})
    @FlakyTest(message = "https://crbug.com/1198484")
    public void testShowOnlyOneTabSuggestionMessageCard_withHardCleanup()
            throws InterruptedException {
        // clang-format on
        verifyOnlyOneTabSuggestionMessageCardIsShowing();
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0/"
        + "thumbnail_aspect_ratio/1.0"})
    @DisabledTest(message = "https://crbug.com/1311825")
    public void testTabSuggestionMessageCardDismissAfterTabClosing() throws InterruptedException {
        // clang-format on
        prepareTabs(3, 0, mUrl);
        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        enterGTSWithThumbnailChecking();
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        onView(withId(R.id.tab_grid_message_item)).check(matches(isDisplayed()));

        closeFirstTabInTabSwitcher();

        CriteriaHelper.pollUiThread(
                () -> !TabSuggestionMessageService.isSuggestionAvailableForTesting());
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(2)));

        onView(tabSwitcherViewMatcher())
                .check(TabUiTestHelper.ChildrenCountAssertion.havingTabSuggestionMessageCardCount(
                        0));
        onView(withId(R.id.tab_grid_message_item)).check(doesNotExist());
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0/"
        + "thumbnail_aspect_ratio/1.0"})
    public void testTabSuggestionMessageCard_orientation() throws InterruptedException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(3, 0, null);

        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        enterGTSWithThumbnailChecking();
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);

        // Force portrait mode since the device can be wrongly in landscape. See crbug/1063639.
        ActivityTestUtils.rotateActivityToOrientation(cta, Configuration.ORIENTATION_PORTRAIT);

        onView(tabSwitcherViewMatcher())
                .check(MessageCardWidthAssertion.checkMessageItemSpanSize(3, 2));

        ActivityTestUtils.rotateActivityToOrientation(cta, Configuration.ORIENTATION_LANDSCAPE);

        onView(tabSwitcherViewMatcher())
                .check(MessageCardWidthAssertion.checkMessageItemSpanSize(3, 3));
    }

    private static class MessageCardWidthAssertion implements ViewAssertion {
        private int mIndex;
        private int mSpanCount;

        public static MessageCardWidthAssertion checkMessageItemSpanSize(int index, int spanCount) {
            return new MessageCardWidthAssertion(index, spanCount);
        }

        public MessageCardWidthAssertion(int index, int spanCount) {
            mIndex = index;
            mSpanCount = spanCount;
        }

        @Override
        public void check(View view, NoMatchingViewException noMatchException) {
            if (noMatchException != null) throw noMatchException;
            float tabListPadding =
                    TabUiThemeProvider.getTabGridCardMarginForDialogAnimation(view.getContext());
            float messageCardMargin =
                    TabUiThemeProvider.getMessageCardMarginDimension(view.getContext());

            assertTrue(view instanceof RecyclerView);
            RecyclerView recyclerView = (RecyclerView) view;
            GridLayoutManager layoutManager = (GridLayoutManager) recyclerView.getLayoutManager();
            assertEquals(mSpanCount, layoutManager.getSpanCount());

            RecyclerView.ViewHolder messageItemViewHolder =
                    recyclerView.findViewHolderForAdapterPosition(mIndex);
            assertNotNull(messageItemViewHolder);
            assertEquals(TabProperties.UiType.MESSAGE, messageItemViewHolder.getItemViewType());
            View messageItemView = messageItemViewHolder.itemView;

            // The message card item width should always be recyclerView width minus padding and
            // margin.
            assertEquals(recyclerView.getWidth() - 2 * tabListPadding - 2 * messageCardMargin,
                    (float) messageItemView.getWidth(), 1.0f);
        }
    }

    @Test
    @MediumTest
    @Feature("NewTabTile")
    // clang-format off
    @DisableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION,
            ChromeFeatureList.CLOSE_TAB_SUGGESTIONS})
    @CommandLineFlags.Add({BASE_PARAMS + "/tab_grid_layout_android_new_tab_tile/false"
            + "/tab_grid_layout_android_new_tab/false"})
    public void testNewTabTile_Disabled() throws InterruptedException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(2, 0, null);

        enterGTSWithThumbnailChecking();

        onView(withId(R.id.new_tab_tile)).check(doesNotExist());
        verifyTabSwitcherCardCount(cta, 2);

        switchTabModel(cta, true);
        onView(withId(R.id.new_tab_tile)).check(doesNotExist());
        verifyTabSwitcherCardCount(cta, 0);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisabledTest(message = "https://crbug.com/1122657")
    public void testThumbnailAspectRatio_default() {
        prepareTabs(2, 0, mUrl);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(1.0));
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/0.75"})
    @DisabledTest(message = "https://crbug.com/1122657")
    public void testThumbnailAspectRatio_point75() {
        prepareTabs(2, 0, mUrl);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(0.75));
        leaveGTSAndVerifyThumbnailsAreReleased();

        Tab tab = mActivityTestRule.getActivity().getTabModelSelector().getCurrentTab();
        mActivityTestRule.loadUrlInTab(
                NTP_URL, PageTransition.TYPED | PageTransition.FROM_ADDRESS_BAR, tab);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(0.75));
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/2.0/allow_to_refetch/true"})
    @DisabledTest(message = "Flaky - https://crbug.com/1124041")
    public void testThumbnailAspectRatio_fromTwoToPoint75() throws Exception {
        prepareTabs(2, 0, mUrl);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(2.0));
        TabModel currentTabModel =
                mActivityTestRule.getActivity().getTabModelSelector().getCurrentModel();
        for (int i = 0; i < currentTabModel.getCount(); i++) {
            TabUiTestHelper.checkThumbnailsExist(currentTabModel.getTabAt(i));
        }
        leaveGTSAndVerifyThumbnailsAreReleased();

        simulateAspectRatioChangedToPoint75();
        verifyAllThumbnailHasAspectRatio(0.75);

        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(2.0));
        TabUiTestHelper.finishActivity(mActivityTestRule.getActivity());
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    @DisabledTest(message = "https://crbug.com/1138739")
    public void testThumbnailFetchingResult_defaultAspectRatio() throws Exception {
        prepareTabs(2, 0, mUrl);
        int oldJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
        int oldEtc1Count = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_ETC1);
        int oldNothingCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_NOTHING);
        int oldDifferentAspectRatioJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG);

        enterTabSwitcher(mActivityTestRule.getActivity());
        int currentJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
        int currentEtc1Count = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_ETC1);
        int currentNothingCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_NOTHING);
        int currentDifferentAspectRatioJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG);

        assertEquals(1, currentJpegCount - oldJpegCount);
        assertEquals(0, currentEtc1Count - oldEtc1Count);
        assertEquals(0, currentDifferentAspectRatioJpegCount - oldDifferentAspectRatioJpegCount);
        // Get thumbnail from a live layer.
        assertEquals(1, currentNothingCount - oldNothingCount);

        oldJpegCount = currentJpegCount;
        oldEtc1Count = currentEtc1Count;
        oldNothingCount = currentNothingCount;
        oldDifferentAspectRatioJpegCount = currentDifferentAspectRatioJpegCount;

        TabModel currentTabModel =
                mActivityTestRule.getActivity().getTabModelSelector().getCurrentModel();
        for (int i = 0; i < currentTabModel.getCount(); i++) {
            TabUiTestHelper.checkThumbnailsExist(currentTabModel.getTabAt(i));
        }

        TabUiTestHelper.finishActivity(mActivityTestRule.getActivity());
        mActivityTestRule.startMainActivityFromLauncher();

        enterTabSwitcher(mActivityTestRule.getActivity());
        assertEquals(2,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_JPEG)
                        - oldJpegCount);
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_ETC1)
                        - oldEtc1Count);
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_NOTHING)
                        - oldNothingCount);
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG)
                        - oldDifferentAspectRatioJpegCount);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/2.0/allow_to_refetch/true"})
    @DisabledTest(message = "http://crbug/1119527 - Flaky on bots.")
    public void testThumbnailFetchingResult_changingAspectRatio() throws Exception {
        prepareTabs(2, 0, mUrl);
        int oldJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
        int oldEtc1Count = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_ETC1);
        int oldNothingCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_NOTHING);
        int oldDifferentAspectRatioJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG);

        enterTabSwitcher(mActivityTestRule.getActivity());
        int currentJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_JPEG);
        int currentEtc1Count = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_ETC1);
        int currentNothingCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_NOTHING);
        int currentDifferentAspectRatioJpegCount = RecordHistogram.getHistogramValueCountForTesting(
                TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG);

        assertEquals(1, currentJpegCount - oldJpegCount);
        assertEquals(0, currentEtc1Count - oldEtc1Count);
        assertEquals(0, currentDifferentAspectRatioJpegCount - oldDifferentAspectRatioJpegCount);
        // Get thumbnail from a live layer.
        assertEquals(1, currentNothingCount - oldNothingCount);

        oldJpegCount = currentJpegCount;
        oldEtc1Count = currentEtc1Count;
        oldNothingCount = currentNothingCount;
        oldDifferentAspectRatioJpegCount = currentDifferentAspectRatioJpegCount;

        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(2.0));

        TabModel currentTabModel =
                mActivityTestRule.getActivity().getTabModelSelector().getCurrentModel();
        for (int i = 0; i < currentTabModel.getCount(); i++) {
            TabUiTestHelper.checkThumbnailsExist(currentTabModel.getTabAt(i));
        }
        leaveGTSAndVerifyThumbnailsAreReleased();

        simulateAspectRatioChangedToPoint75();
        verifyAllThumbnailHasAspectRatio(0.75);

        enterTabSwitcher(mActivityTestRule.getActivity());
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_JPEG)
                        - oldJpegCount);
        assertEquals(2,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_DIFFERENT_ASPECT_RATIO_JPEG)
                        - oldDifferentAspectRatioJpegCount);
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_ETC1)
                        - oldEtc1Count);
        assertEquals(0,
                RecordHistogram.getHistogramValueCountForTesting(
                        TabContentManager.UMA_THUMBNAIL_FETCHING_RESULT,
                        TabContentManager.ThumbnailFetchingResult.GOT_NOTHING)
                        - oldNothingCount);
        onView(tabSwitcherViewMatcher())
                .check(ThumbnailAspectRatioAssertion.havingAspectRatio(2.0));
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS})
    public void testRecycling_defaultAspectRatio() {
        prepareTabs(10, 0, mUrl);
        ChromeTabUtils.switchTabInCurrentTabModel(mActivityTestRule.getActivity(), 0);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher()).perform(RecyclerViewActions.scrollToPosition(9));
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/0.75"})
    public void testRecycling_aspectRatioPoint75() {
        // clang-format on
        prepareTabs(10, 0, mUrl);
        ChromeTabUtils.switchTabInCurrentTabModel(mActivityTestRule.getActivity(), 0);
        enterTabSwitcher(mActivityTestRule.getActivity());
        onView(tabSwitcherViewMatcher()).perform(RecyclerViewActions.scrollToPosition(9));
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/0.75"})
    public void testExpandTab_withAspectRatioPoint75() {
        // clang-format on
        prepareTabs(1, 0, mUrl);
        enterTabSwitcher(mActivityTestRule.getActivity());
        leaveGTSAndVerifyThumbnailsAreReleased();
    }

    @Test
    @MediumTest
    @Feature({"RenderTest"})
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/thumbnail_aspect_ratio/1.0"})
    @DisabledTest(message = "https://crbug.com/1205952")
    public void testRenderGrid_withAspectRatioOfOne() throws IOException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(3, 0, "about:blank");
        TabModel normalTabModel = cta.getTabModelSelector().getModel(false);
        List<Tab> tabGroup = new ArrayList<>(
                Arrays.asList(normalTabModel.getTabAt(0), normalTabModel.getTabAt(1)));
        createTabGroup(cta, false, tabGroup);
        // Make sure all tabs have thumbnail.
        enterGTSWithThumbnailRetry();
        mRenderTestRule.render(cta.findViewById(R.id.tab_list_view), "aspect_ratio_of_one");
    }

    @Test
    @MediumTest
    @Feature("NewTabVariation")
    @DisabledTest(message = "https://crbug.com/1144666")
    // clang-format off
    @Features.DisableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION,
            ChromeFeatureList.CLOSE_TAB_SUGGESTIONS})
    @CommandLineFlags.Add({BASE_PARAMS + "/tab_grid_layout_android_new_tab/NewTabVariation"})
    public void testNewTabVariation() {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        prepareTabs(2, 0, null);
        enterTabSwitcher(cta);
        verifyTabModelTabCount(cta, 2, 0);
        checkNewTabVariationVisibility(true);

        createTabs(cta, true, 1);
        verifyTabModelTabCount(cta, 2, 1);
        enterTabSwitcher(cta);
        checkNewTabVariationVisibility(false);

        switchTabModel(cta, false);
        checkNewTabVariationVisibility(false);

        switchTabModel(cta, true);
        checkNewTabVariationVisibility(false);

        closeFirstTabInTabSwitcher();
        verifyTabModelTabCount(cta, 2, 0);
        checkNewTabVariationVisibility(true);

        createTabs(cta, true, 2);
        verifyTabModelTabCount(cta, 2, 2);
        enterTabSwitcher(cta);
        checkNewTabVariationVisibility(false);

        MenuUtils.invokeCustomMenuActionSync(InstrumentationRegistry.getInstrumentation(), cta,
                R.id.close_all_incognito_tabs_menu_id);
        verifyTabModelTabCount(cta, 2, 0);
        checkNewTabVariationVisibility(true);
    }

    private void checkNewTabVariationVisibility(boolean isVisible) {
        if (isVisible) {
            onView(allOf(withId(R.id.incognito_toggle_tabs),
                           withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(withEffectiveVisibility(GONE)));
            onView(allOf(withId(R.id.new_tab_button),
                           withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(withEffectiveVisibility(GONE)));
            onView(allOf(withId(R.id.new_tab_view), withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(isDisplayed()));
        } else {
            onView(allOf(withId(R.id.incognito_toggle_tabs),
                           withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(isDisplayed()));
            onView(allOf(withId(R.id.new_tab_button),
                           withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(isDisplayed()));
            onView(allOf(withId(R.id.new_tab_view), withParent(withId(R.id.tab_switcher_toolbar))))
                    .check(matches(withEffectiveVisibility(GONE)));
        }
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_GROUPS_CONTINUATION_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<study"})
    @DisabledTest(message = "https://crbug.com/1130212")
    public void testCloseTabViaCloseButton() throws Exception {
        // clang-format on
        mActivityTestRule.getActivity().getSnackbarManager().disableForTesting();
        prepareTabs(1, 0, null);
        enterGTSWithThumbnailChecking();

        onView(allOf(withId(R.id.action_button), withParent(withId(R.id.content_view)),
                       withEffectiveVisibility(VISIBLE)))
                .perform(click());
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({BASE_PARAMS})
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<study"})
    @DisabledTest(message = "Flaky - https://crbug.com/1124041")
    public void testSwipeToDismiss_GTS() {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        // Create 3 tabs and merge the first two tabs into one group.
        createTabs(cta, false, 3);
        enterTabSwitcher(cta);
        TabModel normalTabModel = cta.getTabModelSelector().getModel(false);
        List<Tab> tabGroup = new ArrayList<>(
                Arrays.asList(normalTabModel.getTabAt(0), normalTabModel.getTabAt(1)));
        createTabGroup(cta, false, tabGroup);
        verifyTabSwitcherCardCount(cta, 2);
        verifyTabModelTabCount(cta, 3, 0);

        // Swipe to dismiss a single tab card.
        onView(tabSwitcherViewMatcher())
                .perform(RecyclerViewActions.actionOnItemAtPosition(
                        1, getSwipeToDismissAction(false)));
        verifyTabSwitcherCardCount(cta, 1);
        verifyTabModelTabCount(cta, 2, 0);

        // Swipe to dismiss a tab group card.
        onView(tabSwitcherViewMatcher())
                .perform(RecyclerViewActions.actionOnItemAtPosition(
                        0, getSwipeToDismissAction(true)));
        verifyTabSwitcherCardCount(cta, 0);
        verifyTabModelTabCount(cta, 0, 0);
    }

    @Test
    @MediumTest
    // clang-format off
    @DisabledTest(message = "https://crbug.com/1144666")
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_GROUPS_CONTINUATION_ANDROID + "<study",
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<study"})
    @CommandLineFlags.Add({BASE_PARAMS + "/enable_launch_polish/true"})
    public void testCloseButtonDescription() {
        String expectedDescription = "Close New tab tab";
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        enterTabSwitcher(cta);

        // Test single tab.
        onView(allOf(withParent(withId(R.id.content_view)), withId(R.id.action_button),
                       withEffectiveVisibility(VISIBLE)))
                .check(ViewContentDescription.havingDescription(expectedDescription));

        // Create 2 tabs and merge them into one group.
        createTabs(cta, false, 2);
        enterTabSwitcher(cta);
        TabModel normalTabModel = cta.getTabModelSelector().getModel(false);
        List<Tab> tabGroup = new ArrayList<>(
                Arrays.asList(normalTabModel.getTabAt(0), normalTabModel.getTabAt(1)));
        createTabGroup(cta, false, tabGroup);
        verifyTabSwitcherCardCount(cta, 1);

        // Test group tab.
        expectedDescription = "Close tab group with 2 tabs";
        onView(allOf(withParent(withId(R.id.content_view)), withId(R.id.action_button),
                       withEffectiveVisibility(VISIBLE)))
                .check(ViewContentDescription.havingDescription(expectedDescription));
    }

    private static class ViewContentDescription implements ViewAssertion {
        private String mExpectedDescription;

        public static ViewContentDescription havingDescription(String description) {
            return new ViewContentDescription(description);
        }

        public ViewContentDescription(String description) {
            mExpectedDescription = description;
        }

        @Override
        public void check(View view, NoMatchingViewException noMatchException) {
            if (noMatchException != null) throw noMatchException;

            assertEquals(mExpectedDescription, view.getContentDescription());
        }
    }

    private static class TabCountAssertion implements ViewAssertion {
        private int mExpectedCount;

        public static TabCountAssertion havingTabCount(int tabCount) {
            return new TabCountAssertion(tabCount);
        }

        public TabCountAssertion(int expectedCount) {
            mExpectedCount = expectedCount;
        }

        @Override
        public void check(View view, NoMatchingViewException noMatchException) {
            if (noMatchException != null) throw noMatchException;

            RecyclerView.Adapter adapter = ((RecyclerView) view).getAdapter();
            assertEquals(mExpectedCount, adapter.getItemCount());
        }
    }

    private static class ThumbnailAspectRatioAssertion implements ViewAssertion {
        private double mExpectedRatio;

        public static ThumbnailAspectRatioAssertion havingAspectRatio(double ratio) {
            return new ThumbnailAspectRatioAssertion(ratio);
        }

        private ThumbnailAspectRatioAssertion(double expectedRatio) {
            mExpectedRatio = expectedRatio;
        }

        @Override
        public void check(View view, NoMatchingViewException noMatchException) {
            if (noMatchException != null) throw noMatchException;

            RecyclerView recyclerView = (RecyclerView) view;

            RecyclerView.Adapter adapter = recyclerView.getAdapter();
            for (int i = 0; i < adapter.getItemCount(); i++) {
                RecyclerView.ViewHolder viewHolder =
                        recyclerView.findViewHolderForAdapterPosition(i);
                if (viewHolder != null) {
                    ViewLookupCachingFrameLayout tabView =
                            (ViewLookupCachingFrameLayout) viewHolder.itemView;
                    ImageView thumbnail = (ImageView) tabView.fastFindViewById(R.id.tab_thumbnail);
                    BitmapDrawable drawable = (BitmapDrawable) thumbnail.getDrawable();
                    Bitmap bitmap = drawable.getBitmap();
                    double bitmapRatio = bitmap.getWidth() * 1.0 / bitmap.getHeight();
                    assertTrue(
                            "Actual ratio: " + bitmapRatio + "; Expected ratio: " + mExpectedRatio,
                            Math.abs(bitmapRatio - mExpectedRatio)
                                    <= TabContentManager.ASPECT_RATIO_PRECISION);
                }
            }
        }
    }

    private void waitForLastSearchTerm(Tab tab, String expected) {
        CriteriaHelper.pollUiThread(() -> {
            Criteria.checkThat(
                    TabAttributeCache.getLastSearchTerm(tab.getId()), Matchers.is(expected));
        });
    }

    @Test
    @MediumTest
    // Disable TAB_TO_GTS_ANIMATION to make it less flaky.
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS + "/enable_search_term_chip/true"})
    public void testSearchTermChip_noChip() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.ENABLE_SEARCH_CHIP.getValue());
        prepareTabs(1, 0, mUrl);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(allOf(withId(R.id.page_info_button),
                       isDescendantOfA(withId(R.id.compositor_view_holder))))
                .check(matches(not(isDisplayed())));
    }

    @Test
    @MediumTest
    // Disable TAB_TO_GTS_ANIMATION to make it less flaky.
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS + "/enable_search_term_chip/true"})
    @DisabledTest(message = "http://crbug/1120822 - Flaky on bots.")
    public void testSearchTermChip_withChip() throws InterruptedException {
        assertTrue(TabUiFeatureUtilities.ENABLE_SEARCH_CHIP.getValue());
        // Make sure we support RTL and CJKV languages.
        String searchTermWithSpecialCodePoints = "a\n ئۇيغۇرچە\u200E漢字";
        // Special code points like new line (\n) and left-to-right marker (‎‎‎\u200E) should
        // be stripped out. See TabAttributeCache#removeEscapedCodePoints for more details.
        String expectedTerm = "a ئۇيغۇرچە漢字";

        String anotherTerm = "hello world";

        // Do search, and verify the chip is still not shown.
        AtomicReference<String> searchUrl = new AtomicReference<>();
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        Tab currentTab = cta.getTabModelSelector().getCurrentTab();
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            TemplateUrlServiceFactory.get().setSearchEngine("google.com");
            searchUrl.set(TemplateUrlServiceFactory.get().getUrlForSearchQuery(
                    searchTermWithSpecialCodePoints));
            currentTab.loadUrl(new LoadUrlParams(searchUrl.get()));
        });
        ChromeTabUtils.waitForTabPageLoaded(currentTab, null);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(withId(R.id.page_info_button)).check(matches(not(isDisplayed())));
        Espresso.pressBack();

        // Navigate, and verify the chip is shown.
        mActivityTestRule.loadUrl(mUrl);
        waitForLastSearchTerm(currentTab, expectedTerm);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(withId(R.id.page_info_button))
                .check(waitForView(allOf(withText(expectedTerm), isDisplayed())));
        Espresso.pressBack();

        // Do another search, and verify the chip is gone.
        AtomicReference<String> searchUrl2 = new AtomicReference<>();
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            TemplateUrlServiceFactory.get().setSearchEngine("google.com");
            searchUrl2.set(TemplateUrlServiceFactory.get().getUrlForSearchQuery(anotherTerm));
            currentTab.loadUrl(new LoadUrlParams(searchUrl2.get()));
        });
        ChromeTabUtils.waitForTabPageLoaded(currentTab, null);
        waitForLastSearchTerm(currentTab, null);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(withId(R.id.page_info_button)).check(matches(not(isDisplayed())));
        Espresso.pressBack();

        // Back to previous page, and verify the chip is back.
        Espresso.pressBack();
        waitForLastSearchTerm(currentTab, expectedTerm);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(withId(R.id.page_info_button))
                .check(waitForView(allOf(withText(expectedTerm), isDisplayed())));

        // Click the chip and check the tab navigates back to the search result page.
        assertEquals(mUrl, ChromeTabUtils.getUrlStringOnUiThread(currentTab));
        onView(withId(R.id.page_info_button))
                .check(waitForView(allOf(withText(expectedTerm), isDisplayed())));
        onView(withId(R.id.page_info_button)).perform(click());
        LayoutTestUtils.waitForLayout(cta.getLayoutManager(), LayoutType.BROWSING);
        ChromeTabUtils.waitForTabPageLoaded(currentTab, searchUrl.get());

        // Verify the chip is gone.
        waitForLastSearchTerm(currentTab, null);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(withId(R.id.page_info_button)).check(matches(not(isDisplayed())));
    }

    @Test
    @MediumTest
    // clang-format off
    // Disable TAB_TO_GTS_ANIMATION to make it less flaky.
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS +
            "/enable_search_term_chip/true/enable_search_term_chip_adaptive_icon/true"})
    public void testSearchTermChip_adaptiveIcon() throws InterruptedException {
        // clang-format on
        assertTrue(TabUiFeatureUtilities.ENABLE_SEARCH_CHIP.getValue());
        assertTrue(TabUiFeatureUtilities.ENABLE_SEARCH_CHIP_ADAPTIVE.getValue());
        String searchTerm = "hello world";

        // Do search, and verify the chip is still not shown.
        AtomicReference<String> searchUrl = new AtomicReference<>();
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        Tab currentTab = cta.getTabModelSelector().getCurrentTab();
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            TemplateUrlServiceFactory.get().setSearchEngine("google.com");
            searchUrl.set(TemplateUrlServiceFactory.get().getUrlForSearchQuery(searchTerm));
            currentTab.loadUrl(new LoadUrlParams(searchUrl.get()));
        });
        ChromeTabUtils.waitForTabPageLoaded(currentTab, null);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(allOf(withId(R.id.page_info_button),
                       isDescendantOfA(withId(R.id.compositor_view_holder))))
                .check(matches(not(isDisplayed())));
        Espresso.pressBack();

        // Navigate, and verify the chip is shown.
        mActivityTestRule.loadUrl(mUrl);
        waitForLastSearchTerm(currentTab, searchTerm);
        enterTabSwitcher(mActivityTestRule.getActivity());

        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(1));
        onView(allOf(withId(R.id.page_info_button),
                       isDescendantOfA(withId(R.id.compositor_view_holder))))
                .check(waitForView(allOf(withText(searchTerm), isDisplayed())));

        // Switch the default search engine from google.com to yahoo.com, the search chip icon
        // should change.
        RecyclerView tabListRecyclerView = cta.findViewById(R.id.tab_list_view);
        ChipView chipView =
                tabListRecyclerView.findViewHolderForAdapterPosition(0).itemView.findViewById(
                        R.id.page_info_button);
        ChromeImageView iconImageView = (ChromeImageView) chipView.getChildAt(0);
        Drawable googleDrawable = iconImageView.getDrawable();

        TestThreadUtils.runOnUiThreadBlocking(
                () -> TemplateUrlServiceFactory.get().setSearchEngine("yahoo.com"));

        assertNotEquals(googleDrawable, iconImageView.getDrawable());
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID})
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @FlakyTest(message = "crbug.com/1096997")
    public void testTabGroupManualSelection() throws InterruptedException {
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        TabSelectionEditorTestingRobot robot = new TabSelectionEditorTestingRobot();
        createTabs(cta, false, 3);
        enterTabSwitcher(cta);
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(3));

        enterTabGroupManualSelection(cta);
        robot.resultRobot.verifyTabSelectionEditorIsVisible();

        // Group first two tabs.
        robot.actionRobot.clickItemAtAdapterPosition(0);
        robot.actionRobot.clickItemAtAdapterPosition(1);
        robot.actionRobot.clickToolbarActionButton();

        // Exit manual selection mode, back to tab switcher.
        robot.resultRobot.verifyTabSelectionEditorIsHidden();
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(2));
        onViewWaiting(withText("2 tabs grouped"));
    }

    @Test
    @MediumTest
    // clang-format off
    @DisabledTest(message = "https://crbug.com/1144666")
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testTabGroupManualSelection_DisabledForSingleTab() {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 1);

        // Group option should be disabled when there is only one single tab.
        // We are using UiDevice to finish the click here since there seems to be more than one menu
        // button in the view hierarchy, and we couldn't locate it using espresso approach. Also,
        // performClick() won't work as the logic that handles menu button lies in onTouchListener.
        View menuButton =
                cta.findViewById(R.id.tab_switcher_toolbar).findViewById(R.id.menu_button);
        int[] location = new int[2];
        menuButton.getLocationOnScreen(location);
        UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
                .click(location[0], location[1]);
        onView(withText("Group tabs"))
                .inRoot(withDecorView(not(cta.getWindow().getDecorView())))
                .check(matches(not(isEnabled())));

        // Group option should be enabled when there is more than one single tab.
        createTabs(cta, false, 2);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 2);
        UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
                .click(location[0], location[1]);
        onView(withText("Group tabs"))
                .inRoot(withDecorView(not(cta.getWindow().getDecorView())))
                .check(matches(isEnabled()));
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testTabGroupManualSelection_SystemBackDismiss() {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        TabSelectionEditorTestingRobot robot = new TabSelectionEditorTestingRobot();
        createTabs(cta, false, 2);
        enterTabSwitcher(cta);
        onView(tabSwitcherViewMatcher()).check(TabCountAssertion.havingTabCount(2));
        enterTabGroupManualSelection(cta);
        robot.resultRobot.verifyTabSelectionEditorIsVisible();

        // Pressing system back should dismiss the selection editor.
        Espresso.pressBack();
        robot.resultRobot.verifyTabSelectionEditorIsHidden();
    }

    @Test
    @MediumTest
    @Feature("TabSuggestion")
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.CLOSE_TAB_SUGGESTIONS + "<Study"})
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    @CommandLineFlags.Add({BASE_PARAMS + "/baseline_tab_suggestions/true" +
            "/baseline_close_tab_suggestions/true/min_time_between_prefetches/0" +
            "/thumbnail_aspect_ratio/1.0"})
    public void testTabGroupManualSelection_AfterReviewTabSuggestion() throws InterruptedException {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        TabSelectionEditorTestingRobot robot = new TabSelectionEditorTestingRobot();
        createTabs(cta, false, 3);

        // Review closing tab suggestion.
        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        // Entering GTS with thumbnail checking here is trying to reduce flakiness that is caused by
        // the TabContextObserver. TabContextObserver listens to
        // TabObserver#didFirstVisuallyNonEmptyPaint and invalidates the suggestion. Do the
        // thumbnail checking here is to ensure the suggestion is valid when entering tab switcher.
        enterGTSWithThumbnailChecking();
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        onView(withId(R.id.tab_grid_message_item)).check(matches(isDisplayed()));
        onView(allOf(withId(R.id.action_button), withParent(withId(R.id.tab_grid_message_item))))
                .perform(click());

        robot.resultRobot.verifyTabSelectionEditorIsVisible()
                .verifyToolbarActionButtonWithResourceId(
                        R.string.tab_suggestion_close_tab_action_button);

        robot.actionRobot.clickToolbarActionButton();
        robot.resultRobot.verifyTabSelectionEditorIsHidden();
        CriteriaHelper.pollUiThread(() -> {
            Criteria.checkThat(mActivityTestRule.getActivity().getCurrentTabModel().getCount(),
                    Matchers.is(0));
        });

        // Show Manual Selection Mode.
        createTabs(cta, false, 3);

        TabUiTestHelper.enterTabSwitcher(mActivityTestRule.getActivity());
        enterTabGroupManualSelection(cta);
        robot.resultRobot.verifyTabSelectionEditorIsVisible()
                .verifyToolbarActionButtonWithResourceId(R.string.tab_selection_editor_group);

        // Group first two tabs.
        robot.actionRobot.clickItemAtAdapterPosition(0);
        robot.actionRobot.clickItemAtAdapterPosition(1);
        robot.actionRobot.clickToolbarActionButton();

        // Exit manual selection mode, back to tab switcher.
        robot.resultRobot.verifyTabSelectionEditorIsHidden();
        onViewWaiting(withText("2 tabs grouped"));
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    @DisabledTest(message = "crbug.com/1187320 This doesn't work with FeedV2.")
    public void testActivityCanBeGarbageCollectedAfterFinished() throws Exception {
        prepareTabs(1, 0, "about:blank");

        WeakReference<ChromeTabbedActivity> activityRef =
                new WeakReference<>(mActivityTestRule.getActivity());

        ChromeTabbedActivity activity =
                ApplicationTestUtils.recreateActivity(mActivityTestRule.getActivity());
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();

        mStartSurfaceLayout = null;
        mTabListDelegate = null;
        mActivityTestRule.setActivity(activity);

        // A longer timeout is needed. Achieve that by using the CriteriaHelper.pollUiThread.
        CriteriaHelper.pollUiThread(
                () -> GarbageCollectionTestUtils.canBeGarbageCollected(activityRef));
    }

    /**
     * This test is to simulate closing all incognito tabs during the activity recreation stage.
     * This is a regression test for crbug.com/1044557. The test ensures all incognito tabs closed,
     * which leads to a TabModel switching event, before the first normal tab is restored.
     */
    @Test
    @MediumTest
    @DisableFeatures(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
    // clang-format off
    @DisabledTest(message = "crbug.com/1044557 This regression test fails deterministically with" +
            " the bot, but passes with local emulator. Disable the test for now.")
    public void
    closeAllIncognitoTabsBeforeRestoreCompleted() {
        // clang-format on
        prepareTabs(3, 1, "about:blank");

        assertTrue(mActivityTestRule.getActivity().getCurrentTabModel().isIncognito());

        // Need to wait for contentsState to be initialized for the tab to restore correctly.
        CriteriaHelper.pollUiThread(
                ()
                        -> TabStateExtractor.from(mActivityTestRule.getActivity().getActivityTab())
                                   .contentsState
                        != null,
                "Incognito tab contentsState is null");

        ChromeTabUtils.newTabFromMenu(InstrumentationRegistry.getInstrumentation(),
                mActivityTestRule.getActivity(), true, false);

        mActivityTestRule.loadUrl("about:blank");

        // Need to wait for contentsState to be initialized for the tab to restore correctly.
        CriteriaHelper.pollUiThread(
                ()
                        -> TabStateExtractor.from(mActivityTestRule.getActivity().getActivityTab())
                                   .contentsState
                        != null,
                "Incognito tab contentsState is null");

        PseudoTab.clearForTesting();

        AtomicBoolean tabModelRestoreCompleted = new AtomicBoolean(false);
        AtomicBoolean normalModelIsEmpty = new AtomicBoolean(true);

        mActivityTestRule.recreateActivity();
        ChromeTabbedActivity newActivity = mActivityTestRule.getActivity();

        new TabModelSelectorTabModelObserver(newActivity.getTabModelSelector()) {
            @Override
            public void restoreCompleted() {
                tabModelRestoreCompleted.set(true);
            }

            @Override
            public void willAddTab(Tab tab, int type) {
                if (tab.isIncognito() || !normalModelIsEmpty.get()) return;

                // Ensure normal tab model is active before its first tab restores.
                CriteriaHelper.pollUiThread(
                        ()
                                -> !newActivity.getCurrentTabModel().isIncognito(),
                        "Had not switched to Normal TabModel before the first normal Tab restored");
            }

            @Override
            public void didAddTab(Tab tab, int type, int creationState) {
                if (tab.isIncognito()) return;
                normalModelIsEmpty.set(false);
            }
        };

        CriteriaHelper.pollUiThread(()
                                            -> newActivity.didFinishNativeInitialization(),
                "New Activity has not finished initialized yet");
        CriteriaHelper.pollUiThread(()
                                            -> newActivity.getCurrentTabModel().isIncognito(),
                "New Activity current TabModel is not incognito");
        TestThreadUtils.runOnUiThreadBlocking(IncognitoTabHostUtils::closeAllIncognitoTabs);

        assertTrue("Deferred startup never completed", mActivityTestRule.waitForDeferredStartup());

        CriteriaHelper.pollUiThread(()
                                            -> tabModelRestoreCompleted.get(),
                "New Activity normal TabModel has never restored");

        assertFalse(newActivity.getCurrentTabModel().isIncognito());
        assertNotNull(newActivity.getTabModelSelector().getCurrentTab());
        // With the fix, StaticLayout guarantees to be shown. Otherwise, StartSurfaceLayout could be
        // shown, not guaranteed.
        assertTrue(newActivity.getLayoutManager().getActiveLayout() instanceof StaticLayout);
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GRID_LAYOUT_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testTabGroupNotFormDuringRestore() throws Exception {
        // clang-format on
        // Finish the activity and disable the ChromeFeatureList.TAB_GROUPS_ANDROID flag.
        finishActivity(mActivityTestRule.getActivity());
        CachedFeatureFlags.setForTesting(ChromeFeatureList.TAB_GROUPS_ANDROID, false);

        mActivityTestRule.startMainActivityOnBlankPage();
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        mActivityTestRule.loadUrl(mUrl);
        Tab parentTab = cta.getTabModelSelector().getCurrentTab();

        // Create a tab whose parent tab is parentTab.
        TabCreator tabCreator =
                TestThreadUtils.runOnUiThreadBlockingNoException(() -> cta.getTabCreator(false));
        LoadUrlParams loadUrlParams = new LoadUrlParams(mUrl);
        TestThreadUtils.runOnUiThreadBlocking(
                ()
                        -> tabCreator.createNewTab(
                                loadUrlParams, TabLaunchType.FROM_LONGPRESS_BACKGROUND, parentTab));
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 2);

        // Restart activity with tab group enabled, and the tabs should remain as single tabs.
        finishActivity(cta);
        CachedFeatureFlags.setForTesting(ChromeFeatureList.TAB_GROUPS_ANDROID, true);
        mActivityTestRule.startMainActivityOnBlankPage();
        final ChromeTabbedActivity ctaRestarted = mActivityTestRule.getActivity();
        assertTrue(ctaRestarted.getTabModelSelector()
                           .getTabModelFilterProvider()
                           .getCurrentTabModelFilter()
                           instanceof TabGroupModelFilter);
        enterTabSwitcher(ctaRestarted);
        verifyTabSwitcherCardCount(ctaRestarted, 3);
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void verifyTabGroupStateAfterReparenting() throws Exception {
        // clang-format on
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        assertTrue(cta.getTabModelSelector().getTabModelFilterProvider().getCurrentTabModelFilter()
                           instanceof TabGroupModelFilter);
        mActivityTestRule.loadUrl(mUrl);
        Tab parentTab = cta.getTabModelSelector().getCurrentTab();

        // Create a tab whose parent tab is parentTab.
        TabCreator tabCreator =
                TestThreadUtils.runOnUiThreadBlockingNoException(() -> cta.getTabCreator(false));
        LoadUrlParams loadUrlParams = new LoadUrlParams(mUrl);
        TestThreadUtils.runOnUiThreadBlocking(
                ()
                        -> tabCreator.createNewTab(
                                loadUrlParams, TabLaunchType.FROM_LONGPRESS_BACKGROUND, parentTab));
        Tab childTab = cta.getTabModelSelector().getCurrentModel().getTabAt(1);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 1);
        TabGroupModelFilter filter = (TabGroupModelFilter) cta.getTabModelSelector()
                                             .getTabModelFilterProvider()
                                             .getCurrentTabModelFilter();
        TestThreadUtils.runOnUiThreadBlocking(() -> filter.moveTabOutOfGroup(childTab.getId()));
        verifyTabSwitcherCardCount(cta, 2);

        TestThreadUtils.runOnUiThreadBlocking(
                () -> ChromeNightModeTestUtils.setUpNightModeForChromeActivity(true));
        final ChromeTabbedActivity ctaNightMode = ActivityTestUtils.waitForActivity(
                InstrumentationRegistry.getInstrumentation(), ChromeTabbedActivity.class);
        assertTrue(ColorUtils.inNightMode(ctaNightMode));
        CriteriaHelper.pollUiThread(ctaNightMode.getTabModelSelector()::isTabStateInitialized);
        enterTabSwitcher(ctaNightMode);
        verifyTabSwitcherCardCount(ctaNightMode, 2);
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID,
            ChromeFeatureList.TAB_GROUPS_CONTINUATION_ANDROID,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    public void testUndoClosure_AccessibilityMode() throws Exception {
        // clang-format on
        TestThreadUtils.runOnUiThreadBlocking(
                () -> ChromeAccessibilityUtil.get().setAccessibilityEnabledForTesting(true));
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        SnackbarManager snackbarManager = mActivityTestRule.getActivity().getSnackbarManager();
        createTabs(cta, false, 3);

        // When grid tab switcher is enabled for accessibility mode, tab closure should still show
        // undo snack bar.
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 3);
        assertNull(snackbarManager.getCurrentSnackbarForTesting());
        closeFirstTabInTabSwitcher();
        assertTrue(snackbarManager.getCurrentSnackbarForTesting().getController()
                           instanceof UndoBarController);
        verifyTabSwitcherCardCount(cta, 2);
        CriteriaHelper.pollInstrumentationThread(TabUiTestHelper::verifyUndoBarShowingAndClickUndo);
        verifyTabSwitcherCardCount(cta, 3);
    }

    @Test
    @MediumTest
    // clang-format off
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID, ChromeFeatureList.INSTANT_START,
            ChromeFeatureList.TAB_TO_GTS_ANIMATION + "<Study"})
    // TODO(crbug.com/1112557): Remove this test when critical tests in StartSurfaceLayoutTest are
    // running with InstantStart on.
    public void testSetup_WithInstantStart() throws Exception {
        // clang-format on
        ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 1);

        // Verify TabModelObserver is correctly setup by checking if tab switcher changes with tab
        // closure.
        closeFirstTabInTabSwitcher();
        verifyTabSwitcherCardCount(cta, 0);

        // Verify TabGroupModelFilter is correctly setup by checking if tab switcher changes with
        // tab grouping.
        createTabs(cta, false, 3);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 2);
        mergeAllNormalTabsToAGroup(cta);
        verifyTabSwitcherCardCount(cta, 1);
    }

    @Test
    @MediumTest
    @EnableFeatures({ChromeFeatureList.TAB_GROUPS_ANDROID})
    public void testUndoGroupClosureInTabSwitcher() {
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        SnackbarManager snackbarManager = mActivityTestRule.getActivity().getSnackbarManager();
        createTabs(cta, false, 2);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 2);
        // Create a tab group.
        mergeAllNormalTabsToAGroup(cta);
        verifyTabSwitcherCardCount(cta, 1);
        assertNull(snackbarManager.getCurrentSnackbarForTesting());

        // Verify close this tab group and undo in tab switcher.
        closeFirstTabInTabSwitcher();
        assertTrue(snackbarManager.getCurrentSnackbarForTesting().getController()
                           instanceof UndoBarController);
        verifyTabSwitcherCardCount(cta, 0);
        CriteriaHelper.pollInstrumentationThread(TabUiTestHelper::verifyUndoBarShowingAndClickUndo);
        verifyTabSwitcherCardCount(cta, 1);
    }

    private void enterTabGroupManualSelection(ChromeTabbedActivity cta) {
        MenuUtils.invokeCustomMenuActionSync(
                InstrumentationRegistry.getInstrumentation(), cta, R.id.menu_group_tabs);
    }

    /**
     * TODO(wychen): move some of the callers to {@link TabUiTestHelper#enterTabSwitcher}.
     */
    private void enterGTSWithThumbnailChecking() throws InterruptedException {
        Tab currentTab = mActivityTestRule.getActivity().getTabModelSelector().getCurrentTab();
        // Native tabs need to be invalidated first to trigger thumbnail taking, so skip them.
        boolean checkThumbnail = !currentTab.isNativePage();

        if (checkThumbnail) {
            mActivityTestRule.getActivity().getTabContentManager().removeTabThumbnail(
                    currentTab.getId());
        }

        int count = getCaptureCount();
        waitForCaptureRateControl();
        // TODO(wychen): use TabUiTestHelper.enterTabSwitcher() instead.
        //  Might increase flakiness though. See crbug.com/1024742.
        LayoutTestUtils.startShowingAndWaitForLayout(
                mActivityTestRule.getActivity().getLayoutManager(), LayoutType.TAB_SWITCHER, true);

        // Make sure the fading animation is done.
        int delta;
        if (UrlUtilities.isNTPUrl(mActivityTestRule.getActivity()
                                          .getCurrentWebContents()
                                          .getLastCommittedUrl())) {
            // NTP is not invalidated, so no new captures.
            delta = 0;
        } else {
            // The final capture at StartSurfaceLayout#finishedShowing time.
            delta = 1;
            if (ChromeFeatureList.isEnabled(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
                    && areAnimatorsEnabled()) {
                // The faster capturing without writing back to cache.
                delta += 1;
            }
        }
        checkCaptureCount(delta, count);
        TabUiTestHelper.verifyAllTabsHaveThumbnail(
                mActivityTestRule.getActivity().getCurrentTabModel());
    }

    /**
     * Like {@link TabUiTestHelper#enterTabSwitcher}, but make sure all tabs have thumbnail.
     */
    private void enterGTSWithThumbnailRetry() {
        enterTabSwitcher(mActivityTestRule.getActivity());
        try {
            TabUiTestHelper.verifyAllTabsHaveThumbnail(
                    mActivityTestRule.getActivity().getCurrentTabModel());
        } catch (AssertionError ae) {
            // If the last thumbnail is missing, try without animation.
            Espresso.pressBack();
            TestThreadUtils.runOnUiThreadBlocking(
                    ()
                            -> mActivityTestRule.getActivity().getLayoutManager().showLayout(
                                    LayoutType.TAB_SWITCHER, false));
            TabUiTestHelper.verifyAllTabsHaveThumbnail(
                    mActivityTestRule.getActivity().getCurrentTabModel());
        }
    }

    /**
     * If thumbnail checking is not needed, use {@link TabUiTestHelper#leaveTabSwitcher} instead.
     */
    private void leaveGTSAndVerifyThumbnailsAreReleased() {
        assertTrue(mActivityTestRule.getActivity().getLayoutManager().overviewVisible());

        StartSurface startSurface = mStartSurfaceLayout.getStartSurfaceForTesting();
        TestThreadUtils.runOnUiThreadBlocking(
                () -> { startSurface.getController().onBackPressed(); });
        // TODO(wychen): using default timeout or even converting to
        //  OverviewModeBehaviorWatcher shouldn't increase flakiness.
        CriteriaHelper.pollUiThread(
                ()
                        -> !mActivityTestRule.getActivity().getLayoutManager().overviewVisible(),
                "Overview not hidden yet", DEFAULT_MAX_TIME_TO_POLL * 10, DEFAULT_POLLING_INTERVAL);
        assertThumbnailsAreReleased();
    }

    private void checkFinalCaptureCount(boolean switchToAnotherTab, int initCount) {
        int expected;
        if (UrlUtilities.isNTPUrl(mActivityTestRule.getActivity()
                                          .getCurrentWebContents()
                                          .getLastCommittedUrl())) {
            expected = 0;
        } else {
            expected = mRepeat;
            if (ChromeFeatureList.isEnabled(ChromeFeatureList.TAB_TO_GTS_ANIMATION)
                    && areAnimatorsEnabled()) {
                expected += mRepeat;
            }
            if (switchToAnotherTab) {
                expected += mRepeat;
            }
        }
        checkCaptureCount(expected, initCount);
    }

    private void checkCaptureCount(int expectedDelta, int initCount) {
        // TODO(wychen): With animation, the 2nd capture might be skipped if the 1st takes too long.
        CriteriaHelper.pollUiThread(() -> {
            Criteria.checkThat(getCaptureCount() - initCount, Matchers.is(expectedDelta));
        });
    }

    private int getCaptureCount() {
        // TODO(crbug/1110961): Find a replacement for depending on Compositing.CopyFromSurfaceTime.
        return RecordHistogram.getHistogramTotalCountForTesting("Compositing.CopyFromSurfaceTime");
    }

    private void waitForCaptureRateControl() throws InterruptedException {
        // Needs to wait for at least |kCaptureMinRequestTimeMs| in order to capture another one.
        // TODO(wychen): find out why waiting is still needed after setting
        //               |kCaptureMinRequestTimeMs| to 0.
        Thread.sleep(2000);
    }

    private void assertThumbnailsAreReleased() {
        // Could not directly assert canAllBeGarbageCollected() because objects can be in Cleaner.
        CriteriaHelper.pollUiThread(() -> canAllBeGarbageCollected(mAllBitmaps));
    }

    private boolean canAllBeGarbageCollected(List<WeakReference<Bitmap>> bitmaps) {
        for (WeakReference<Bitmap> bitmap : bitmaps) {
            if (!GarbageCollectionTestUtils.canBeGarbageCollected(bitmap)) {
                return false;
            }
        }
        return true;
    }

    private void simulateAspectRatioChangedToPoint75() throws IOException {
        TabModel currentModel = mActivityTestRule.getActivity().getCurrentTabModel();
        for (int i = 0; i < currentModel.getCount(); i++) {
            Tab tab = currentModel.getTabAt(i);
            Bitmap bitmap = TabContentManager.getJpegForTab(tab.getId());
            bitmap = Bitmap.createScaledBitmap(
                    bitmap, bitmap.getWidth(), (int) (bitmap.getWidth() * 1.0 / 0.75), false);
            encodeJpeg(tab, bitmap);
        }
    }

    private void encodeJpeg(Tab tab, Bitmap bitmap) throws IOException {
        FileOutputStream outputStream =
                new FileOutputStream(TabContentManager.getTabThumbnailFileJpeg(tab.getId()));
        bitmap.compress(Bitmap.CompressFormat.JPEG, 50, outputStream);
        outputStream.close();
    }

    private void verifyAllThumbnailHasAspectRatio(double ratio) {
        TabModel currentModel = mActivityTestRule.getActivity().getCurrentTabModel();
        for (int i = 0; i < currentModel.getCount(); i++) {
            Tab tab = currentModel.getTabAt(i);
            Bitmap bitmap = TabContentManager.getJpegForTab(tab.getId());
            double bitmapRatio = bitmap.getWidth() * 1.0 / bitmap.getHeight();
            assertTrue("Actual ratio: " + bitmapRatio + "; Expected ratio: " + ratio,
                    Math.abs(bitmapRatio - ratio) <= TabContentManager.ASPECT_RATIO_PRECISION);
        }
    }

    private void verifyOnlyOneTabSuggestionMessageCardIsShowing() throws InterruptedException {
        String suggestionMessageTemplate = mActivityTestRule.getActivity().getString(
                org.chromium.chrome.tab_ui.R.string.tab_suggestion_close_stale_message);
        String suggestionMessage =
                String.format(Locale.getDefault(), suggestionMessageTemplate, "3");
        prepareTabs(3, 0, mUrl);
        CriteriaHelper.pollUiThread(TabSuggestionMessageService::isSuggestionAvailableForTesting);
        CriteriaHelper.pollUiThread(
                () -> Criteria.checkThat(getTabCountInCurrentTabModel(), Matchers.is(3)));

        enterGTSWithThumbnailChecking();
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        onView(allOf(withText(suggestionMessage), withParent(withId(R.id.tab_grid_message_item))))
                .check(matches(isDisplayed()));
        leaveGTSAndVerifyThumbnailsAreReleased();

        // With soft or hard clean up depends on the soft-cleanup-delay and cleanup-delay params.
        enterGTSWithThumbnailChecking();
        CriteriaHelper.pollUiThread(TabSwitcherCoordinator::hasAppendedMessagesForTesting);
        // This will fail with error "matched multiple views" when there is more than one suggestion
        // message card.
        onView(allOf(withText(suggestionMessage), withParent(withId(R.id.tab_grid_message_item))))
                .check(matches(isDisplayed()));
    }

    private Matcher<View> tabSwitcherViewMatcher() {
        return allOf(withParent(withId(R.id.compositor_view_holder)), withId(R.id.tab_list_view));
    }
}
