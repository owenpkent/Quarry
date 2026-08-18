//
// Created by Damien Ronssin on 10.03.23.
//

#include <algorithm>
#include <array>

#include "BasicPitch.h"

namespace
{
// How far either sensitivity knob can pull a derived threshold. The knobs run 0.05 to 0.95, so
// this gives roughly plus or minus 0.36 around whatever the take asked for: enough to override a
// bad reading, not enough to get back to the pathological ends of the old raw mapping.
constexpr float kSensitivitySpan = 0.8f;

// Bounds on a derived threshold. Neither end of this is a number anyone would choose deliberately,
// and a strange take can put an unconstrained fit anywhere.
constexpr float kMinDerivedThreshold = 0.25f;
constexpr float kMaxDerivedThreshold = 0.75f;

// Absolute floor under the derived noise floor, for a take with no measurable noise at all.
constexpr float kMinNoiseFloor = 0.05f;

// MAD sigmas above the median at which a cell stops being noise. Eight is deliberately far out:
// the floor is a very tight distribution and everything of interest is a long way above it.
constexpr double kFloorSigmas = 8.0;

// Below this many samples the histogram is noise rather than a distribution worth fitting.
constexpr double kMinSamples = 64.0;

constexpr int kOtsuBins = 256;
constexpr int kFloorBins = 1024;

using OtsuHistogram = std::array<double, kOtsuBins>;

/**
 * Bin index for a posteriorgram value, which is a probability and so already in 0 to 1.
 */
inline int binOf(float inValue, int inNumBins)
{
    return std::min(inNumBins - 1, std::max(0, static_cast<int>(inValue * static_cast<float>(inNumBins))));
}

/**
 * Value at the median of an accumulated histogram, at bin-centre resolution.
 */
double histogramMedian(const std::vector<double>& inHistogram, double inCount)
{
    const auto half = inCount * 0.5;
    double running = 0.0;

    for (size_t bin = 0; bin < inHistogram.size(); bin++) {
        running += inHistogram[bin];

        if (running >= half) {
            return (static_cast<double>(bin) + 0.5) / static_cast<double>(inHistogram.size());
        }
    }

    return 1.0;
}

/**
 * Where a posteriorgram's noise floor ends, measured from the take rather than assumed.
 *
 * This exists because a fixed floor gets the question wrong. On real material the overwhelming
 * majority of (frame, pitch) cells are silence, and that silence does not sit at zero: on the
 * fixture in Tests/ it sits at 0.10 with a spread of 0.004, so a hardcoded floor of 0.05 excludes
 * nothing at all and every fit downstream is dominated by noise it was supposed to have discarded.
 * Median and MAD rather than mean and standard deviation, because the notes are the outliers here
 * and the whole point is to measure the floor without them dragging the estimate up.
 */
float noiseFloor(const std::vector<std::vector<float>>& inPG)
{
    std::vector<double> values(kFloorBins, 0.0);
    double count = 0.0;

    for (const auto& frame: inPG) {
        for (const auto value: frame) {
            values[static_cast<size_t>(binOf(value, kFloorBins))] += 1.0;
            count += 1.0;
        }
    }

    if (count < kMinSamples) {
        return kMinNoiseFloor;
    }

    const auto median = histogramMedian(values, count);

    std::vector<double> deviations(kFloorBins, 0.0);

    for (const auto& frame: inPG) {
        for (const auto value: frame) {
            const auto deviation = std::abs(static_cast<double>(value) - median);
            deviations[static_cast<size_t>(binOf(static_cast<float>(deviation), kFloorBins))] += 1.0;
        }
    }

    // 1.4826 scales the median absolute deviation to a standard deviation for normal data.
    const auto sigma = histogramMedian(deviations, count) * 1.4826;

    return std::max(kMinNoiseFloor, static_cast<float>(median + kFloorSigmas * sigma));
}

/**
 * Otsu's split of an accumulated histogram: the threshold maximising between-class variance.
 *
 * @param inFallback Returned when there is too little to fit anything to.
 */
float otsuFromHistogram(const OtsuHistogram& inHistogram, double inCount, float inFallback)
{
    if (inCount < kMinSamples) {
        return inFallback;
    }

    double total_mass = 0.0;

    for (int bin = 0; bin < kOtsuBins; bin++) {
        total_mass += inHistogram[static_cast<size_t>(bin)] * bin;
    }

    double background_weight = 0.0;
    double background_mass = 0.0;
    double best_variance = -1.0;
    int best_bin = 0;

    for (int bin = 0; bin < kOtsuBins; bin++) {
        background_weight += inHistogram[static_cast<size_t>(bin)];
        background_mass += inHistogram[static_cast<size_t>(bin)] * bin;

        const auto foreground_weight = inCount - background_weight;

        if (background_weight <= 0.0 || foreground_weight <= 0.0) {
            continue;
        }

        const auto background_mean = background_mass / background_weight;
        const auto foreground_mean = (total_mass - background_mass) / foreground_weight;
        const auto separation = background_mean - foreground_mean;
        const auto variance = background_weight * foreground_weight * separation * separation;

        if (variance > best_variance) {
            best_variance = variance;
            best_bin = bin;
        }
    }

    // The split sits at the top edge of the winning bin.
    return static_cast<float>(best_bin + 1) / static_cast<float>(kOtsuBins);
}

/**
 * Threshold for the note posteriorgram, fitted to the cells that carry a note.
 */
float deriveFrameThreshold(const std::vector<std::vector<float>>& inNotesPG, float inFallback, float& outFloor)
{
    const auto floor = noiseFloor(inNotesPG);
    outFloor = floor;

    OtsuHistogram histogram {};
    histogram.fill(0.0);
    double count = 0.0;

    for (const auto& frame: inNotesPG) {
        for (const auto value: frame) {
            if (!(value > floor)) {
                continue;
            }

            histogram[static_cast<size_t>(binOf(value, kOtsuBins))] += 1.0;
            count += 1.0;
        }
    }

    return otsuFromHistogram(histogram, count, inFallback);
}

/**
 * Threshold for the onset posteriorgram, fitted to its local maxima over time.
 *
 * Fitting the maxima rather than every cell is the whole point: the decoder only ever compares
 * this threshold against a value it has already established is a local maximum, so that is the
 * population the split has to separate. Fitting all cells instead answers a question nobody asked
 * and lets the mass of non-peak frames decide where the peaks get cut.
 */
float deriveOnsetThreshold(const std::vector<std::vector<float>>& inOnsetsPG, float inFallback)
{
    const auto floor = noiseFloor(inOnsetsPG);

    OtsuHistogram histogram {};
    histogram.fill(0.0);
    double count = 0.0;

    const auto n_frames = static_cast<int>(inOnsetsPG.size());

    for (int frame = 1; frame + 1 < n_frames; frame++) {
        const auto& previous = inOnsetsPG[static_cast<size_t>(frame - 1)];
        const auto& current = inOnsetsPG[static_cast<size_t>(frame)];
        const auto& next = inOnsetsPG[static_cast<size_t>(frame + 1)];

        for (size_t note_idx = 0; note_idx < current.size(); note_idx++) {
            const auto value = current[note_idx];

            if (!(value > floor) || value < previous[note_idx] || value < next[note_idx]) {
                continue;
            }

            histogram[static_cast<size_t>(binOf(value, kOtsuBins))] += 1.0;
            count += 1.0;
        }
    }

    return otsuFromHistogram(histogram, count, inFallback);
}
} // namespace

void BasicPitch::reset()
{
    mBasicPitchCNN.reset();
    mNotesCreator.clear();
    mNoteVelocity.clear();

    mDerivedFrameThreshold = 0.3f;
    mDerivedOnsetThreshold = 0.5f;
    mDerivedNoiseFloor = kMinNoiseFloor;

    mContoursPG.clear();
    mContoursPG.shrink_to_fit();
    mNotesPG.clear();
    mNotesPG.shrink_to_fit();
    mOnsetsPG.clear();
    mOnsetsPG.shrink_to_fit();
    mNoteEvents.clear();
    mNoteEvents.shrink_to_fit();

    mNumFrames = 0;
}

void BasicPitch::setParameters(float inNoteSensitivity, float inSplitSensitivity, float inMinNoteDurationMs)
{
    mNoteSensitivity = inNoteSensitivity;
    mSplitSensitivity = inSplitSensitivity;
    mMinNoteDurationMs = inMinNoteDurationMs;

    _applyParameters();
}

void BasicPitch::_deriveThresholds()
{
    // The fallbacks are basic-pitch's own defaults, which is what these controls used to sit on
    // when centred. They are what a take with nothing to fit falls back to.
    const auto frame_threshold = deriveFrameThreshold(mNotesPG, 0.3f, mDerivedNoiseFloor);
    const auto onset_threshold = deriveOnsetThreshold(mOnsetsPG, 0.5f);

    mDerivedFrameThreshold = std::min(kMaxDerivedThreshold, std::max(kMinDerivedThreshold, frame_threshold));
    mDerivedOnsetThreshold = std::min(kMaxDerivedThreshold, std::max(kMinDerivedThreshold, onset_threshold));
}

void BasicPitch::setLegacyEngine(bool inLegacy)
{
    mLegacyEngine = inLegacy;
    _applyParameters();
}

void BasicPitch::_applyParameters()
{
    if (mLegacyEngine) {
        // The old mapping, kept only so the bench has something to measure against: the knob was
        // the threshold, with no reference whatsoever to the material it was being applied to.
        mParams.frameThreshold = 1.0f - mNoteSensitivity;
        mParams.onsetThreshold = 1.0f - mSplitSensitivity;
    } else {
        // The knob offsets the derived threshold rather than setting it. At the neutral position
        // the user gets what the take says and never has to hunt for it; above neutral lowers the
        // bar and yields more notes, which is the direction the control name promises.
        const auto note_offset = (0.5f - mNoteSensitivity) * kSensitivitySpan;
        const auto split_offset = (0.5f - mSplitSensitivity) * kSensitivitySpan;

        mParams.frameThreshold = std::min(0.95f, std::max(0.05f, mDerivedFrameThreshold + note_offset));
        mParams.onsetThreshold = std::min(0.95f, std::max(0.05f, mDerivedOnsetThreshold + split_offset));
    }

    mParams.minNoteLength =
        static_cast<int>(std::round(mMinNoteDurationMs / 1000.0f / (FFT_HOP / BASIC_PITCH_SAMPLE_RATE)));

    mParams.pitchBend = MultiPitchBend;
    mParams.melodiaTrick = true;
    mParams.inferOnsets = true;

    // Everything below defaults to off in Notes so that the parity fixtures in Tests/ keep
    // checking this decoder against the Python implementation. The real pipeline wants them on.
    mParams.refineOnsetTiming = !mLegacyEngine;
    // A note is over once it has decayed to this share of its own peak posteriorgram value.
    // Swept on the bench: below about 0.4 the tail overshoots on short notes badly enough to blow
    // the offset tolerance (fast_run offset F1 0.77 -> 0.26 at 0.25), and above it the behaviour
    // saturates because the release meets the global threshold. 0.4 is the point where offset F1
    // is at its best on every case at once.
    mParams.releaseRatio = mLegacyEngine ? -1.0f : 0.4f;
    // Floored at the take's own measured noise floor rather than a constant. A note is finished
    // once it is indistinguishable from the silence around it, and where that is depends on the
    // recording, not on a number chosen here.
    mParams.releaseFloor = mDerivedNoiseFloor;
    // A neighbouring bin below half the centre's is leakage; at or above it, a second note.
    mParams.neighbourSuppressionRatio = mLegacyEngine ? -1.0f : 0.5f;
}

void BasicPitch::transcribeToMIDI(float* inAudio, int inNumSamples)
{
    // To test if downsampling works as expected
#if SAVE_DOWNSAMPLED_AUDIO
    auto file = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("Test_Downsampled.wav");

    std::unique_ptr<AudioFormatWriter> format_writer;

    format_writer.reset(WavAudioFormat().createWriterFor(new FileOutputStream(file), 22050, 1, 16, {}, 0));

    if (format_writer != nullptr) {
        AudioBuffer<float> tmp_buffer;
        tmp_buffer.setSize(1, inNumSamples);
        tmp_buffer.copyFrom(0, 0, inAudio, inNumSamples);
        format_writer->writeFromAudioSampleBuffer(tmp_buffer, 0, inNumSamples);

        format_writer->flush();

        file.revealToUser();
    }
#endif

    const float* stacked_cqt = mFeaturesCalculator.computeFeatures(inAudio, inNumSamples, mNumFrames);

    mOnsetsPG.resize(mNumFrames, std::vector<float>(static_cast<size_t>(NUM_FREQ_OUT), 0.0f));
    mNotesPG.resize(mNumFrames, std::vector<float>(static_cast<size_t>(NUM_FREQ_OUT), 0.0f));
    mContoursPG.resize(mNumFrames, std::vector<float>(static_cast<size_t>(NUM_FREQ_IN), 0.0f));

    mOnsetsPG.shrink_to_fit();
    mNotesPG.shrink_to_fit();
    mContoursPG.shrink_to_fit();

    mBasicPitchCNN.reset();

    const size_t num_lh_frames = BasicPitchCNN::getNumFramesLookahead();

    // The loops below index the posteriorgrams at [frame_idx - num_lh_frames]. On audio shorter than
    // the CNN lookahead that subtraction underflows (size_t) and writes out of bounds. Callers gate on
    // a minimum amount of audio, but don't depend on that here.
    if (mNumFrames < num_lh_frames) {
        mNoteEvents.clear();
        mNoteVelocity.clear();
        return;
    }

    std::vector<float> zero_stacked_cqt(NUM_HARMONICS * NUM_FREQ_IN, 0.0f);

    // Run the CNN with 0 input and discard output (only for num_lh_frames)
    for (size_t i = 0; i < num_lh_frames; i++) {
        mBasicPitchCNN.frameInference(zero_stacked_cqt.data(), mContoursPG[0], mNotesPG[0], mOnsetsPG[0]);
    }

    // Run the CNN with real inputs and discard outputs (only for num_lh_frames)
    for (size_t frame_idx = 0; frame_idx < num_lh_frames; frame_idx++) {
        mBasicPitchCNN.frameInference(
            stacked_cqt + frame_idx * NUM_HARMONICS * NUM_FREQ_IN, mContoursPG[0], mNotesPG[0], mOnsetsPG[0]);
    }

    // Run the CNN with real inputs and correct outputs
    for (size_t frame_idx = num_lh_frames; frame_idx < mNumFrames; frame_idx++) {
        mBasicPitchCNN.frameInference(stacked_cqt + frame_idx * NUM_HARMONICS * NUM_FREQ_IN,
                                      mContoursPG[frame_idx - num_lh_frames],
                                      mNotesPG[frame_idx - num_lh_frames],
                                      mOnsetsPG[frame_idx - num_lh_frames]);
    }

    // Run end with zeroes as input and last frames as output
    for (size_t frame_idx = mNumFrames; frame_idx < mNumFrames + num_lh_frames; frame_idx++) {
        mBasicPitchCNN.frameInference(zero_stacked_cqt.data(),
                                      mContoursPG[frame_idx - num_lh_frames],
                                      mNotesPG[frame_idx - num_lh_frames],
                                      mOnsetsPG[frame_idx - num_lh_frames]);
    }

    // Read the thresholds off this take before decoding it, then fold the knobs back in on top.
    _deriveThresholds();
    _applyParameters();

    // The stacked CQT is still live here: it points into the ONNX session's output tensor, which
    // stays valid until the next computeFeatures call. This is the only place velocity can be
    // measured without either keeping the audio around or running the transform a second time.
    mNoteVelocity.prepare(stacked_cqt, mNumFrames);

    mNoteEvents = mNotesCreator.convert(mNotesPG, mOnsetsPG, mContoursPG, mParams, true);
    mNoteVelocity.apply(mNoteEvents);
}

void BasicPitch::updateMIDI()
{
    mNoteEvents = mNotesCreator.convert(mNotesPG, mOnsetsPG, mContoursPG, mParams, false);
    mNoteVelocity.apply(mNoteEvents);
}

const std::vector<Notes::Event>& BasicPitch::getNoteEvents() const
{
    return mNoteEvents;
}
