#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The accessibility contract for the whole OK Studio line, as code.
//
// Every interaction must work with a single left-click, a drag, or a scroll.
// No keyboard requirement, no double-click, no modifier keys, no fine-precision
// gestures. Hit targets are at least minHitPx on a side. Right-click may exist
// only as an optional accelerator (e.g. Keys' per-note latch) and every such
// action must also be reachable left-click-only; nothing may require it.
//
// This is not a preference. Owen builds music mouse-only; it is the product.
namespace okstudio::ui
{
// Minimum comfortable click target, in pixels.
constexpr int minHitPx = 34;

// Belt-and-braces: a control that never steals keyboard focus from the DAW and
// never depends on the keyboard. Call from a component's constructor.
inline void makeMouseOnly(juce::Component& c)
{
    c.setWantsKeyboardFocus(false);
    c.setMouseClickGrabsKeyboardFocus(false);
}

// A window is only movable by its title bar, so a title bar off-screen means a window
// stuck where it is, permanently. Keyboard users can shove one back with Alt+Space;
// we have no such escape hatch, which makes this a mouse-only bug, not a cosmetic one.
//
// The trap: with a *native* title bar, the bar sits above the bounds JUCE reports, so
// the obvious clamp (jlimit the component into display->userArea) pushes the bar
// itself off the top edge. The window looks perfectly placed to the code that placed
// it. Anything positioning a top-level window should finish by calling this.
//
// Moves the whole frame, title bar included, inside the work area of the display it
// sits on. Position only: a window larger than the display pins to the top-left with
// its title bar reachable rather than being resized behind the user's back. A window
// already fully on-screen is left exactly where it is.
inline void ensureWindowReachable(juce::Component& window)
{
    auto frame = juce::BorderSize<int>();
    if (auto* peer = window.getPeer())
        if (const auto present = peer->getFrameSizeIfPresent())
            frame = *present;

    // The component's bounds exclude any native title bar; the frame border puts it back.
    const auto outer = frame.addedTo(window.getScreenBounds());
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(outer);
    if (display == nullptr)
        return;

    const auto area = display->userArea;
    const auto fitted = outer.withPosition(
        juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - outer.getWidth()), outer.getX()),
        juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - outer.getHeight()), outer.getY()));

    if (fitted != outer)
        window.setBounds(frame.subtractedFrom(fitted));
}
} // namespace okstudio::ui
