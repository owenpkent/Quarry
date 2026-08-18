//
// Checking that the key reading says what it means.
//

#ifndef NN_KEY_ESTIMATE_TEST_H
#define NN_KEY_ESTIMATE_TEST_H

#include <cmath>
#include <iostream>
#include <vector>

#include "KeyEstimate.h"
#include "Notes.h"

namespace key_estimate_test_utils
{
static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

struct ToneWeight {
    int semitone;
    double weight;
};

/** One note of the given weight, since estimateKey weighs duration times velocity. */
static Notes::Event makeEvent(int inPitch, double inDuration, double inVelocity)
{
    Notes::Event event {};

    event.startTime = 0.0;
    event.endTime = inDuration;
    event.startFrame = 0;
    event.endFrame = 1;
    event.pitch = inPitch;
    event.velocity = inVelocity;

    return event;
}

/** A phrase given as intervals above inRootPitch, so the same shape can be transposed. */
static std::vector<Notes::Event> makePhrase(int inRootPitch, const std::vector<ToneWeight>& inTones)
{
    std::vector<Notes::Event> events;

    for (const auto& tone : inTones)
        events.push_back(makeEvent(inRootPitch + tone.semitone, tone.weight, 1.0));

    return events;
}

static bool expectKey(const char* inWhat, const KeyEstimate& inEstimate, int inRootNote, bool inIsMinor)
{
    if (!inEstimate.isValid()) {
        std::cout << "FAIL: " << inWhat << " was rejected, confidence " << inEstimate.confidence << ", expected "
                  << kNoteNames[inRootNote] << (inIsMinor ? " minor" : " major") << std::endl;
        return false;
    }

    if (inEstimate.rootNote != inRootNote || inEstimate.isMinor != inIsMinor) {
        std::cout << "FAIL: " << inWhat << " read as " << inEstimate.toString().toStdString() << ", expected "
                  << kNoteNames[inRootNote] << (inIsMinor ? " minor" : " major") << std::endl;
        return false;
    }

    return true;
}

static bool expectNoKey(const char* inWhat, const KeyEstimate& inEstimate)
{
    if (std::isnan(inEstimate.confidence)) {
        std::cout << "FAIL: " << inWhat << " produced a NaN confidence" << std::endl;
        return false;
    }

    if (inEstimate.isValid()) {
        std::cout << "FAIL: " << inWhat << " read as " << inEstimate.toString().toStdString() << " with confidence "
                  << inEstimate.confidence << ", expected no key" << std::endl;
        return false;
    }

    if (inEstimate.toString().isNotEmpty()) {
        std::cout << "FAIL: " << inWhat << " named a key it does not have: "
                  << inEstimate.toString().toStdString() << std::endl;
        return false;
    }

    return true;
}

/** An estimate refused by the support gate is left at zero, so a positive confidence
    proves the reading was scored and then turned down by kMinConfidence instead. */
static bool expectScoredThenRejected(const char* inWhat, const KeyEstimate& inEstimate)
{
    if (!(inEstimate.confidence > 0.0f)) {
        std::cout << "FAIL: " << inWhat << " scored " << inEstimate.confidence
                  << ", expected a positive confidence turned down by kMinConfidence rather than a gate refusal"
                  << std::endl;
        return false;
    }

    return true;
}
} // namespace key_estimate_test_utils

/*
 * The figures asserted here come from running the shipped profiles: a tonic-led
 * major phrase correlates around 0.98, a flat scale around 0.76, and the two
 * pitch classes of a kick and a snare around 0.84, which is why the support gate
 * rather than the confidence has to be what rejects the drum loop.
 *
 * Above two pitch classes the gate is no longer the thing doing the work, so the
 * whole tone and diminished seventh cases below clear it and are left to
 * kMinConfidence, scoring 0.068 and 0.322. They are here so that relaxing that
 * threshold shows up as a failure: without them nothing in this file lands
 * between zero and 0.5, and the threshold could be removed unnoticed.
 */
bool key_estimate_test()
{
    using namespace key_estimate_test_utils;

    bool succeeded = true;

    // C major with the tonic and dominant held longest.
    const std::vector<ToneWeight> major_phrase = {
        {0, 4.0}, {2, 1.0}, {4, 2.0}, {5, 1.0}, {7, 3.0}, {9, 1.0}, {11, 1.0}};

    std::cout << "  Clean C major: ";
    const auto c_major = estimateKey(makePhrase(60, major_phrase));
    std::cout << c_major.toString().toStdString() << " " << c_major.confidence << std::endl;
    succeeded &= expectKey("a clean C major phrase", c_major, 0, false);

    if (c_major.confidence < 0.9f) {
        std::cout << "FAIL: a clean C major phrase scored only " << c_major.confidence << ", expected at least 0.9"
                  << std::endl;
        succeeded = false;
    }

    // The same shape rooted on F, to show nothing is anchored to C.
    std::cout << "  Transposed to F: ";
    const auto f_major = estimateKey(makePhrase(65, major_phrase));
    std::cout << f_major.toString().toStdString() << " " << f_major.confidence << std::endl;
    succeeded &= expectKey("the same phrase rooted on F", f_major, 5, false);

    // A minor, leaning hard on the tonic so it is not heard as its relative major.
    std::cout << "  Clean A minor: ";
    const auto a_minor = estimateKey(makePhrase(69, {{0, 6.0}, {2, 1.5}, {3, 3.0}, {5, 1.5}, {7, 4.0}, {8, 1.0}, {10, 1.0}}));
    std::cout << a_minor.toString().toStdString() << " " << a_minor.confidence << std::endl;
    succeeded &= expectKey("a clean A minor phrase", a_minor, 9, true);

    // A bare triad is the smallest thing that tells major from minor, so the support
    // gate has to let three pitch classes through.
    std::cout << "  Bare C major triad: ";
    const auto c_triad = estimateKey(makePhrase(60, {{0, 1.0}, {4, 1.0}, {7, 1.0}}));
    std::cout << c_triad.toString().toStdString() << " " << c_triad.confidence << std::endl;
    succeeded &= expectKey("a bare C-E-G triad", c_triad, 0, false);

    // A kick and a snare: two pitch classes, which correlation alone would reward.
    std::cout << "  Kick and snare: ";
    const auto drums = estimateKey({makeEvent(36, 2.0, 1.0), makeEvent(39, 1.0, 1.0)});
    std::cout << (drums.isValid() ? drums.toString().toStdString() : std::string("no key")) << std::endl;
    succeeded &= expectNoKey("a two pitch class drum loop", drums);

    // Six pitch classes, evenly spread, belonging to no key. The gate cannot touch
    // this one, so only the confidence threshold can turn it down.
    std::cout << "  Whole tone run: ";
    const auto whole_tone =
        estimateKey(makePhrase(60, {{0, 1.0}, {2, 1.0}, {4, 1.0}, {6, 1.0}, {8, 1.0}, {10, 1.0}}));
    std::cout << (whole_tone.isValid() ? whole_tone.toString().toStdString() : std::string("no key")) << " "
              << whole_tone.confidence << std::endl;
    succeeded &= expectNoKey("an equal weight whole tone run", whole_tone);
    succeeded &= expectScoredThenRejected("an equal weight whole tone run", whole_tone);

    // Four pitch classes with no tonic among them: scores higher than the whole tone
    // run and still has to be refused, which brackets the threshold from below.
    std::cout << "  Diminished seventh: ";
    const auto dim_seventh = estimateKey(makePhrase(60, {{0, 1.0}, {3, 1.0}, {6, 1.0}, {9, 1.0}}));
    std::cout << (dim_seventh.isValid() ? dim_seventh.toString().toStdString() : std::string("no key")) << " "
              << dim_seventh.confidence << std::endl;
    succeeded &= expectNoKey("an equal weight diminished seventh stack", dim_seventh);
    succeeded &= expectScoredThenRejected("an equal weight diminished seventh stack", dim_seventh);

    std::cout << "  Empty take: ";
    const auto empty = estimateKey({});
    std::cout << (empty.isValid() ? empty.toString().toStdString() : std::string("no key")) << std::endl;
    succeeded &= expectNoKey("an empty take", empty);

    std::cout << "  Zero length notes: ";
    const auto no_duration =
        estimateKey({makeEvent(60, 0.0, 1.0), makeEvent(62, 0.0, 1.0), makeEvent(64, 0.0, 1.0), makeEvent(67, 0.0, 1.0)});
    std::cout << (no_duration.isValid() ? no_duration.toString().toStdString() : std::string("no key")) << std::endl;
    succeeded &= expectNoKey("notes with no duration", no_duration);

    std::cout << "  Silent notes: ";
    const auto no_velocity =
        estimateKey({makeEvent(60, 1.0, 0.0), makeEvent(62, 1.0, 0.0), makeEvent(64, 1.0, 0.0), makeEvent(67, 1.0, 0.0)});
    std::cout << (no_velocity.isValid() ? no_velocity.toString().toStdString() : std::string("no key")) << std::endl;
    succeeded &= expectNoKey("notes with no velocity", no_velocity);

    std::cout << "  Default estimate: ";
    const KeyEstimate untouched {};
    std::cout << (untouched.isValid() ? untouched.toString().toStdString() : std::string("no key")) << std::endl;
    succeeded &= expectNoKey("a default estimate", untouched);

    if (succeeded) {
        std::cout << "Success" << std::endl;
    }

    return succeeded;
}

#endif //NN_KEY_ESTIMATE_TEST_H
