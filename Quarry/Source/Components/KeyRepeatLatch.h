//
// One click per physical key press.
//
// A key held down past the keyboard's repeat delay arrives as a stream of key presses, and a
// shortcut that toggles something (the space bar on the Sample page starts a take and stops it)
// would flip on every one of them: a held key would cut one recording into several short ones.
// The latch takes the first press and refuses the rest until it is told the key came up.
//
// It knows nothing about keys or components, so it can be tested without either. The view that
// owns it calls press() for each key press and release() when the key is up or the focus has gone
// (a key released while another window has the focus never reports its release here).
//

#ifndef QUARRY_KEYREPEATLATCH_H
#define QUARRY_KEYREPEATLATCH_H

namespace quarry
{
class KeyRepeatLatch
{
public:
    /** Call for every key press. True for the first one since release(), false for the repeats. */
    bool press() noexcept
    {
        if (mHeld)
            return false;

        mHeld = true;
        return true;
    }

    /** The key came up, or the focus left: the next press counts again. */
    void release() noexcept { mHeld = false; }

    bool isHeld() const noexcept { return mHeld; }

private:
    bool mHeld {false};
};
} // namespace quarry

#endif // QUARRY_KEYREPEATLATCH_H
