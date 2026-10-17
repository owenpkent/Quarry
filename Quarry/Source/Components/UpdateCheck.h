//
// Created by Damien Ronssin on 11/11/2024.
//
// The banner that announces a newer release. In the Windows Standalone it also offers to
// install it (UpdateInstaller does the work); everywhere else it only announces, with a link to
// the release page. UpdatePolicy decides what counts as newer and what may be downloaded, and
// docs/AUTO_UPDATE.md describes the whole flow.
//

#ifndef UPDATECHECK_H
#define UPDATECHECK_H

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>

#include "UpdatePolicy.h"

class UpdateCheck
    : public Component
    , public Timer
{
public:
    /** inIsStandalone is whether the app owns its process. A plugin runs inside a DAW that has
     *  the installed plugin loaded, so it can only announce a release, never replace it. */
    explicit UpdateCheck(bool inIsStandalone);

    ~UpdateCheck() override;

    void resized() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

    void checkForUpdate(bool inShowNotificationOnLatestVersion);

    /** Reads the arguments the installer relaunches Quarry with: --updated and --update-blocked. */
    void showLaunchNotice(const String& inCommandLine);

    /** Asked before an install quits the app. True means work would be lost (a take being
     *  recorded or transcribed), and the user is asked to confirm first. */
    std::function<bool()> isBusy;

    /** The "Check for updates at startup" setting. On unless the user turned it off. It lives in
     *  its own small file so a plugin and the standalone share it. */
    static bool isStartupCheckEnabled();

    static void setStartupCheckEnabled(bool inEnabled);

private:
    // What the banner is doing. Only a transient one hides itself; the others wait for the user
    // or for the install to finish.
    enum class Mode {
        hidden,
        transient,  // text, optionally the release link, hides after a few seconds
        actionable, // text with Install and Later
        busy        // text only, no way out until it ends
    };

    void _onReleaseFound(const quarry::update::ReleaseInfo& inRelease, bool inShowNotificationOnLatestVersion);

    void _setMode(Mode inMode, const String& inText, bool inShowLink = false);

    void _showTransient(const String& inText, bool inShowLink = false);

    void _hideNotification();

    // Install path, Windows Standalone only. Each step reports back through a SafePointer.
    void _startInstall();

    void _showProgress(double inFraction);

    void _confirmAndLaunch();

    void _launchAndQuit();

    // Message shown with Install and Later when something went wrong or the user can try again.
    void _showProblem(const String& inText);

    String _fitText(const String& inText, float inMaxWidth) const;

    float _buttonsWidth() const;

    const bool mIsStandalone;

    Mode mMode {Mode::hidden};
    String mText;
    double mProgress {-1.0};

    // Set when the installer relaunched us because a host had the plugin loaded: the message must
    // stay up, and the release fetched in the background only adds the Install button to it.
    bool mHoldNotice {false};

    quarry::update::ReleaseInfo mRelease;

    HyperlinkButton mUrlButton;
    TextButton mInstallButton {"Install"};
    TextButton mLaterButton {"Later"};

    Time mHideTime;

    // Set when the banner goes away so a download in flight stops at its next read.
    std::shared_ptr<std::atomic<bool>> mCancel {std::make_shared<std::atomic<bool>>(false)};

    static constexpr int mPadding = 5;
    static constexpr int mButtonWidth = 60;
    static constexpr int mButtonGap = 4;
    static constexpr int mLinkWidth = 65;

    static constexpr double mNotificationDurationSeconds = 10.0f;
    static constexpr double mTimeIncrementOnMouseOverSeconds = 3.0f;

    // Single source of truth for the repository the update check and the button point at.
    static constexpr const char* mGitHubRepo = "owenpkent/Quarry";

    const URL mLatestReleaseUrl {String("https://github.com/") + mGitHubRepo + "/releases/latest"};
};

#endif //UPDATECHECK_H
