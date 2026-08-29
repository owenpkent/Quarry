//
// The engine catalog is a contract with two parties that cannot check each other.
//
// One is the sidecar, which answers to wire names and only to wire names: PROTOCOL.md's table
// is the list, "sep+kong" is spelled exactly that way, and an engine asked for under any other
// spelling comes back {"ok":false} at transcribe time, on someone's take, with a fallback to
// the built-in tier that they then have to notice.
//
// The other is the host. The catalog's order is the choice list of an AudioParameterChoice, so
// index 1 has to still mean Kong in a session saved last month and reopened on a machine with
// no Python on it at all. Inserting an engine anywhere but the end, or sorting the list for
// tidiness, silently rewrites every saved project that used one.
//
// Neither party is present at compile time, so this stands in for both.
//

#ifndef QUARRY_ENGINE_CATALOG_TEST_H
#define QUARRY_ENGINE_CATALOG_TEST_H

#include <iostream>
#include <string>

#include <JuceHeader.h>

#include "EngineCatalog.h"

namespace engine_catalog_test_utils
{
static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (! condition)
    {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}
} // namespace engine_catalog_test_utils

inline bool engine_catalog_test()
{
    using namespace engine_catalog_test_utils;
    using namespace EngineCatalog;

    failures = 0;

    // The order, spelled out. Written as literals rather than derived from the enum, because a
    // test that reads the same table the code reads agrees with it by construction and would
    // have nothing to say about the day someone reorders both.
    check(BuiltIn == 0, "the built-in tier is index 0");
    check(Kong == 1, "kong is index 1");
    check(Transkun == 2, "transkun is index 2");
    check(Muscriptor == 3, "muscriptor is index 3");
    check(SepKong == 4, "sep+kong is index 4");
    check(SepTranskun == 5, "sep+transkun is index 5");
    check(SepMuscriptor == 6, "sep+muscriptor is index 6");
    check(NumEngines == 7, "seven engines, and a new one goes on the end");

    // The wire names, against PROTOCOL.md.
    check(juce::String(get(Kong).wireName) == "kong", "kong's wire name");
    check(juce::String(get(Transkun).wireName) == "transkun", "transkun's wire name");
    check(juce::String(get(Muscriptor).wireName) == "muscriptor", "muscriptor's wire name");

    // The sep+ twins are the base name with a fixed prefix, which is what the sidecar builds
    // its own list from. Derived here so a renamed base engine cannot leave its twin behind.
    check(juce::String(get(SepKong).wireName) == "sep+" + juce::String(get(Kong).wireName),
          "sep+kong is kong's name with the prefix");
    check(juce::String(get(SepTranskun).wireName) == "sep+" + juce::String(get(Transkun).wireName),
          "sep+transkun is transkun's name with the prefix");
    check(juce::String(get(SepMuscriptor).wireName) == "sep+" + juce::String(get(Muscriptor).wireName),
          "sep+muscriptor is muscriptor's name with the prefix");

    // The built-in tier never goes on a wire, and everything else must.
    check(juce::String(get(BuiltIn).wireName).isEmpty(), "the built-in tier has no wire name");
    check(! isSidecar(BuiltIn), "the built-in tier is not a sidecar engine");

    for (int i = 0; i < NumEngines; ++i)
    {
        const auto name = juce::String(get(i).displayName);
        check(name.isNotEmpty(), "every engine has a display name");
        check(traitLine(i).isNotEmpty(), "every engine says what it is for");

        if (i != BuiltIn)
        {
            check(isSidecar(i), "everything but the built-in tier runs through the sidecar");
            check(juce::String(get(i).wireName).isNotEmpty(), "every sidecar engine has a wire name");

            // The round trip is what the environment variable and the ready line both rely on.
            check(indexForWireName(get(i).wireName) == i, "a wire name maps back to its own index");
        }
    }

    // Display names are what a person picks from, so two the same is two they cannot tell apart.
    const auto names = displayNames();
    check(names.size() == NumEngines, "one display name per engine");

    juce::StringArray unique(names);
    unique.removeDuplicates(false);
    check(unique.size() == names.size(), "no two engines share a display name");

    // Whatever the sidecar is handed, an unrecognised name has to land somewhere that exists.
    check(indexForWireName("") == BuiltIn, "an empty wire name falls back to the built-in tier");
    check(indexForWireName("nonsense") == BuiltIn, "an unknown wire name falls back to the built-in tier");

    // "auto" is the protocol's own default and means "the sidecar picks". No index in a picker
    // can honestly say that, so it resolves to the one engine that is always there rather than
    // to a guess the UI would then have to display as fact.
    check(indexForWireName("auto") == BuiltIn, "auto is not a choice this list can make");

    // QUARRY_SIDECAR_ENGINE is typed by a person into a shell.
    check(indexForWireName("KONG") == Kong, "wire names match regardless of case");
    check(indexForWireName("  transkun  ") == Transkun, "wire names survive surrounding whitespace");

    // The traits are the part someone chooses on, and the protocol's table is where they come
    // from. Kong is the only engine that reports pedal; muscriptor is the only one with no
    // velocity of its own; the built-in tier has neither.
    check(get(Kong).reportsPedal, "kong reports pedal");
    check(! get(Transkun).reportsPedal, "transkun does not report pedal");
    check(! get(Muscriptor).reportsVelocity, "muscriptor reports no velocity");
    check(get(Transkun).reportsVelocity, "transkun measures velocity");
    check(! get(BuiltIn).reportsPedal && ! get(BuiltIn).reportsVelocity,
          "the built-in tier reports neither pedal nor its own velocity");

    // A sep+ engine keeps its base engine's reporting: separation happens in front of it and
    // changes what it is fed, not what it can measure.
    check(get(SepKong).reportsPedal == get(Kong).reportsPedal, "sep+kong reports what kong reports");
    check(get(SepMuscriptor).reportsVelocity == get(Muscriptor).reportsVelocity,
          "sep+muscriptor reports what muscriptor reports");
    check(get(SepKong).separatesStems && ! get(Kong).separatesStems, "only the sep+ engines separate");

    // Out of range is reachable from a saved session written by a later version of this list.
    check(&get(-1) == &get(0), "an index below the list clamps into it");
    check(&get(NumEngines + 5) == &get(NumEngines - 1), "an index past the list clamps into it");

    if (failures == 0)
        std::cout << "  all engine catalog checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_ENGINE_CATALOG_TEST_H
