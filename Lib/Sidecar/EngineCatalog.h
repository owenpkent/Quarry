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
 *
 * Two of the fields are read by nobody but a person, and they carry the whole of what the
 * picker knows about *when* to reach for an engine. "Kong", "Transkun" and "Muscriptor" are
 * the names their authors gave them; not one of the three says what it is for, and a list of
 * seven such names is a list of seven guesses. So every engine also carries the heading it
 * sits under, which is the material it is for, and the line it shows once chosen, which is
 * when you would pick it over the engine beside it. What it reports is not stored beside
 * them: reportsLine derives that from the flags, so a corrected flag cannot leave a stale
 * sentence next to it.
 */
namespace EngineCatalog
{

enum EngineIndex { BuiltIn = 0, Kong, Transkun, Muscriptor, SepKong, SepTranskun, SepMuscriptor, NumEngines };

// The four headings the picker groups by, named once so that two engines meant to share one
// cannot drift apart by a comma. In caps because that is what a heading looks like everywhere
// else in this interface; nothing upper-cases them at draw time.
constexpr const char* kAlwaysThere = "ALWAYS AVAILABLE, NO SETUP";
constexpr const char* kSoloPiano = "SOLO PIANO";
constexpr const char* kGeneral = "ANY INSTRUMENT, AND MIXES";
constexpr const char* kSplitFirst = "MIXES, SPLIT INTO PARTS FIRST (SLOWER)";

struct Engine {
    /** As sent in a transcribe request. Empty for the built-in tier, which never goes on a wire. */
    const char* wireName;
    const char* displayName;
    /** The heading this engine sits under in the picker, naming the material it is for.
        Engines that share one are adjacent in this table and the picker prints it once above
        the run, so a reader sorts seven proper nouns into four decisions before reading any
        one of them. */
    const char* group;
    /** The line under the picker once this engine is chosen: what it is for, and the one
        thing that would make someone pick it over the engine beside it under the same
        heading. Written rather than derived, because "the more cautious of the two" is a
        judgement read off the bench (docs/ANALYSIS.md 4.2) and no flag in this struct
        implies it.

        Short on purpose, and the ceiling is low. It is drawn into one 238 px row of
        LABEL_FONT and clipped, not wrapped, if it runs over, and nothing about that is
        visible until someone writes the sentence that overflows; Tests/engine_catalog_test.h
        holds the ceiling instead. Anything that applies to a whole run of engines -- that a
        sep+ take is slower, for instance -- belongs in the heading, which is written once and
        has more room. */
    const char* when;
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
        {"", "Built-in", kAlwaysThere, "Any instrument, and always there.", false, false, false},
        {"kong", "Kong", kSoloPiano, "Solo piano. Hears the pedal.", true, true, false},
        {"transkun", "Transkun", kSoloPiano, "Solo piano. The cautious one.", false, true, false},
        {"muscriptor", "Muscriptor", kGeneral, "Non-piano and mixes. Hears everything.", false, false, false},
        {"sep+kong", "Kong + separation", kSplitFirst, "Piano out of a mix, with pedal.", true, true, true},
        {"sep+transkun", "Transkun + separation", kSplitFirst, "Piano out of a mix, cautiously.", false,
         true, true},
        {"sep+muscriptor", "Muscriptor + separation", kSplitFirst, "A whole mix, part by part.", false, false,
         true},
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

/**
 * Which engine a configured sidecar starts on, given whatever QUARRY_SIDECAR_ENGINE said.
 *
 * Separate from indexForWireName because they answer different questions. That one asks what a
 * wire name means, and "auto" honestly means nothing this list can say. This one asks what to
 * do about it, and the answer cannot be the built-in tier: a machine that has gone to the
 * trouble of configuring a sidecar and then not named an engine is asking for the sidecar, and
 * seeding it back to BasicPitch would hand that machine a 0.775 onset F1 where it had 0.98
 * (docs/ANALYSIS.md 4.2) with nothing anywhere saying a substitution had happened.
 *
 * Kong rather than the best-scoring engine, and for the protocol's own reason: "auto" is a
 * fixed alias for kong on the sidecar side too (tools/sidecar/PROTOCOL.md, "Request"), because
 * it is the complete piano answer -- onsets, offsets, pedal and velocity, all first-class. The
 * difference is that the picker now names it on screen, so the default is a visible statement
 * rather than the invisible one this replaced.
 */
inline int startingEngine(const juce::String& inWireName)
{
    const auto named = indexForWireName(inWireName);
    return named != BuiltIn ? named : Kong;
}

// Why an engine cannot be reached, in the words every part of the interface uses for it.
//
// Written once because two places say them from two different sources of truth: the picker
// works off the sidecar's ready line and greys a row before a take, the summary works off
// EngineFallback and reports after one. They describe the same three situations, and a reader
// who is told "the sidecar would not start" in one and "sidecar unreachable" in the other has
// to work out for themselves whether those are one problem or two.
constexpr const char* kNeedsSidecar = "needs the sidecar";
constexpr const char* kSidecarUnreachable = "sidecar unreachable";
constexpr const char* kNotInstalled = "not installed";

/** One of the phrases above, capitalised for the start of a line rather than the middle of one. */
inline juce::String asSentence(const juce::String& inPhrase)
{ return inPhrase.substring(0, 1).toUpperCase() + inPhrase.substring(1); }

/**
 * The right-hand column of the picker's row: what this engine measures for itself.
 *
 * Derived from the flags rather than stored beside them. Two engines can sit under the same
 * heading and be indistinguishable to a reader until this line separates them -- Kong and
 * Transkun are both "solo piano" and differ only here -- so it has to be the same fact the
 * transcription is actually run on, not a second copy of it left behind by a correction.
 *
 * "Estimated velocity" is not a softer way of saying "velocity". An engine that measures none
 * of its own still comes out of the plugin with a velocity on every note, because NoteVelocity
 * reads them off the audio afterwards, and calling both "velocity" would claim a match between
 * a measurement and a guess.
 */
inline juce::String reportsLine(int inIndex)
{
    const auto& engine = get(inIndex);

    juce::StringArray parts;
    parts.add(engine.reportsPedal ? "pedal" : "no pedal");
    parts.add(engine.reportsVelocity ? "velocity" : "estimated velocity");

    return parts.joinIntoString(juce::String::fromUTF8(" \xc2\xb7 "));
}

/** The line under the picker: what the selected engine is for, and when you would reach for it. */
inline juce::String whenLine(int inIndex)
{ return get(inIndex).when; }

/** The heading the picker prints above this engine, naming the material it is for. */
inline juce::String groupOf(int inIndex)
{ return get(inIndex).group; }

} // namespace EngineCatalog

#endif // EngineCatalog_h
