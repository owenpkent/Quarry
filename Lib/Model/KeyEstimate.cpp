//
// Working out what key a transcription is in.
//

#include "KeyEstimate.h"

#include <array>
#include <cmath>

namespace
{
// Krumhansl and Kessler's probe-tone profiles, relative to the tonic.
const std::array<double, 12> kMajorProfile = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};

const std::array<double, 12> kMinorProfile = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// A pitch class counts towards support once it carries this much of the take.
constexpr double kSupportShare = 0.01;

// A one or two class histogram puts nearly all of its deviation onto those bins,
// so correlating it measures that collapse rather than a key: a kick and a snare
// alone score 0.84 against C minor, above a real scale. That degenerate case is
// the only thing this gate is for. Three classes is the smallest histogram that
// tells a major triad from a minor one, so the gate stops there and
// KeyEstimate::kMinConfidence carries everything above it, which is where the
// measured drum kits fall, between 0.28 and 0.38. Neither is a percussion filter:
// a four class spread that happens to sit on a chord shape scores 0.75 and passes
// both.
constexpr int kMinSupportingPitchClasses = 3;

/** Pearson correlation of a histogram against a profile rotated to inRoot. */
double correlate(const std::array<double, 12>& inHistogram,
                 const std::array<double, 12>& inProfile,
                 int inRoot)
{
    double hist_mean = 0.0;
    double prof_mean = 0.0;

    for (int i = 0; i < 12; i++) {
        hist_mean += inHistogram[static_cast<size_t>(i)];
        prof_mean += inProfile[static_cast<size_t>(i)];
    }

    hist_mean /= 12.0;
    prof_mean /= 12.0;

    double numerator = 0.0;
    double hist_var = 0.0;
    double prof_var = 0.0;

    for (int i = 0; i < 12; i++) {
        // Rotating the lookup is the same as rotating the profile, and cheaper.
        const auto h = inHistogram[static_cast<size_t>((i + inRoot) % 12)] - hist_mean;
        const auto p = inProfile[static_cast<size_t>(i)] - prof_mean;

        numerator += h * p;
        hist_var += h * h;
        prof_var += p * p;
    }

    const auto denominator = std::sqrt(hist_var * prof_var);

    if (denominator <= 0.0)
        return 0.0;

    return numerator / denominator;
}
} // namespace

String KeyEstimate::toString() const
{
    if (!isValid())
        return {};

    return String(kNoteNames[rootNote]) + (isMinor ? " minor" : " major");
}

String KeyEstimate::runnerUpToString() const
{
    if (!isValid())
        return {};

    return String(kNoteNames[runnerUpRoot]) + (runnerUpIsMinor ? " minor" : " major");
}

KeyEstimate estimateKey(const std::vector<Notes::Event>& inNoteEvents)
{
    KeyEstimate estimate;

    if (inNoteEvents.empty())
        return estimate;

    std::array<double, 12> histogram {};
    histogram.fill(0.0);

    double total_weight = 0.0;

    for (const auto& event : inNoteEvents) {
        const auto duration = event.endTime - event.startTime;

        if (duration <= 0.0 || event.pitch < 0)
            continue;

        // Loudness as well as duration: a loud held note is more tonally
        // telling than a quiet one of the same length. Measured loudness, not
        // the model's confidence, which says nothing about how the note was played.
        const auto weight = duration * jmax(0.0, event.velocity);

        histogram[static_cast<size_t>(event.pitch % 12)] += weight;
        total_weight += weight;
    }

    if (total_weight <= 0.0)
        return estimate;

    int supporting_pitch_classes = 0;

    for (const auto bin : histogram) {
        if (bin >= kSupportShare * total_weight)
            supporting_pitch_classes++;
    }

    if (supporting_pitch_classes < kMinSupportingPitchClasses)
        return estimate;

    double best = -1.0;
    double second = -1.0;

    // The runner-up is kept because the winner alone overstates how settled the answer is.
    // Two rotations a hundredth apart is a different situation from one that won by a
    // quarter, and the readout can only say so if the loser survives the loop.
    const auto consider = [&](double score, int root, bool minor) {
        if (score > best) {
            second = best;
            estimate.runnerUpRoot = estimate.rootNote;
            estimate.runnerUpIsMinor = estimate.isMinor;

            best = score;
            estimate.rootNote = root;
            estimate.isMinor = minor;
            return;
        }

        if (score > second) {
            second = score;
            estimate.runnerUpRoot = root;
            estimate.runnerUpIsMinor = minor;
        }
    };

    for (int root = 0; root < 12; root++) {
        consider(correlate(histogram, kMajorProfile, root), root, false);
        consider(correlate(histogram, kMinorProfile, root), root, true);
    }

    // The clamp only keeps the number inside its advertised range; what decides
    // whether the answer is usable is KeyEstimate::kMinConfidence.
    estimate.confidence = static_cast<float>(jlimit(0.0, 1.0, best));
    estimate.runnerUpConfidence = static_cast<float>(jlimit(0.0, 1.0, second));

    return estimate;
}
