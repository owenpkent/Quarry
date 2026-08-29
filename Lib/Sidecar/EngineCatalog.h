//
// Every transcription engine Quarry can be asked for, in one closed, ordered list.
//

#ifndef EngineCatalog_h
#define EngineCatalog_h

#include <JuceHeader.h>

/**
 * The engines the picker offers and the wire names the sidecar answers to, kept in one place
 * because they have to agree and they are edited for different reasons.
 *
 * The list is closed and ordered deliberately. It is the choice list of an
 * AudioParameterChoice, so an index has to mean the same engine on a machine with no Python at
 * all as it does on one with every model installed, and it has to still mean it when a session
 * saved on the second is opened on the first. Discovering the list at runtime from the
 * sidecar's "ready" line, which is where availability genuinely comes from, would make the
 * index a function of what happened to be installed the day the session was saved. So
 * availability greys a row out; it never removes one.
 *
 * The traits are the protocol's own table (tools/sidecar/PROTOCOL.md, "Engines"), not bench
 * scores. They are categorical facts about what an engine reports, they do not move when the
 * corpus changes, and they are the part a person can actually choose on: a take with pedal
 * wants an engine that reports pedal.
 */
namespace EngineCatalog
{

enum EngineIndex { BuiltIn = 0, Kong, Transkun, Muscriptor, SepKong, SepTranskun, SepMuscriptor, NumEngines };

struct Engine {
    /** As sent in a transcribe request. Empty for the built-in tier, which never goes on a wire. */
    const char* wireName;
    const char* displayName;
    /** What the engine is for, in the words the picker shows. */
    const char* scope;
    bool reportsPedal;
    /** True when the model itself measures velocity per note. The built-in tier does not: its
        dynamics come from NoteVelocity reading the audio afterwards, which is a different and
        weaker thing, and the picker says so rather than claiming a match. */
    bool reportsVelocity;
    bool separatesStems;
};

inline const Engine* table()
{
    static const Engine engines[NumEngines] = {
        {"", "Built-in", "Any instrument", false, false, false},
        {"kong", "Kong", "Piano", true, true, false},
        {"transkun", "Transkun", "Piano", false, true, false},
        {"muscriptor", "Muscriptor", "Any instrument", false, false, false},
        {"sep+kong", "Kong + separation", "Mixes, piano parts", true, true, true},
        {"sep+transkun", "Transkun + separation", "Mixes, piano parts", false, true, true},
        {"sep+muscriptor", "Muscriptor + separation", "Mixes", false, false, true},
    };

    return engines;
}

inline const Engine& get(int inIndex)
{
    jassert(inIndex >= 0 && inIndex < NumEngines);
    return table()[juce::jlimit(0, NumEngines - 1, inIndex)];
}

/** Everything but the built-in tier runs out of process, through the sidecar. */
inline bool isSidecar(int inIndex)
{ return inIndex != BuiltIn; }

inline juce::StringArray displayNames()
{
    juce::StringArray names;

    for (int i = 0; i < NumEngines; ++i)
        names.add(get(i).displayName);

    return names;
}

/**
 * @return The index for a sidecar wire name, or BuiltIn for anything unrecognised.
 *
 * "auto" lands on BuiltIn rather than on a guess. It is the protocol's own default and means
 * "whatever the sidecar picks", which is exactly the thing a picker exists to stop happening:
 * a person who opens the page should be told which engine is about to run, and no index in
 * this list honestly says "one of these".
 */
inline int indexForWireName(const juce::String& inWireName)
{
    const auto wanted = inWireName.trim().toLowerCase();

    for (int i = 0; i < NumEngines; ++i)
        if (wanted == juce::String(get(i).wireName) && wanted.isNotEmpty())
            return i;

    return BuiltIn;
}

/** The line under the picker: what the selected engine is for, and what it reports. */
inline juce::String traitLine(int inIndex)
{
    const auto& engine = get(inIndex);

    juce::StringArray parts;
    parts.add(engine.scope);
    parts.add(engine.reportsPedal ? "pedal" : "no pedal");
    parts.add(engine.reportsVelocity ? "velocity" : "estimated velocity");

    return parts.joinIntoString(juce::String::fromUTF8(" \xc2\xb7 "));
}

} // namespace EngineCatalog

#endif // EngineCatalog_h
