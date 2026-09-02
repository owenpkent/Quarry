//
// One line of the activity log, formatted for the drawer: a clock time and the text, so the
// formatting can be tested without a Component to paint it into.
//

#ifndef ActivityFormat_h
#define ActivityFormat_h

#include <JuceHeader.h>

#include "ActivityLog.h"

namespace quarry
{

/** "HH:MM:SS  text", in local time. Two spaces rather than one: a single space next to a
    monospace clock reads as part of the timestamp, and the gap is what tells the eye the
    clock has ended and the message has started. */
inline juce::String formatActivityLine(const ActivityLine& inLine)
{
    const juce::Time time(inLine.timeMs);

    const auto pad = [](int n) { return juce::String(n).paddedLeft('0', 2); };

    return pad(time.getHours()) + ":" + pad(time.getMinutes()) + ":" + pad(time.getSeconds())
         + "  " + inLine.text;
}

} // namespace quarry

#endif // ActivityFormat_h
