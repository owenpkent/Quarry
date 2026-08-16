#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

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
    /** The biggest visible top-level window belonging to the process, or to whichever
        ancestor of it has one. Empty if nothing owns a window. */
    juce::String windowTitle;

    /** The address bar, for a browser that will say. Empty for everything else. */
    juce::String url;
};

/**
 * Title and, where there is one, the URL.
 *
 * The pid that owns a render session is not always the pid with the window: a browser
 * renders audio from a child that has none, so this walks up the process tree until a
 * window turns up, which is exactly the case the sampler exists to serve.
 *
 * Reading the URL uses UI Automation, and Chrome turns on renderer accessibility when a UIA
 * client appears. That is a real cost paid by the browser, not by us, and it is the reason
 * this is a deliberate call at the start of a take rather than something polled.
 */
SourceIdentity identifySource(juce::uint32 processId);

/**
 * A picture of the source's window, downscaled, or a null image.
 *
 * Taken when recording starts and written when it stops, because by then the window may
 * show something else entirely and the sample is a record of what was playing, not of what
 * happened to be on screen afterwards.
 */
juce::Image captureWindowImage(juce::uint32 processId);

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
