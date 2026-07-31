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

        // Amplitude as well as duration: a loud held note is more tonally
        // telling than a quiet one of the same length.
        const auto weight = duration * jmax(0.0, event.amplitude);

        histogram[static_cast<size_t>(event.pitch % 12)] += weight;
        total_weight += weight;
    }

    if (total_weight <= 0.0)
        return estimate;

    double best = -1.0;

    for (int root = 0; root < 12; root++) {
        const auto major = correlate(histogram, kMajorProfile, root);
        const auto minor = correlate(histogram, kMinorProfile, root);

        if (major > best) {
            best = major;
            estimate.rootNote = root;
            estimate.isMinor = false;
        }

        if (minor > best) {
            best = minor;
            estimate.rootNote = root;
            estimate.isMinor = true;
        }
    }

    // A negative correlation means the material does not fit any key; report it
    // as no confidence rather than as a key nobody should act on.
    estimate.confidence = static_cast<float>(jlimit(0.0, 1.0, best));

    return estimate;
}
