#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>

//
// What Windows will tell us about the application behind a pid.
//
// Four signals were designed for; two are here. Process and window title always work.
// Browser tab URL works when the browser cooperates. The media session (track and artist)
// needs C++/WinRT and a compiler flag this project does not set yet, and the window
// screenshot is the third of them, below.
//
// Every field is allowed to be empty, and each signal fails on its own. A missing URL never
// costs you the title, because a capture that half-describes itself is worth much more than
// one that describes itself not at all.
//

#if JUCE_WINDOWS

namespace quarry::sampler
{

struct SourceIdentity
{
    /** The frontmost visible top-level window belonging to the process, or to whichever
        ancestor of it has one. Empty if nothing owns a window. */
    juce::String windowTitle;

    /** True when that process owned more than one window it could have been, so the title is
        one of several. A browser with two windows open is one process, and nothing about the
        pid says which of them made the sound. */
    bool windowTitleIsAmbiguous = false;

    /** The address bar, for a browser that will say. Empty for everything else. */
    juce::String url;
};

/** One window an application is showing, as offered to whoever is picking a source. */
struct SourceWindow
{
    /** The HWND, carried as an integer so this header stays clear of windows.h. */
    juce::uint64 handle = 0;

    juce::String title;
};

/**
 * Every visible top-level window behind a pid, frontmost first.
 *
 * The same walk up the process tree that identifySource does, but returning all of what it
 * finds rather than one of it. Two browser windows are two entries here and one process
 * underneath, which is the whole reason this exists: the pid cannot say which of them is
 * making the sound, so the choice belongs to the person who can see them.
 *
 * Empty when nothing in that line owns a window.
 */
std::vector<SourceWindow> windowsOfSource(juce::uint32 processId);

/** One window on the desktop, and the application behind it. */
struct DesktopWindow
{
    /** The HWND, carried as an integer so this header stays clear of windows.h. */
    juce::uint64 handle = 0;

    juce::String title;

    juce::uint32 processId = 0;

    /** "chrome.exe". Empty when the process would not say. */
    juce::String processName;
};

/**
 * Every window on the desktop a person could point at, frontmost first.
 *
 * windowsOfSource() answers "which windows does this pid own", which is only askable once
 * something is already making a sound. This is the other direction: everything that is open,
 * whether or not it holds an audio session, because a browser tab that is paused is exactly
 * the thing you want to arm the recorder on *before* you hit play on it.
 *
 * Process loopback does not need the target to be audible. Its stream is a clock: a silent
 * process still delivers zero-filled packets at the requested rate, so arming on a quiet
 * window and waiting is a supported way to use it, not a trick.
 *
 * Skips owned windows, untitled windows, and the cloaked shells Windows keeps around for
 * suspended UWP apps, all of which are things nobody means when they say "that window".
 * Quarry's own windows are skipped too.
 */
std::vector<DesktopWindow> desktopWindows();

/**
 * The full path of a process's executable, or empty.
 *
 * The audio-session enumeration reports this for anything holding a session. A window picked
 * out of desktopWindows() may not be, and the capture sidecar still wants the path, so this
 * asks the process directly.
 */
juce::String executablePathOf(juce::uint32 processId);

/**
 * Title and, where there is one, the URL.
 *
 * The pid that owns a render session is not always the pid with the window: a browser
 * renders audio from a child that has none, so this walks up the process tree until a
 * window turns up, which is exactly the case the sampler exists to serve.
 *
 * `windowHandle` names which of the application's windows the take is of, as chosen from
 * windowsOfSource(). Pass 0 to let this pick, which is only ever a guess when the application
 * is showing more than one; `windowTitleIsAmbiguous` says when it was.
 *
 * Reading the URL uses UI Automation, and Chrome turns on renderer accessibility when a UIA
 * client appears. That is a real cost paid by the browser, not by us, and it is the reason
 * this is a deliberate call at the start of a take rather than something polled.
 */
SourceIdentity identifySource(juce::uint32 processId, juce::uint64 windowHandle = 0);

/**
 * A picture of the source's window, downscaled, or a null image.
 *
 * Taken when recording starts and written when it stops, because by then the window may
 * show something else entirely and the sample is a record of what was playing, not of what
 * happened to be on screen afterwards.
 */
juce::Image captureWindowImage(juce::uint32 processId, juce::uint64 windowHandle = 0);

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
