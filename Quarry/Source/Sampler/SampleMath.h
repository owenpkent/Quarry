#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <functional>
#include <vector>

//
// The arithmetic behind a captured sample: where the sound starts and stops, how loud it is,
// and what to call the file.
//
// Header-only and juce_core only, deliberately. The recorder that uses this reaches
// juce_audio_formats and the Windows audio stack; none of that can be linked into a test
// runner without dragging the whole plugin in. Everything here is a pure function of its
// inputs, so Tests/sampler_test.h can cover it by including this one header.
//

namespace quarry::sampler
{

/** Where the sound actually is, in samples, within a captured buffer. */
struct TrimBounds
{
    int startSample = 0;
    int endSample   = 0;   // one past the last sample to keep
    bool foundSound = false;

    int length() const noexcept { return juce::jmax(0, endSample - startSample); }
};

/** Peak, an estimate of inter-sample peak, and integrated loudness. All in dB, except that
    `lufs` is LUFS. Silence reports -144, not negative infinity, so it survives a JSON round
    trip and sorts sensibly in a library browser. */
struct Loudness
{
    float peakDb     = -144.0f;
    float truePeakDb = -144.0f;
    float lufs       = -144.0f;
};

namespace detail
{
constexpr float silenceFloorDb = -144.0f;

inline float toDb(double linear) noexcept
{
    return linear <= 1.0e-8 ? silenceFloorDb : (float) (20.0 * std::log10(linear));
}

/** One direct-form-1 biquad, which is all the K-weighting needs. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0; }

    double process(double x) noexcept
    {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/** ITU-R BS.1770-4 K-weighting, stage one: the shelving filter that stands in for the head.

    Computed from the sample rate rather than pasted at 48 kHz, because this machine's
    endpoint mixes at 44.1 and reading a 48 kHz table at 44.1 is simply the wrong filter. */
inline Biquad kWeightingShelf(double sampleRate) noexcept
{
    constexpr double f0 = 1681.974450955533;
    constexpr double gain = 3.999843853973347;
    constexpr double q = 0.7071752369554196;

    const auto k  = std::tan(juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto vh = std::pow(10.0, gain / 20.0);
    const auto vb = std::pow(vh, 0.4996667741545416);
    const auto a0 = 1.0 + k / q + k * k;

    Biquad f;
    f.b0 = (vh + vb * k / q + k * k) / a0;
    f.b1 = 2.0 * (k * k - vh) / a0;
    f.b2 = (vh - vb * k / q + k * k) / a0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

/** Stage two: the RLB high-pass that discards rumble the ear does not weigh. */
inline Biquad kWeightingHighPass(double sampleRate) noexcept
{
    constexpr double f0 = 38.13547087602444;
    constexpr double q = 0.5003270373238773;

    const auto k  = std::tan(juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto denominator = 1.0 + k / q + k * k;

    Biquad f;
    f.b0 = 1.0;
    f.b1 = -2.0;
    f.b2 = 1.0;
    f.a1 = 2.0 * (k * k - 1.0) / denominator;
    f.a2 = (1.0 - k / q + k * k) / denominator;
    return f;
}
} // namespace detail

//==============================================================================
/**
 * Where to crop a capture so it starts on the sound.
 *
 * `thresholdDb` is measured against the loudest sample in the whole capture, not against
 * full scale: a quiet recording should still be trimmed at its own edges rather than
 * treated as silence throughout. `paddingSamples` gives back a little of what was cut, so
 * an attack is never shaved.
 *
 * A capture with no sound in it at all reports foundSound false and the full range, because
 * silently returning a zero-length sample would look like a bug at every later step.
 */
inline TrimBounds findTrimBounds(const float* const* channels, int numChannels, int numSamples,
                                 float thresholdDb = -60.0f, int paddingSamples = 0)
{
    TrimBounds bounds;
    bounds.startSample = 0;
    bounds.endSample   = juce::jmax(0, numSamples);

    if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
        return bounds;

    float loudest = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            loudest = juce::jmax(loudest, std::fabs(channels[ch][i]));

    if (loudest <= 0.0f)
        return bounds;

    const auto threshold = loudest * std::pow(10.0f, thresholdDb / 20.0f);

    int first = -1;
    int last  = -1;

    for (int i = 0; i < numSamples && first < 0; ++i)
        for (int ch = 0; ch < numChannels; ++ch)
            if (std::fabs(channels[ch][i]) >= threshold)
            {
                first = i;
                break;
            }

    for (int i = numSamples - 1; i >= 0 && last < 0; --i)
        for (int ch = 0; ch < numChannels; ++ch)
            if (std::fabs(channels[ch][i]) >= threshold)
            {
                last = i;
                break;
            }

    if (first < 0 || last < first)
        return bounds;

    bounds.startSample = juce::jmax(0, first - paddingSamples);
    bounds.endSample   = juce::jmin(numSamples, last + 1 + paddingSamples);
    bounds.foundSound  = true;
    return bounds;
}

//==============================================================================
/**
 * Peak, inter-sample peak and integrated loudness, measured and never applied.
 *
 * `truePeakDb` is an estimate: 4x Catmull-Rom interpolation rather than the 48-tap
 * polyphase filter BS.1770 specifies. It lands within a few tenths of a dB, which is the
 * right investment for a field a library browser sorts by. It is not a certified meter and
 * this comment is here so nobody later mistakes it for one.
 *
 * `lufs` is the real thing: K-weighting computed for this sample rate, 400 ms blocks at 75%
 * overlap, absolute gate at -70 LUFS and the relative gate 10 LU below the ungated mean.
 */
inline Loudness measureLoudness(const float* const* channels, int numChannels, int numSamples,
                                double sampleRate)
{
    Loudness result;

    if (channels == nullptr || numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
        return result;

    // Sample peak, and the interpolated peak between samples.
    double peak = 0.0;
    double truePeak = 0.0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto* data = channels[ch];

        for (int i = 0; i < numSamples; ++i)
        {
            peak = juce::jmax(peak, (double) std::fabs(data[i]));

            const auto p0 = (double) data[juce::jmax(0, i - 1)];
            const auto p1 = (double) data[i];
            const auto p2 = (double) data[juce::jmin(numSamples - 1, i + 1)];
            const auto p3 = (double) data[juce::jmin(numSamples - 1, i + 2)];

            for (int step = 1; step < 4; ++step)
            {
                const auto t = (double) step * 0.25;
                const auto interpolated = 0.5 * ((2.0 * p1)
                                                 + (-p0 + p2) * t
                                                 + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t
                                                 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
                truePeak = juce::jmax(truePeak, std::fabs(interpolated));
            }
        }
    }

    result.peakDb     = detail::toDb(peak);
    result.truePeakDb = detail::toDb(juce::jmax(peak, truePeak));

    //==========================================================================
    // Integrated loudness.
    const auto blockSamples = (int) std::llround(sampleRate * 0.4);
    const auto stepSamples  = juce::jmax(1, (int) std::llround(sampleRate * 0.1));

    if (blockSamples <= 0 || numSamples < blockSamples)
        return result; // Shorter than one gating block: there is no integrated value to give.

    // K-weight every channel once, into its own buffer.
    std::vector<std::vector<double>> weighted((size_t) numChannels);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto shelf    = detail::kWeightingShelf(sampleRate);
        auto highPass = detail::kWeightingHighPass(sampleRate);

        auto& out = weighted[(size_t) ch];
        out.resize((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = highPass.process(shelf.process((double) channels[ch][i]));
    }

    // Channel weights: 1.0 for left, right and centre. Surround would be 1.41, and nothing
    // here produces surround, so anything past the first few channels is weighted 1.0 too
    // rather than guessing at a layout we were never told.
    const auto weightFor = [](int) { return 1.0; };

    std::vector<double> blockLoudness;
    std::vector<double> blockPower;

    for (int start = 0; start + blockSamples <= numSamples; start += stepSamples)
    {
        double summed = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            double meanSquare = 0.0;
            const auto& data = weighted[(size_t) ch];

            for (int i = start; i < start + blockSamples; ++i)
                meanSquare += data[(size_t) i] * data[(size_t) i];

            meanSquare /= (double) blockSamples;
            summed += weightFor(ch) * meanSquare;
        }

        if (summed <= 0.0)
            continue;

        blockPower.push_back(summed);
        blockLoudness.push_back(-0.691 + 10.0 * std::log10(summed));
    }

    if (blockPower.empty())
        return result;

    // Absolute gate, then the relative gate 10 LU below what survived it.
    const auto meanPowerAbove = [&](double thresholdLoudness) -> double
    {
        double total = 0.0;
        int counted = 0;

        for (size_t i = 0; i < blockPower.size(); ++i)
        {
            if (blockLoudness[i] > thresholdLoudness)
            {
                total += blockPower[i];
                ++counted;
            }
        }

        return counted > 0 ? total / (double) counted : 0.0;
    };

    const auto absoluteGated = meanPowerAbove(-70.0);

    if (absoluteGated <= 0.0)
        return result;

    const auto relativeThreshold = -0.691 + 10.0 * std::log10(absoluteGated) - 10.0;
    const auto finalPower = meanPowerAbove(juce::jmax(-70.0, relativeThreshold));

    if (finalPower > 0.0)
        result.lufs = (float) (-0.691 + 10.0 * std::log10(finalPower));

    return result;
}

//==============================================================================
/**
 * Text reduced to something safe on every filesystem: lower case, ASCII, words joined by
 * single hyphens, length capped.
 *
 * Capping happens on a word boundary where one is close enough, because a title cut
 * mid-word reads like corruption rather than like a shortened name.
 */
inline juce::String slugify(const juce::String& text, int maxLength = 48)
{
    juce::String out;
    bool pendingSeparator = false;

    for (auto character : text)
    {
        const auto lower = juce::CharacterFunctions::toLowerCase(character);

        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9'))
        {
            if (pendingSeparator && out.isNotEmpty())
                out += '-';

            pendingSeparator = false;
            out += lower;
        }
        else
        {
            pendingSeparator = true;
        }
    }

    if (maxLength > 0 && out.length() > maxLength)
    {
        out = out.substring(0, maxLength);

        // Prefer the last whole word, unless that throws away most of the name.
        const auto lastHyphen = out.lastIndexOfChar('-');
        if (lastHyphen > maxLength / 2)
            out = out.substring(0, lastHyphen);

        out = out.trimCharactersAtEnd("-");
    }

    return out;
}

/**
 * A window title with the noise the application put in it taken back out.
 *
 * Browsers pad their titles at both ends. A real capture came back as
 * "(413) silence - YouTube - Google Chrome", of which the useful part is two words: the
 * leading count is unread notifications, and the trailing segment names the browser we
 * already recorded the name of.
 *
 * The trailing rule is general rather than a list of browsers: a segment goes only when it
 * names the source itself, so "silence - YouTube" keeps YouTube, and Spotify or a game does
 * the same thing without being enumerated here.
 */
inline juce::String tidyWindowTitle(const juce::String& title, const juce::String& sourceName)
{
    auto out = title.trim();

    // A leading "(413)" is a notification count, not part of what is playing.
    if (out.startsWithChar('('))
    {
        const auto close = out.indexOfChar(')');

        if (close > 1 && out.substring(1, close).containsOnly("0123456789"))
            out = out.substring(close + 1).trim();
    }

    const auto sourceSlug = slugify(sourceName.upToLastOccurrenceOf(".exe", false, true), 24);

    if (sourceSlug.isEmpty())
        return out;

    // Titles separate with a hyphen, sometimes an en or em dash. Peel trailing segments off
    // for as long as they are naming the application rather than the thing being played.
    for (;;)
    {
        auto cut = out.lastIndexOf(" - ");
        auto width = 3;

        for (const auto* dash : { " \xe2\x80\x93 ", " \xe2\x80\x94 " }) // en dash, em dash
        {
            const auto found = out.lastIndexOf(juce::String::fromUTF8(dash));

            if (found > cut)
            {
                cut = found;
                width = juce::String::fromUTF8(dash).length();
            }
        }

        if (cut <= 0)
            break;

        const auto tail = out.substring(cut + width).trim();

        if (! slugify(tail, 64).contains(sourceSlug))
            break;

        out = out.substring(0, cut).trim();
    }

    return out;
}

/** The folders a capture lands in, date first: "2026-08/2026-08-16". */
inline juce::String dateFolder(juce::Time when)
{
    return when.formatted("%Y-%m") + "/" + when.formatted("%Y-%m-%d");
}

/**
 * The filename, without extension: "143052-chrome-never-gonna-give-you-up".
 *
 * Time first so a folder sorts chronologically, then the source the date-first layout
 * dropped, then whatever the thing called itself. Any part that is missing is skipped
 * rather than left as an empty gap, so a capture with no title is still a sensible name.
 */
inline juce::String sampleStem(juce::Time when, const juce::String& sourceName, const juce::String& title)
{
    juce::StringArray parts;
    parts.add(when.formatted("%H%M%S"));

    // "chrome.exe" is the source's name, but ".exe" says nothing in a filename.
    const auto source = slugify(sourceName.upToLastOccurrenceOf(".exe", false, true), 24);
    if (source.isNotEmpty())
        parts.add(source);

    const auto titleSlug = slugify(tidyWindowTitle(title, sourceName), 48);
    if (titleSlug.isNotEmpty())
        parts.add(titleSlug);

    return parts.joinIntoString("-");
}

/** `stem`, or `stem-02`, or the first number after that which is not taken. Nothing a
    capture writes ever replaces something already on disk. */
inline juce::String uniqueStem(const juce::String& stem,
                               const std::function<bool(const juce::String&)>& isTaken)
{
    if (! isTaken(stem))
        return stem;

    for (int suffix = 2; suffix < 1000; ++suffix)
    {
        const auto candidate = stem + "-" + juce::String(suffix).paddedLeft('0', 2);

        if (! isTaken(candidate))
            return candidate;
    }

    return stem + "-" + juce::String(juce::Time::getHighResolutionTicks());
}

} // namespace quarry::sampler
