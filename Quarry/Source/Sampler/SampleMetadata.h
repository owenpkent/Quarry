#pragma once

#include <juce_core/juce_core.h>

#include "SampleMath.h"

//
// Everything known about one captured sample.
//
// Written in full to a .json sidecar beside the audio, and in a flattened human-readable
// subset into the WAV itself. The sidecar is the complete record and the thing a library
// browser reads; the embedded copy exists so the file still says what it is after being
// dragged somewhere the sidecar did not follow.
//
// No DAW reads any of it. Ableton in particular ignores ACID chunks outright, reads no
// embedded tempo or key from any format, and runs its own analysis into a .asd sidecar it
// writes next to the sample. So the embedded tags are for other tools and for the file
// being self-describing, never a channel to Live.
//
// juce_core only, so this stays testable without dragging in juce_audio_formats.
//

namespace quarry::sampler
{

struct SampleMetadata
{
    /** Bumped when the shape below changes. Versioned from day one because the two null
        blocks, analysis and classification, are going to be filled. */
    static constexpr int schemaVersion = 1;

    juce::Time capturedAt;

    struct Source
    {
        /** False when the whole endpoint was recorded rather than one application, in which
            case everything else here is inferred and should be read as such. */
        bool isolatedToProcess = true;

        juce::uint32 processId = 0;
        juce::String processName;      // "chrome.exe"
        juce::String processPath;
        juce::String windowTitle;
        juce::String url;              // browsers only, empty otherwise

        /** The app's own volume slider at the moment of capture. Anything below 1.0 is loss
            baked into the audio that no format can undo, so it is recorded rather than
            silently tolerated. */
        float sessionVolume = 1.0f;

        juce::String screenshotFile;   // filename only, beside the audio
    };

    struct Media
    {
        bool known = false;            // false when no media session matched the process
        juce::String title;
        juce::String artist;
        juce::String album;
    };

    struct Audio
    {
        double sampleRate = 0.0;
        int channels = 0;
        double durationSec = 0.0;
        Loudness loudness;
    };

    struct Trim
    {
        bool foundSound = false;
        double startSec = 0.0;
        double endSec = 0.0;
        double originalDurationSec = 0.0;
    };

    Source source;
    Media media;
    Audio audio;
    Trim trim;
    juce::StringArray tags;

    //==========================================================================
    /** The one line that goes in the WAV's BWF description, and reads as a sentence in any
        tool that shows it. */
    juce::String description() const
    {
        juce::StringArray parts;

        if (media.known && media.title.isNotEmpty())
            parts.add(media.artist.isNotEmpty() ? media.artist + " - " + media.title : media.title);
        else if (source.windowTitle.isNotEmpty())
            parts.add(source.windowTitle);

        if (source.processName.isNotEmpty())
            parts.add("captured from " + source.processName);

        if (! source.isolatedToProcess)
            parts.add("(whole system audio, source inferred)");

        return parts.joinIntoString(" ");
    }

    /** The complete record, as it goes into the sidecar. */
    juce::var toVar() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("schema", schemaVersion);
        root->setProperty("capturedAt", capturedAt.toISO8601(true));

        auto* sourceVar = new juce::DynamicObject();
        sourceVar->setProperty("isolation", source.isolatedToProcess ? "process" : "endpoint");
        sourceVar->setProperty("processId", (int) source.processId);
        sourceVar->setProperty("processName", source.processName);
        sourceVar->setProperty("processPath", source.processPath);
        sourceVar->setProperty("windowTitle", source.windowTitle);
        sourceVar->setProperty("url", source.url.isEmpty() ? juce::var() : juce::var(source.url));
        sourceVar->setProperty("sessionVolume", source.sessionVolume);
        sourceVar->setProperty("screenshot",
                               source.screenshotFile.isEmpty() ? juce::var()
                                                               : juce::var(source.screenshotFile));
        root->setProperty("source", juce::var(sourceVar));

        if (media.known)
        {
            auto* mediaVar = new juce::DynamicObject();
            mediaVar->setProperty("title", media.title);
            mediaVar->setProperty("artist", media.artist);
            mediaVar->setProperty("album", media.album);
            root->setProperty("media", juce::var(mediaVar));
        }
        else
        {
            root->setProperty("media", juce::var());
        }

        auto* audioVar = new juce::DynamicObject();
        audioVar->setProperty("format", "wav-float32");
        audioVar->setProperty("sampleRate", audio.sampleRate);
        audioVar->setProperty("channels", audio.channels);
        audioVar->setProperty("bitDepth", 32);
        audioVar->setProperty("durationSec", audio.durationSec);
        audioVar->setProperty("peakDb", audio.loudness.peakDb);
        audioVar->setProperty("truePeakDb", audio.loudness.truePeakDb);
        audioVar->setProperty("lufs", audio.loudness.lufs);
        root->setProperty("audio", juce::var(audioVar));

        auto* trimVar = new juce::DynamicObject();
        trimVar->setProperty("foundSound", trim.foundSound);
        trimVar->setProperty("startSec", trim.startSec);
        trimVar->setProperty("endSec", trim.endSec);
        trimVar->setProperty("originalDurationSec", trim.originalDurationSec);
        root->setProperty("trim", juce::var(trimVar));

        // Both deliberately empty. Analysis is never automatic, and the classifier is
        // undecided; the slots exist so neither changes the schema when it arrives.
        root->setProperty("analysis", juce::var());
        root->setProperty("classification", juce::var());

        juce::Array<juce::var> tagList;
        for (const auto& tag : tags)
            tagList.add(tag);
        root->setProperty("tags", tagList);

        return juce::var(root);
    }

    juce::String toJson() const { return juce::JSON::toString(toVar(), false); }
};

} // namespace quarry::sampler
