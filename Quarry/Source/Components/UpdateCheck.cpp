//
// Created by Damien Ronssin on 11/11/2024.
//

#include "UpdateCheck.h"

#include "QuarryLookAndFeel.h"
#include "UIDefines.h"
#include "UpdateInstaller.h"

namespace
{
// One line of release notes is all the banner has room for.
constexpr int kBannerNotesChars = 90;

PropertiesFile::Options updateSettingsOptions()
{
    PropertiesFile::Options options;
    options.applicationName = "QuarryUpdate";
    options.filenameSuffix = "settings";
    options.folderName = "Quarry";
    options.osxLibrarySubFolder = "Application Support";
    return options;
}

constexpr const char* kStartupCheckKey = "checkForUpdatesAtStartup";
} // namespace

UpdateCheck::UpdateCheck(bool inIsStandalone)
    : mIsStandalone(inIsStandalone)
{
    mUrlButton.setButtonText("See update");
    mUrlButton.setURL(mLatestReleaseUrl);
    mUrlButton.setFont(UIDefines::LABEL_FONT(), false);
    mUrlButton.setJustificationType(Justification::centred);
    mUrlButton.setColour(HyperlinkButton::ColourIds::textColourId, TEXT_MAIN);
    addChildComponent(mUrlButton);

    quarry::lnf::setRole(mInstallButton, quarry::lnf::Role::primary);
    mInstallButton.setTitle("Install update");
    mInstallButton.onClick = [this] { _startInstall(); };
    addChildComponent(mInstallButton);

    quarry::lnf::setRole(mLaterButton, quarry::lnf::Role::quiet);
    mLaterButton.setTitle("Install later");
    mLaterButton.onClick = [this] { _hideNotification(); };
    addChildComponent(mLaterButton);
}

UpdateCheck::~UpdateCheck()
{
    mCancel->store(true);
}

bool UpdateCheck::isStartupCheckEnabled()
{
    PropertiesFile file(updateSettingsOptions());
    return file.getBoolValue(kStartupCheckKey, true);
}

void UpdateCheck::setStartupCheckEnabled(bool inEnabled)
{
    PropertiesFile file(updateSettingsOptions());
    file.setValue(kStartupCheckKey, inEnabled);
    file.saveIfNeeded();
}

float UpdateCheck::_buttonsWidth() const
{
    if (mInstallButton.isVisible() || mLaterButton.isVisible()) {
        const auto count = (mInstallButton.isVisible() ? 1 : 0) + (mLaterButton.isVisible() ? 1 : 0);
        return static_cast<float>(count * mButtonWidth + (count - 1) * mButtonGap + mPadding);
    }
    if (mUrlButton.isVisible())
        return static_cast<float>(mLinkWidth);
    return 0.0f;
}

void UpdateCheck::resized()
{
    const auto h = getHeight();
    auto right = getWidth() - mPadding;

    mUrlButton.setBounds(right - mLinkWidth, 0, mLinkWidth, h);

    // 2 px in from the pill's top and bottom so the button reads as sitting inside it.
    mLaterButton.setBounds(right - mButtonWidth, 2, mButtonWidth, h - 4);
    mInstallButton.setBounds(right - 2 * mButtonWidth - mButtonGap, 2, mButtonWidth, h - 4);
}

String UpdateCheck::_fitText(const String& inText, float inMaxWidth) const
{
    const auto font = UIDefines::LABEL_FONT();
    if (GlyphArrangement::getStringWidth(font, inText) <= inMaxWidth)
        return inText;

    auto text = inText;
    while (text.isNotEmpty() && GlyphArrangement::getStringWidth(font, text + "...") > inMaxWidth)
        text = text.dropLastCharacters(1);
    return text.trimEnd() + "...";
}

void UpdateCheck::paint(Graphics& g)
{
    g.setColour(PANEL_BG);
    g.setFont(UIDefines::LABEL_FONT());

    const auto buttons_width = _buttonsWidth();
    const auto text = _fitText(mText, static_cast<float>(getWidth()) - 2.0f * mPadding - buttons_width);

    AttributedString attributed_string(text);
    attributed_string.setFont(UIDefines::LABEL_FONT());
    attributed_string.setJustification(Justification::centred);
    // A TextLayout carries its own colour and ignores whatever the Graphics context
    // is set to, so this is the only place the text colour can be chosen.
    attributed_string.setColour(TEXT_MAIN);

    TextLayout text_layout;
    text_layout.createLayout(attributed_string, static_cast<float>(getWidth()), static_cast<float>(getHeight()));
    float text_width = text_layout.getWidth();
    float rectangle_width = text_width + 2 * mPadding + buttons_width;

    int rect_x_start = getWidth() - static_cast<int>(rectangle_width);

    g.fillRoundedRectangle(getLocalBounds().toFloat().withLeft(static_cast<float>(rect_x_start)), 4.0f);

    text_layout.draw(
        g,
        Rectangle<float>(static_cast<float>(rect_x_start + mPadding), 0, text_width, static_cast<float>(getHeight())));

    // A hairline along the bottom edge of the pill, filling left to right as the download goes.
    if (mProgress >= 0.0) {
        const auto bar = Rectangle<float>(static_cast<float>(rect_x_start) + 4.0f,
                                          static_cast<float>(getHeight()) - 2.0f,
                                          (rectangle_width - 8.0f) * static_cast<float>(jlimit(0.0, 1.0, mProgress)),
                                          1.5f);
        g.setColour(TEXT_MAIN.withAlpha(0.7f));
        g.fillRect(bar);
    }
}

void UpdateCheck::timerCallback()
{
    auto current_time = Time::getCurrentTime();
    auto mouse_over = isMouseOver(true);

    if (mouse_over) {
        mHideTime = std::max(current_time + RelativeTime::seconds(mTimeIncrementOnMouseOverSeconds), mHideTime);
    }

    if (mMode == Mode::transient && current_time >= mHideTime) {
        _hideNotification();
    }
}

void UpdateCheck::checkForUpdate(bool inShowNotificationOnLatestVersion)
{
    // Call async because of issue on Windows with spinning cursor.
    // The request outlives the editor if it is closed meanwhile, so nothing below may touch this
    // directly: the thread works from a copy of what it needs and reaches back through a SafePointer.
    Component::SafePointer<UpdateCheck> safe_this(this);

    MessageManager::callAsync([safe_this, inShowNotificationOnLatestVersion] {
        if (safe_this == nullptr) {
            return;
        }
        const String repo = safe_this->mGitHubRepo;

        Thread::launch([safe_this, repo, inShowNotificationOnLatestVersion] {
            // A manual check says so when it could not reach the answer; the silent one stays silent.
            const auto fail = [&safe_this, inShowNotificationOnLatestVersion] {
                if (!inShowNotificationOnLatestVersion) {
                    return;
                }
                MessageManager::callAsync([safe_this] {
                    if (safe_this != nullptr) {
                        safe_this->_showTransient("Could not check for updates");
                    }
                });
            };

            const URL url(String("https://api.github.com/repos/") + repo + "/releases/latest");

            int status_code = 0;
            const auto stream = url.createInputStream(URL::InputStreamOptions(URL::ParameterHandling::inAddress)
                                                          .withConnectionTimeoutMs(5000)
                                                          .withStatusCode(&status_code));

            // An error body (404, rate limit) still parses as JSON, so only a 200 is worth looking at.
            if (stream == nullptr || status_code != 200) {
                fail();
                return;
            }

            // A release description is a few KB; refuse anything absurd rather than buffer it.
            constexpr int64 kMaxResponseBytes = 1024 * 1024;
            const auto total_length = stream->getTotalLength();
            if (total_length > kMaxResponseBytes) {
                fail();
                return;
            }

            MemoryOutputStream body;
            body.writeFromInputStream(*stream, kMaxResponseBytes + 1);
            if (body.getDataSize() > static_cast<size_t>(kMaxResponseBytes)) {
                fail();
                return;
            }
            const auto result = body.toUTF8();

            if (result.isEmpty()) {
                fail();
                return;
            }

            const auto release = quarry::update::parseRelease(JSON::parse(result));

            if (!release.has_value()) {
                fail();
                return;
            }

            MessageManager::callAsync([release = *release, inShowNotificationOnLatestVersion, safe_this] {
                if (safe_this == nullptr) {
                    return;
                }
                safe_this->_onReleaseFound(release, inShowNotificationOnLatestVersion);
            });
        });
    });
}

void UpdateCheck::_onReleaseFound(const quarry::update::ReleaseInfo& inRelease,
                                  bool inShowNotificationOnLatestVersion)
{
    // Uncomment this line to test the new version available notification
    // const auto current_version = String("0.0.1");
    const auto current_version = String(JucePlugin_VersionString).trim();

    // An older or equal tag is not an update, whatever else differs.
    if (!quarry::update::isNewer(inRelease.tag, current_version)) {
        if (inShowNotificationOnLatestVersion) {
            _showTransient("You are on the latest version of Quarry!");
        }
        return;
    }

    // Don't replace a download or install already under way with the same news.
    if (mMode == Mode::busy) {
        return;
    }

    mRelease = inRelease;

#if JUCE_WINDOWS
    const bool can_install = mIsStandalone && inRelease.hasInstallableAssets();
#else
    const bool can_install = false;
#endif

    if (can_install) {
        auto text = "Quarry " + inRelease.version + " is available";
        const auto notes = quarry::update::shortenNotes(inRelease.notes, kBannerNotesChars);
        if (notes.isNotEmpty()) {
            text << ": " << notes;
        }

        // The relaunch after a blocked install keeps its own message and only gains the button.
        if (mHoldNotice) {
            text = mText;
            mHoldNotice = false;
        }
        _setMode(Mode::actionable, text);
    } else if (!mIsStandalone) {
        _showTransient("Quarry " + inRelease.version + " is available. Close your DAW, then run the installer.", true);
    } else {
        _showTransient("Quarry " + inRelease.version + " is available:", true);
    }
}

void UpdateCheck::showLaunchNotice(const String& inCommandLine)
{
    const auto arguments = StringArray::fromTokens(inCommandLine, " ", "\"");

    if (arguments.contains("--updated")) {
        _showTransient("Updated to Quarry " + String(JucePlugin_VersionString).trim());
    } else if (arguments.contains("--update-blocked")) {
        _showTransient("Update not installed: a DAW had the Quarry plugin loaded. Close it and choose Install again.");
        mHideTime = Time::getCurrentTime() + RelativeTime::seconds(30.0);
        mHoldNotice = true;

        // The pinned installer died with the old process, so Install has to be offered afresh.
        checkForUpdate(false);
    }
}

void UpdateCheck::_setMode(Mode inMode, const String& inText, bool inShowLink)
{
    mMode = inMode;
    mText = inText;
    mProgress = -1.0;

#if JUCE_WINDOWS
    const bool can_install = mIsStandalone && mRelease.hasInstallableAssets();
#else
    const bool can_install = false;
#endif

    mInstallButton.setVisible(inMode == Mode::actionable && can_install);
    mLaterButton.setVisible(inMode == Mode::actionable && can_install);
    mUrlButton.setVisible(inShowLink);

    setVisible(inMode != Mode::hidden);
    repaint();

    if (inMode == Mode::hidden) {
        stopTimer();
    } else {
        startTimerHz(5);
    }
}

void UpdateCheck::_showTransient(const String& inText, bool inShowLink)
{
    // Never talk over a download in progress.
    if (mMode == Mode::busy) {
        return;
    }
    mHideTime = Time::getCurrentTime() + RelativeTime::seconds(mNotificationDurationSeconds);
    _setMode(Mode::transient, inText, inShowLink);
}

void UpdateCheck::_hideNotification()
{
    mHoldNotice = false;
    _setMode(Mode::hidden, {});
}

void UpdateCheck::_showProblem(const String& inText)
{
    _setMode(Mode::actionable, inText);
}

void UpdateCheck::_showProgress(double inFraction)
{
    if (mMode != Mode::busy) {
        return;
    }
    mText = "Downloading Quarry " + mRelease.version + ": " + String(roundToInt(inFraction * 100.0)) + "%";
    mProgress = inFraction;
    repaint();
}

void UpdateCheck::_startInstall()
{
#if JUCE_WINDOWS
    if (!mIsStandalone || !mRelease.hasInstallableAssets() || mMode == Mode::busy) {
        return;
    }

    mCancel->store(false);
    _setMode(Mode::busy, "Downloading Quarry " + mRelease.version + ": 0%");
    mProgress = 0.0;

    Component::SafePointer<UpdateCheck> safe_this(this);
    const auto release = mRelease;
    const auto cancel = mCancel;

    Thread::launch([safe_this, release, cancel] {
        // Throttled to whole percents: a 100 MB download reads thousands of chunks.
        auto last_percent = std::make_shared<int>(-1);
        const auto progress = [safe_this, last_percent](double inFraction) {
            const auto percent = static_cast<int>(inFraction * 100.0);
            if (percent == *last_percent) {
                return;
            }
            *last_percent = percent;
            MessageManager::callAsync([safe_this, inFraction] {
                if (safe_this != nullptr) {
                    safe_this->_showProgress(inFraction);
                }
            });
        };

        const auto prepared = quarry::update::UpdateInstaller::prepare(release, progress, *cancel);

        if (cancel->load()) {
            return;
        }

        if (!prepared.ok) {
            MessageManager::callAsync([safe_this, message = prepared.message] {
                if (safe_this != nullptr) {
                    safe_this->_showProblem(message);
                }
            });
            return;
        }

        // A host holding the plugin would make the installer give up and relaunch us, so ask now.
        const auto host = quarry::update::UpdateInstaller::findHostHoldingPlugin();

        MessageManager::callAsync([safe_this, host] {
            if (safe_this == nullptr) {
                return;
            }
            if (host.isNotEmpty()) {
                safe_this->_showProblem(quarry::update::hostBlockedMessage(host));
            } else {
                safe_this->_confirmAndLaunch();
            }
        });
    });
#endif
}

void UpdateCheck::_confirmAndLaunch()
{
    // Installing quits the app the way closing the window does. Quarry keeps no project file to
    // save, so the only thing a quit can lose is a take that is still being recorded or processed.
    if (isBusy != nullptr && isBusy()) {
        Component::SafePointer<UpdateCheck> safe_this(this);

        NativeMessageBox::showOkCancelBox(
            MessageBoxIconType::WarningIcon,
            "Install update",
            "Quarry is still recording or transcribing. Installing quits Quarry and this take will be lost.",
            this,
            ModalCallbackFunction::create([safe_this](int inResult) {
                if (safe_this == nullptr) {
                    return;
                }
                if (inResult == 1) {
                    safe_this->_launchAndQuit();
                } else {
                    safe_this->_showProblem("Update not installed");
                }
            }));
        return;
    }

    _launchAndQuit();
}

void UpdateCheck::_launchAndQuit()
{
#if JUCE_WINDOWS
    const auto launched = quarry::update::UpdateInstaller::launch();

    if (!launched.ok) {
        _showProblem(launched.message);
        return;
    }

    _setMode(Mode::busy, "Installing. Quarry will restart.");

    // The installer waits for this process to exit, so quit the way the window's close button
    // does, after a beat so the line above is on screen first.
    Timer::callAfterDelay(500, [] {
        if (auto* app = JUCEApplicationBase::getInstance()) {
            app->systemRequestedQuit();
        }
    });
#endif
}
