//
// Naming shared by everything that writes a take to the folder you picked.
//

#ifndef TakeNaming_h
#define TakeNaming_h

#include <JuceHeader.h>

namespace TakeNaming
{
constexpr int kMaxStemAttempts = 999;

/** A stem free for every extension the caller writes, so a pair cannot drift apart.
    Empty when every candidate is taken, which the caller has to report rather than
    write over somebody's take.
*/
inline String freeStem(const File& inFolder, const String& inBase, const StringArray& inExtensions)
{
    for (int attempt = 1; attempt <= kMaxStemAttempts; attempt++) {
        const auto stem = attempt == 1 ? inBase : inBase + " (" + String(attempt) + ")";
        bool taken = false;

        for (const auto& extension: inExtensions)
            taken = taken || inFolder.getChildFile(stem + extension).existsAsFile();

        if (!taken)
            return stem;
    }

    return {};
}
} // namespace TakeNaming

#endif // TakeNaming_h
