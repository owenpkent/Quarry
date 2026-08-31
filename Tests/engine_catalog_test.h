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

#include "Components/Views/LeftColumnLayout.h"
#include "EngineCatalog.h"
#include "UIDefines.h"

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
        check(reportsLine(i).isNotEmpty(), "every engine says what it reports");
        check(whenLine(i).isNotEmpty(), "every engine says what it is for");
        check(groupOf(i).isNotEmpty(), "every engine sits under a heading");

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

    // What the picker shows, and the reason it can be read at all.
    //
    // Six of the seven display names are the names their authors chose -- "Kong", "Transkun",
    // "Muscriptor" -- and not one of them says what it is for. The heading and the when line
    // are the entire answer to "which of these do I want", so they are checked here rather
    // than left to be noticed by someone choosing wrong.

    // The picker prints a heading once above the run of engines that share it, so a heading
    // that appears twice with something else in between prints twice and groups nothing.
    {
        juce::StringArray runs;

        for (int i = 0; i < NumEngines; ++i)
            if (runs.isEmpty() || runs[runs.size() - 1] != groupOf(i))
                runs.add(groupOf(i));

        juce::StringArray distinct(runs);
        distinct.removeDuplicates(false);
        check(distinct.size() == runs.size(), "engines sharing a heading are adjacent in the table");
        check(runs.size() >= 3, "the list is grouped, not one heading over all of it");
    }

    // Two engines under one heading are, by construction, for the same material. The second
    // column is then the only thing between them, and two identical ones are a coin toss.
    for (int i = 0; i < NumEngines; ++i)
        for (int j = i + 1; j < NumEngines; ++j)
            if (groupOf(i) == groupOf(j))
                check(reportsLine(i) != reportsLine(j),
                      "two engines under one heading can be told apart by what they report");

    // The when line is drawn into one row of LABEL_FONT the width of the picker, and clipped
    // there, not wrapped. Nothing makes an overflow visible except selecting that engine and
    // looking at the panel, which is six selections away from wherever the sentence was
    // written, so it is measured here in the font it is actually drawn in.
    {
        const auto label = UIDefines::LABEL_FONT();
        const auto room = (float) LeftColumnLayout::MODEL_ROW_WIDTH;

        float widest = 0.0f;
        int widest_engine = 0;

        for (int i = 0; i < NumEngines; ++i)
        {
            const auto width = label.getStringWidthFloat(whenLine(i));

            if (width > widest)
            {
                widest = width;
                widest_engine = i;
            }

            check(width <= room, "a when line fits the panel unclipped");
        }

        // Printed rather than only asserted, because the number is the whole margin anyone
        // writing the next engine's line has to work with, and it is not otherwise knowable
        // without building a window and selecting that engine.
        std::cout << "  widest when line: " << get(widest_engine).displayName << ", " << widest
                  << " px of " << room << std::endl;
    }

    // Distinct, or two engines give the same answer to "what is this for" and the line is
    // doing nothing for at least one of them.
    {
        juce::StringArray whens;

        for (int i = 0; i < NumEngines; ++i)
            whens.add(whenLine(i));

        juce::StringArray unique_whens(whens);
        unique_whens.removeDuplicates(false);
        check(unique_whens.size() == whens.size(), "no two engines say the same thing about themselves");
    }

    // Whatever the sidecar is handed, an unrecognised name has to land somewhere that exists.
    check(indexForWireName("") == BuiltIn, "an empty wire name falls back to the built-in tier");
    check(indexForWireName("nonsense") == BuiltIn, "an unknown wire name falls back to the built-in tier");

    // "auto" is the protocol's own default and means "the sidecar picks". No index in a picker
    // can honestly say that, so it resolves to the one engine that is always there rather than
    // to a guess the UI would then have to display as fact.
    check(indexForWireName("auto") == BuiltIn, "auto is not a choice this list can make");

    // QUARRY_SIDECAR_ENGINE is typed by a person into a shell.
    // startingEngine answers the other half of the same question: not what a wire name means,
    // but what to do when it means nothing. It has to differ from indexForWireName on exactly
    // the inputs that land on BuiltIn, because seeding the built-in tier on a machine that
    // configured a sidecar is how the sidecar silently stopped being used at all.
    check(startingEngine("kong") == Kong, "a named engine starts on itself");
    check(startingEngine("sep+transkun") == SepTranskun, "a named sep+ engine starts on itself");
    check(isSidecar(startingEngine("auto")), "auto starts on a sidecar engine, not on the built-in tier");
    check(isSidecar(startingEngine("")), "an unset engine name still starts on the sidecar");
    check(isSidecar(startingEngine("nonsense")), "an unrecognised engine name still starts on the sidecar");

    for (int i = 0; i < NumEngines; ++i)
        check(startingEngine(get(i).wireName) == (isSidecar(i) ? i : startingEngine("auto")),
              "every sidecar engine's wire name starts on that engine");

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
    // The three phrases the picker and the summary both use. Written once so a change to one
    // cannot leave the panel and the sentence under it describing the same failure differently;
    // the check that matters is that they are distinguishable and that asSentence only touches
    // the first letter, since the summary uses them mid-sentence and the panel starts a line.
    check(juce::String(kNeedsSidecar) != juce::String(kNotInstalled)
              && juce::String(kNotInstalled) != juce::String(kSidecarUnreachable),
          "the three unavailability phrases say three different things");
    check(asSentence(kSidecarUnreachable) == "Sidecar unreachable", "asSentence capitalises the first letter");
    check(asSentence(kSidecarUnreachable).substring(1) == juce::String(kSidecarUnreachable).substring(1),
          "asSentence changes nothing but the first letter");
    check(asSentence("").isEmpty(), "asSentence survives an empty phrase");

    check(&get(-1) == &get(0), "an index below the list clamps into it");
    check(&get(NumEngines + 5) == &get(NumEngines - 1), "an index past the list clamps into it");

    if (failures == 0)
        std::cout << "  all engine catalog checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_ENGINE_CATALOG_TEST_H
