//
// Created by Damien Ronssin on 11/11/2024.
//

#include "UpdateCheck.h"

#include "UIDefines.h"

UpdateCheck::UpdateCheck()
{
    mUrlButton.setButtonText("See update");
    mUrlButton.setURL(mLatestReleaseUrl);
    mUrlButton.setFont(UIDefines::LABEL_FONT(), false);
    mUrlButton.setJustificationType(Justification::centred);
    mUrlButton.setColour(HyperlinkButton::ColourIds::textColourId, TEXT_MAIN);
    addAndMakeVisible(mUrlButton);
}

void UpdateCheck::resized()
{
    mUrlButton.setBounds(getWidth() - 65 - mPadding, 0, 65, getHeight());
}

void UpdateCheck::paint(Graphics& g)
{
    g.setColour(PANEL_BG);
    g.setFont(UIDefines::LABEL_FONT());

    String text;
    if (mUpdateAvailable) {
        text = "A new version of Quarry is available:";
    } else {
        text = "You are on the latest version of Quarry!";
    }

    AttributedString attributed_string(text);
    attributed_string.setFont(UIDefines::LABEL_FONT());
    attributed_string.setJustification(Justification::centred);
    // A TextLayout carries its own colour and ignores whatever the Graphics context
    // is set to, so this is the only place the text colour can be chosen.
    attributed_string.setColour(TEXT_MAIN);

    TextLayout text_layout;
    text_layout.createLayout(attributed_string, static_cast<float>(getWidth()), static_cast<float>(getHeight()));
    float text_width = text_layout.getWidth();
    float rectangle_width = text_width + 2 * mPadding;

    if (mUpdateAvailable) {
        rectangle_width += static_cast<float>(mUrlButton.getWidth());
    }

    int rect_x_start = getWidth() - static_cast<int>(rectangle_width);

    g.fillRoundedRectangle(getLocalBounds().toFloat().withLeft(static_cast<float>(rect_x_start)), 4.0f);

    text_layout.draw(
        g,
        Rectangle<float>(static_cast<float>(rect_x_start + mPadding), 0, text_width, static_cast<float>(getHeight())));
}

void UpdateCheck::timerCallback()
{
    auto current_time = Time::getCurrentTime();
    auto mouse_over = isMouseOver(true);

    if (mouse_over) {
        mHideTime = std::max(current_time + RelativeTime::seconds(mTimeIncrementOnMouseOverSeconds), mHideTime);
    }

    if (current_time >= mHideTime) {
        _hideNotification();
    }
}

void UpdateCheck::checkForUpdate(bool inShowNotificationOnLatestVersion)
{
    // Call async because of issue on Windows with spinning cursor.
    MessageManager::callAsync([this, inShowNotificationOnLatestVersion] {
        Thread::launch([this, inShowNotificationOnLatestVersion] {
            const URL url(String("https://api.github.com/repos/") + mGitHubRepo + "/releases/latest");

            int status_code = 0;
            const auto stream = url.createInputStream(URL::InputStreamOptions(URL::ParameterHandling::inAddress)
                                                          .withConnectionTimeoutMs(5000)
                                                          .withStatusCode(&status_code));

            // An error body (404, rate limit) still parses as JSON, so only a 200 is worth looking at.
            if (stream == nullptr || status_code != 200) {
                return;
            }

            const auto result = stream->readEntireStreamAsString();

            if (result.isEmpty()) {
                return;
            }

            auto json = JSON::parse(result);

            if (json.isObject()) {
                const auto current_version_str = String("v") + String(JucePlugin_VersionString).trim();

                // Uncomment this line to test the new version available notification
                // const auto current_version_str = String("v0.0.1");

                if (!json.hasProperty("tag_name")) {
                    return;
                }

                const auto latest_version = json.getProperty("tag_name", var()).toString().trim();

                if (!_isVersionTag(latest_version)) {
                    return;
                }

                MessageManager::callAsync(
                    [current_version_str, latest_version, inShowNotificationOnLatestVersion, this] {
                        if (!current_version_str.equalsIgnoreCase(latest_version)) {
                            _showNewVersionAvailableNotification();
                        } else if (inShowNotificationOnLatestVersion) {
                            _showOnLatestVersionNotification();
                        }
                    });
            } else {
                jassertfalse;
            }
        });
    });
}

bool UpdateCheck::_isVersionTag(const String& inTag)
{
    return inTag.length() >= 2 && inTag[0] == 'v' && CharacterFunctions::isDigit(inTag[1]);
}

void UpdateCheck::_showNewVersionAvailableNotification()
{
    mUpdateAvailable = true;
    setVisible(true);
    mUrlButton.setVisible(true);
    mHideTime = Time::getCurrentTime() + RelativeTime::seconds(mNotificationDurationSeconds);

    startTimerHz(5);
}

void UpdateCheck::_showOnLatestVersionNotification()
{
    mUpdateAvailable = false;
    setVisible(true);
    mUrlButton.setVisible(false);
    mHideTime = Time::getCurrentTime() + RelativeTime::seconds(mNotificationDurationSeconds);

    startTimerHz(5);
}

void UpdateCheck::_hideNotification()
{
    stopTimer();
    mUrlButton.setVisible(false);
    setVisible(false);
}