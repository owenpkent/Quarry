//
// The Windows Standalone's half of the updater: fetch the installer, prove it is the one the
// release published, keep it from changing underneath us, check no DAW is holding the plugin,
// and start it. The decisions about what is acceptable live in UpdatePolicy; this file only does
// the I/O. See docs/AUTO_UPDATE.md for the flow and what each check does and does not stop.
//
// Nothing here is compiled elsewhere: the plugin and the other platforms only announce a release.
//

#ifndef UPDATEINSTALLER_H
#define UPDATEINSTALLER_H

#include <JuceHeader.h>

#include "UpdatePolicy.h"

#if JUCE_WINDOWS

#include <atomic>
#include <functional>

namespace quarry::update
{
struct InstallerResult
{
    bool ok {false};
    // A short line for the banner. The detail goes to the log.
    juce::String message;
};

class UpdateInstaller
{
public:
    /** Called from the worker thread with 0..1. */
    using ProgressCallback = std::function<void(double)>;

    /** Downloads the installer and checksum for the release, verifies and pins the installer.
     *  Blocking; call it from a worker thread. A release that is already pinned returns at once. */
    static InstallerResult prepare(const ReleaseInfo& inRelease,
                                   const ProgressCallback& inProgress,
                                   const std::atomic<bool>& inCancel);

    /** Fetches a small text file through the same hand-followed, allowlisted redirect path the
     *  installer uses. Fails, rather than truncates, if the body is over inMaxBytes. Blocking. */
    static bool fetchSmallText(const juce::String& inUrl,
                               juce::int64 inMaxBytes,
                               juce::String& outText,
                               juce::String& outError);

    /** The friendly name of the process holding the installed VST3 loaded (more than one are
     *  joined with commas), or an empty string if none is, or if the question could not be
     *  answered. The installer makes the same check itself, so a failed question is not fatal. */
    static juce::String findHostHoldingPlugin();

    /** True if the VST3 bundle is in the shared VST3 folder, which decides what the installer updates. */
    static bool isPluginInstalled();

    /** Starts the pinned installer. Does not quit the app; the caller does, once this says ok. */
    static InstallerResult launch();
};
} // namespace quarry::update

#endif // JUCE_WINDOWS

#endif // UPDATEINSTALLER_H
