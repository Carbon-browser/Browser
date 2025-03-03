// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firstrun;

import android.content.Context;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.method.LinkMovementMethod;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityEvent;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;
import androidx.fragment.app.Fragment;

import org.chromium.base.metrics.RecordHistogram;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.privacy.settings.PrivacyPreferencesManagerImpl;
import org.chromium.chrome.browser.signin.services.FREMobileIdentityConsistencyFieldTrial;
import org.chromium.chrome.browser.ui.signin.fre.FreUMADialogCoordinator;
import org.chromium.components.version_info.VersionInfo;
import org.chromium.ui.modaldialog.ModalDialogManagerHolder;
import org.chromium.ui.text.NoUnderlineClickableSpan;
import org.chromium.ui.text.SpanApplier;
import org.chromium.ui.text.SpanApplier.SpanInfo;

import java.util.LinkedList;
import java.util.List;

import android.content.res.Configuration;
import android.media.MediaPlayer;
import android.view.SurfaceView;
import android.view.SurfaceHolder;
import android.os.Handler;
import android.net.Uri;
import org.chromium.chrome.browser.device.DeviceClassManager;
import android.widget.ImageView;
import android.graphics.Color;
import android.graphics.Shader;
import android.graphics.LinearGradient;

/**
 * The First Run Experience fragment that allows the user to accept Terms of Service ("ToS") and
 * Privacy Notice, and to opt-in to the usage statistics and crash reports collection ("UMA",
 * User Metrics Analysis) as defined in the Chrome Privacy Notice.
 */
public class ToSAndUMAFirstRunFragment
        extends Fragment implements FirstRunFragment, FreUMADialogCoordinator.Listener, SurfaceHolder.Callback {
    /** Alerts about some methods once ToSAndUMAFirstRunFragment executes them. */
    public interface Observer {
        /** See {@link #onNativeInitialized}. */
        public void onNativeInitialized();
        public void onPolicyServiceInitialized();
        public void onHideLoadingUIComplete();
    }

    private static boolean sShowUmaCheckBoxForTesting;

    @Nullable
    private static ToSAndUMAFirstRunFragment.Observer sObserver;

    private boolean mNativeInitialized;
    private boolean mPolicyServiceInitialized;
    private boolean mTosButtonClicked;
    // TODO(https://crbug.com/1274145): Rename mAllowCrashUpload field.
    private boolean mAllowCrashUpload;

    private Button mAcceptButton;
    private CheckBox mSendReportCheckBox;
    private TextView mTosAndPrivacy;
    private View mProgressSpinner;

    private long mTosAcceptedTime;

    private ImageView mLogoImageView;
    private SurfaceView surfaceView;
    private MediaPlayer player;
    private Handler mHandler;
    private int mCurrentVideoPosition;
    private View noAnimSpacerTop;
    private View noAnimSpacerBottom;

    @Override
    public View onCreateView(
            LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fre_tosanduma, container, false);
    }

    @Override
    public void onAttach(@NonNull Context context) {
        super.onAttach(context);
        getPageDelegate().getPolicyLoadListener().onAvailable(this::onPolicyServiceInitialized);
    }

    @Override
    public void onViewCreated(View view, Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        mLogoImageView = view.findViewById(R.id.image);

        noAnimSpacerTop = view.findViewById(R.id.fre_spacer_top);
        noAnimSpacerBottom = view.findViewById(R.id.fre_spacer_bottom);

        mProgressSpinner = view.findViewById(R.id.progress_spinner);
        mProgressSpinner.setVisibility(View.GONE);
        mAcceptButton = (Button) view.findViewById(R.id.terms_accept);
        mSendReportCheckBox = (CheckBox) view.findViewById(R.id.send_report_checkbox);
        mTosAndPrivacy = (TextView) view.findViewById(R.id.tos_and_privacy);

        // set start button gradient
        Shader textShader = new LinearGradient(0, 0, 175, 0,
            new int[]{Color.parseColor("#FF320A"),Color.parseColor("#FF9133")},
           null, Shader.TileMode.REPEAT);
        mAcceptButton.getPaint().setShader(textShader);

        // Register event listeners.
        mAcceptButton.setOnClickListener((v) -> onTosButtonClicked());
        mSendReportCheckBox.setOnCheckedChangeListener(
                ((compoundButton, isChecked) -> mAllowCrashUpload = isChecked));

        // Make TextView links clickable.
        mTosAndPrivacy.setMovementMethod(LinkMovementMethod.getInstance());

        // Intro animation
        surfaceView = (SurfaceView) view.findViewById(R.id.fre_surface_view);
        surfaceView.getHolder().addCallback(this);
        // if (DeviceClassManager.enableAnimations()) {
        //     setupMediaPlayer();
        // } else {
            setAnimationDisabled();
        // }
        getActivity().getWindow().setNavigationBarColor(Color.parseColor("#1E1E1E"));

        updateView();

        // If this page should be skipped, it can be one of the following cases:
        //   1. Native hasn't been initialized yet and this page will be skipped once that happens.
        //   2. The user has moved back to this page after advancing past it. In this case, this
        //      may not even be the same object as before, as the fragment may have been re-created.
        //
        // In case 1, hide all the elements except for Chrome logo and the spinner until native gets
        // initialized at which point the activity will skip the page.
        // We distinguish case 1 from case 2 by the value of |mNativeInitialized|, as that is set
        // via onAttachFragment() from FirstRunActivity - which is before this onViewCreated().
        if (isWaitingForNativeAndPolicyInit() && FirstRunStatus.shouldSkipWelcomePage()) {
            setSpinnerVisible(true);
        }
    }

    @Override
    public void setInitialA11yFocus() {
        // Ignore calls before view is created.
    }

    @Override
    public void setUserVisibleHint(boolean isVisibleToUser) {
        super.setUserVisibleHint(isVisibleToUser);

        if (!isVisibleToUser) {
            // Restore original enabled & visibility states, in case the user returns to the page.
            setSpinnerVisible(false);
        } else {
            // On certain versions of Android, the checkbox will appear unchecked upon revisiting
            // the page.  Force it to the end state of the drawable animation as a work around.
            // crbug.com/666258
            mSendReportCheckBox.jumpDrawablesToCurrentState();
        }
    }

    @Override
    public void onNativeInitialized() {
        assert !mNativeInitialized;

        mNativeInitialized = true;
        tryMarkTermsAccepted(false);

        if (mPolicyServiceInitialized) {
            onNativeAndPolicyServiceInitialized();
        }

        if (sObserver != null) {
            sObserver.onNativeInitialized();
        }
    }

    @Override
    public void reset() {
        // We cannot pass the welcome page when native or policy is not initialized. When this page
        // is revisited, this means this page is persist and we should re-show the ToS And UMA.
        assert !isWaitingForNativeAndPolicyInit();

        setSpinnerVisible(false);
        mSendReportCheckBox.setChecked(mAllowCrashUpload);
    }

    /** Implements {@link FreUMADialogCoordinator.Listener} */
    @Override
    public void onAllowCrashUploadChecked(boolean allowCrashUpload) {
        mAllowCrashUpload = allowCrashUpload;
    }

    private void updateView() {
        // Avoid early calls.
        if (getPageDelegate() == null) {
            return;
        }

        final boolean umaDialogMayBeShown = false;
                //FREMobileIdentityConsistencyFieldTrial.shouldShowOldFreWithUmaDialog();
        final boolean hasChildAccount = false;//getPageDelegate().getProperties().getBoolean(
                //SyncConsentFirstRunFragment.IS_CHILD_ACCOUNT, false);
        final boolean isMetricsReportingDisabledByPolicy = !isWaitingForNativeAndPolicyInit()
                && !PrivacyPreferencesManagerImpl.getInstance()
                            .isUsageAndCrashReportingPermittedByPolicy();

        updateTosText(umaDialogMayBeShown, hasChildAccount, isMetricsReportingDisabledByPolicy);

        updateReportCheckbox(umaDialogMayBeShown, isMetricsReportingDisabledByPolicy);
    }

    private SpanInfo buildTermsOfServiceLink() {
        NoUnderlineClickableSpan clickableGoogleTermsSpan =
                new NoUnderlineClickableSpan(getContext(), (view1) -> {
                    if (!isAdded()) return;
                    getPageDelegate().showInfoPage(R.string.google_terms_of_service_url);
                });
        return new SpanInfo("<TOS_LINK>", "</TOS_LINK>", clickableGoogleTermsSpan);
    }

    private SpanInfo buildAdditionalTermsOfServiceLink() {
        NoUnderlineClickableSpan clickableChromeAdditionalTermsSpan =
                new NoUnderlineClickableSpan(getContext(), (view1) -> {
                    if (!isAdded()) return;
                    getPageDelegate().showInfoPage(R.string.chrome_additional_terms_of_service_url);
                });
        return new SpanInfo("<ATOS_LINK>", "</ATOS_LINK>", clickableChromeAdditionalTermsSpan);
    }

    private SpanInfo buildPrivacyPolicyLink() {
        NoUnderlineClickableSpan clickableFamilyLinkPrivacySpan =
                new NoUnderlineClickableSpan(getContext(), (view1) -> {
                    if (!isAdded()) return;
                    getPageDelegate().showInfoPage(R.string.google_privacy_policy_url);
                });

        return new SpanInfo("<PRIVACY_LINK>", "</PRIVACY_LINK>", clickableFamilyLinkPrivacySpan);
    }

    private SpanInfo buildMetricsAndCrashReportingLink() {
        NoUnderlineClickableSpan clickableUMADialogSpan =
                new NoUnderlineClickableSpan(getContext(), (view1) -> openUmaDialog());
        return new SpanInfo("<UMA_LINK>", "</UMA_LINK>", clickableUMADialogSpan);
    }

    private void updateTosText(boolean umaDialogMayBeShown, boolean hasChildAccount,
            boolean isMetricsReportingDisabledByPolicy) {
        List<SpanInfo> spans = new LinkedList<SpanInfo>();

        // Description always has a Terms of Service link.
        spans.add(buildTermsOfServiceLink());

        // Additional terms of service link.
        if (!umaDialogMayBeShown) {
            spans.add(buildAdditionalTermsOfServiceLink());
        }

        // Privacy policy link.
        if (hasChildAccount) {
            spans.add(buildPrivacyPolicyLink());
        }

        // Metrics and crash reporting link.
        if (umaDialogMayBeShown && !isMetricsReportingDisabledByPolicy) {
            spans.add(buildMetricsAndCrashReportingLink());
        }

        String tosString;
        if (umaDialogMayBeShown) {
            tosString =
                    getString(hasChildAccount ? R.string.signin_fre_footer_tos_with_supervised_user
                                              : R.string.signin_fre_footer_tos);

            if (!isMetricsReportingDisabledByPolicy) {
                tosString += "\n" + getString(R.string.signin_fre_footer_metrics_reporting);
            }
        } else {
            tosString = getString(//hasChildAccount ? R.string.fre_tos_and_privacy_child_account :
                                                   R.string.fre_tos_short);
        }

        mTosAndPrivacy.setText(SpanApplier.applySpans(tosString, spans.toArray(new SpanInfo[0])));
    }

    private void updateReportCheckbox(
            boolean umaDialogMayBeShown, boolean isMetricsReportingDisabledByPolicy) {
        mAllowCrashUpload = getUmaCheckBoxInitialState();
        mSendReportCheckBox.setChecked(mAllowCrashUpload);

        if (!canShowUmaCheckBox()) {
            if (!umaDialogMayBeShown) {
                mAllowCrashUpload = (sShowUmaCheckBoxForTesting || VersionInfo.isOfficialBuild())
                        && !isMetricsReportingDisabledByPolicy;
            }
            mSendReportCheckBox.setVisibility(View.GONE);
        }
    }

    private void openUmaDialog() {
        new FreUMADialogCoordinator(requireContext(),
                ((ModalDialogManagerHolder) getActivity()).getModalDialogManager(), this,
                mAllowCrashUpload);
    }

    private void onPolicyServiceInitialized(boolean onDevicePolicyFound) {
        assert !mPolicyServiceInitialized;

        mPolicyServiceInitialized = true;
        tryMarkTermsAccepted(false);

        if (mNativeInitialized) {
            onNativeAndPolicyServiceInitialized();
        }

        if (sObserver != null) {
            sObserver.onPolicyServiceInitialized();
        }
    }

    private void onNativeAndPolicyServiceInitialized() {
        // Once we have native & policies, Check whether metrics reporting are permitted by policy
        // and update interface accordingly.
        updateView();
    }

    private void onTosButtonClicked() {
        mTosButtonClicked = true;
        mTosAcceptedTime = SystemClock.elapsedRealtime();
        tryMarkTermsAccepted(true);
    }

    /**
     * This should be called Tos button is clicked for a fresh new FRE, or when native and policies
     * are initialized if Tos has ever been accepted.
     *
     * @param fromButtonClicked Whether called from {@link #onTosButtonClicked()}.
     */
    private void tryMarkTermsAccepted(boolean fromButtonClicked) {
        if (!mTosButtonClicked || isWaitingForNativeAndPolicyInit()) {
            if (fromButtonClicked) setSpinnerVisible(true);
            return;
        }

        // In cases where the attempt is triggered other than button click, the ToS should have been
        // accepted by the user already.
        if (!fromButtonClicked) {
            RecordHistogram.recordTimesHistogram("MobileFre.TosFragment.SpinnerVisibleDuration",
                    SystemClock.elapsedRealtime() - mTosAcceptedTime);
        }
        boolean allowCrashUpload = canShowUmaCheckBox() && mSendReportCheckBox.isChecked();
        getPageDelegate().acceptTermsOfService(false);
        getPageDelegate().advanceToNextPage();
    }

    private void setSpinnerVisible(boolean spinnerVisible) {
        // When the progress spinner is visible, we hide the other UI elements so that
        // the user can't interact with them.
        boolean otherElementVisible = !spinnerVisible;

        setTosAndUmaVisible(otherElementVisible);
        mProgressSpinner.setVisibility(spinnerVisible ? View.VISIBLE : View.GONE);
    }

    private boolean isWaitingForNativeAndPolicyInit() {
        return !mNativeInitialized || !mPolicyServiceInitialized;
    }

    private boolean getUmaCheckBoxInitialState() {
        // Metrics and crash reporting could not be permitted by policy.
        if (!isWaitingForNativeAndPolicyInit()
                && !PrivacyPreferencesManagerImpl.getInstance()
                            .isUsageAndCrashReportingPermittedByPolicy()) {
            return false;
        }

        // A user could start FRE and accept terms of service, then close the browser and start
        // again. In this case we rely on whatever state the user has already set.
        if (FirstRunUtils.didAcceptTermsOfService()) {
            return PrivacyPreferencesManagerImpl.getInstance()
                    .isUsageAndCrashReportingPermittedByUser();
        }

        return FirstRunActivity.DEFAULT_METRICS_AND_CRASH_REPORTING;
    }

    // Exposed methods for ToSAndUMACCTFirstRunFragment

    protected void setTosAndUmaVisible(boolean isVisible) {
        int visibility = isVisible ? View.VISIBLE : View.GONE;

        mAcceptButton.setVisibility(visibility);
        mTosAndPrivacy.setVisibility(visibility);
        // Avoid updating visibility if the UMA check box can't be shown right now.
        if (canShowUmaCheckBox()) {
            mSendReportCheckBox.setVisibility(visibility);
        }
    }

    protected View getToSAndPrivacyText() {
        return mTosAndPrivacy;
    }

    protected void onHideLoadingUIComplete() {
        if (sObserver != null) {
            sObserver.onHideLoadingUIComplete();
        }
    }

    /**
     * @return Whether the check box for Uma metrics can be shown. It should be used in conjunction
     *         with whether other non-spinner elements can generally be shown.
     */
    protected boolean canShowUmaCheckBox() {
        // return !FREMobileIdentityConsistencyFieldTrial.shouldShowOldFreWithUmaDialog()
        //         && (sShowUmaCheckBoxForTesting || VersionInfo.isOfficialBuild())
        //         && (isWaitingForNativeAndPolicyInit()
        //                 || PrivacyPreferencesManagerImpl.getInstance()
        //                            .isUsageAndCrashReportingPermittedByPolicy());
        return false;
    }

    @VisibleForTesting
    public static void setShowUmaCheckBoxForTesting(boolean showForTesting) {
        sShowUmaCheckBoxForTesting = showForTesting;
    }

    @VisibleForTesting
    public static void setObserverForTesting(ToSAndUMAFirstRunFragment.Observer observer) {
        assert sObserver == null;
        sObserver = observer;
    }

    private void setAnimationDisabled() {
        mAcceptButton.setVisibility(View.VISIBLE);
        mTosAndPrivacy.setVisibility(View.VISIBLE);
        mLogoImageView.setVisibility(View.VISIBLE);
        noAnimSpacerTop.setVisibility(View.VISIBLE);
        noAnimSpacerBottom.setVisibility(View.VISIBLE);
        updateParams(false);
    }
    private java.lang.Runnable updateSurfaceViewVisibility = new java.lang.Runnable() {
        @Override
        public void run() {
            if (player != null && player.isPlaying() && player.getCurrentPosition() > 1) {
                surfaceView.setAlpha(1);
            } else {
                mHandler.postDelayed(this, 250);
            }
        }
    };
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // orientation change
        updateParams(player != null);
    }
    private void updateParams(boolean shouldUpdateSurfaceView) {
        int screenHeight = getActivity().getWindowManager().getDefaultDisplay().getHeight();
        if (shouldUpdateSurfaceView && player != null) {
           // Adjust the size of the video
           // so it fits on the screen
           int videoWidth = player.getVideoWidth();
           int videoHeight = player.getVideoHeight();
           float videoProportion = (float) videoWidth / (float) videoHeight;
           int screenWidth = getActivity().getWindowManager().getDefaultDisplay().getWidth();
           float screenProportion = (float) screenWidth / (float) screenHeight;
           ViewGroup.LayoutParams lp = surfaceView.getLayoutParams();
           if (videoProportion > screenProportion) {
               lp.width = screenWidth;
               lp.height = (int) ((float) screenWidth / videoProportion);
           } else {
               lp.width = (int) (videoProportion * (float) screenHeight);
               lp.height = screenHeight;
           }
           surfaceView.setLayoutParams(lp);
        }
     }
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (player == null) {
            // if (DeviceClassManager.enableAnimations()) {
            //     setupMediaPlayer();
            // } else {
                setAnimationDisabled();
            // }
        }
        try {
            player.setDisplay(holder);
            holder.setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
            holder.setSizeFromLayout();
        } catch (Exception ignore) { }
    }
    @Override
    public void surfaceChanged(SurfaceHolder surfaceHolder, int i, int i1, int i2) {
    }
    @Override
    public void surfaceDestroyed(SurfaceHolder surfaceHolder) {
        if (player != null) {
            mCurrentVideoPosition = player.getCurrentPosition();
            player.release();
            player = null;
        }
    }
    private void setupMediaPlayer() {
        player = new MediaPlayer();
        // player = MediaPlayer.create(getContext(), R.raw.sfb_anim_intro);
        // Uri mUri = Uri.parse("android.resource://" + getContext().getPackageName() + "/" + R.raw.sfb_anim_intro);//"file:///android_asset/sfb_anim_intro.mp4");
        // String uri = mUri.getPath();
        // player.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
        //     @Override
        //     public void onPrepared(MediaPlayer mp) {
        //         updateParams(true);
        //         if (!player.isPlaying()) {
        //             player.start();
        //             if (mHandler == null) mHandler = new Handler();
        //             mHandler.postDelayed(updateSurfaceViewVisibility, 0);
        //         }
        //         if (mCurrentVideoPosition != 0) {
        //             player.seekTo(mCurrentVideoPosition);
        //             player.start();
        //         }
        //     }
        // });
        // player.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
        //     @Override
        //     public void onCompletion(MediaPlayer mediaPlayer) {
        //         mAcceptButton.setVisibility(View.VISIBLE);
        //         mTosAndPrivacy.setVisibility(View.VISIBLE);
        //     }
        // });
        // try {
        //     player.setDataSource(uri);
        // } catch (Exception e) { }
    }
}
