//
// Created by Damien Ronssin on 09.03.23.
//

#include "AudioUtils.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

#include <limits>
#include <new>

namespace AudioUtils
{
bool isSaneAudio(int numChannels, juce::int64 numSamples, double sampleRate)
{
    // A float is four bytes and AudioBuffer indexes with int, so the whole buffer has to stay
    // inside what an int can count: more than this would not allocate, or would wrap.
    constexpr juce::int64 max_total_samples = (std::numeric_limits<int>::max)();

    if (numChannels <= 0 || numChannels > kMaxChannels || numSamples <= 0)
        return false;

    // Written so that NaN fails the comparisons rather than slipping past them.
    if (!(sampleRate >= kMinSampleRate && sampleRate <= kMaxSampleRate))
        return false;

    return numSamples <= max_total_samples / numChannels;
}

bool loadAudioFile(const juce::File& inFile, AudioBuffer<float>& outBuffer, double& outSampleRate)
{
    if (inFile.getFileExtension() == ".mp3") {
        return _loadMP3File(inFile.getFullPathName().toStdString(), outBuffer, outSampleRate);
    }

    // Register different audio formats
    auto audio_format_manager = createAudioFormatManager();

    std::unique_ptr<juce::AudioFormatReader> format_reader;
    format_reader.reset(audio_format_manager->createReaderFor(inFile));

    // Verify format reader is not null
    if (!format_reader)
        return false;

    const juce::int64 length_in_samples = format_reader->lengthInSamples;
    const int num_channels = (int) format_reader->numChannels;

    // The length is whatever the header says, and JUCE does not check it against the stream:
    // a 50 byte wav can claim four gigabytes.
    if (!isSaneAudio(num_channels, length_in_samples, format_reader->sampleRate))
        return false;

    // For formats that store raw samples the file cannot hold more than its own size, which
    // catches a header that claims far more than the file contains. Compressed formats have no
    // such relation, so they only get the limits above.
    const auto format_name = format_reader->getFormatName();
    const bool is_uncompressed = format_name == "WAV file" || format_name == "AIFF file";

    if (is_uncompressed && format_reader->bitsPerSample > 0) {
        const juce::int64 bytes_per_sample = (juce::int64) ((format_reader->bitsPerSample + 7) / 8);

        if (length_in_samples * num_channels * bytes_per_sample > inFile.getSize())
            return false;
    }

    outSampleRate = format_reader->sampleRate;
    const int num_source_samples = (int) length_in_samples;

    try {
        outBuffer.setSize(num_channels, num_source_samples);
    } catch (const std::bad_alloc&) {
        return false;
    }

    // Read source file. If not successful, return false
    if (!format_reader->read(&outBuffer, 0, num_source_samples, 0, true, true))
        return false;

    return true;
}

StringArray getSupportedAudioFileExtensions()
{
    StringArray supported_extensions;
    supported_extensions.add(".mp3");

    auto audio_format_manager = createAudioFormatManager();
    for (auto& format: *audio_format_manager) {
        StringArray file_extensions = format->getFileExtensions();
        for (auto& extension: file_extensions) {
            supported_extensions.add(extension);
        }
    }

    return supported_extensions;
}

std::unique_ptr<AudioFormatManager> createAudioFormatManager()
{
    auto audio_format_manager = std::make_unique<AudioFormatManager>();
    audio_format_manager->registerFormat(new juce::WavAudioFormat, true);
    audio_format_manager->registerFormat(new juce::AiffAudioFormat, false);
    audio_format_manager->registerFormat(new juce::FlacAudioFormat, false);
    audio_format_manager->registerFormat(new juce::OggVorbisAudioFormat, false);

    return std::move(audio_format_manager);
}

void downmixToMono(AudioBuffer<float>& ioBuffer)
{
    const auto num_channels = ioBuffer.getNumChannels();

    if (num_channels <= 1) {
        return;
    }

    const auto num_samples = ioBuffer.getNumSamples();

    for (int ch = 1; ch < num_channels; ch++) {
        ioBuffer.addFrom(0, 0, ioBuffer, ch, 0, num_samples);
    }

    ioBuffer.applyGain(0, 0, num_samples, 1.0f / static_cast<float>(num_channels));
    ioBuffer.setSize(1, num_samples, true, true, true);
}

void resampleBuffer(const AudioBuffer<float>& inBuffer,
                    AudioBuffer<float>& outBuffer,
                    double inSourceSampleRate,
                    double inTargetSampleRate)
{
    if (inSourceSampleRate == inTargetSampleRate) {
        outBuffer.makeCopyOf(inBuffer);
        return;
    }

    Resampler resampler;
    // Prepare resampler
    resampler.prepareToPlay(inSourceSampleRate, inBuffer.getNumSamples(), inTargetSampleRate);
    auto num_expected_samples_after_resample = resampler.getNumOutSamplesOnNextProcessBlock(inBuffer.getNumSamples());

    outBuffer.setSize(inBuffer.getNumChannels(), num_expected_samples_after_resample);

    for (int ch = 0; ch < inBuffer.getNumChannels(); ch++) {
        resampler.reset();
        int num_samples_after_resample = resampler.processBlock(
            inBuffer.getReadPointer(ch), outBuffer.getWritePointer(ch), inBuffer.getNumSamples());
        jassertquiet(num_samples_after_resample == num_expected_samples_after_resample);
    }
}

bool _loadMP3File(const std::string& filename, juce::AudioBuffer<float>& outBuffer, double& outSampleRate)
{
    mp3dec_t mp3d;
    mp3dec_file_info_t info {}; // zeroed: on an open failure the decoder never writes it, and buffer is freed below
    int loadResult = mp3dec_load(&mp3d, filename.c_str(), &info, nullptr, nullptr);

    // A zero result can still come with no channels: no decodable frame, or a switch between
    // mono and stereo part way through, which the decoder reports that way. Dividing by it
    // would take the host down, so everything unusable is turned away here, and the buffer is
    // freed on every path (free(nullptr) is fine).
    if (loadResult != 0 || info.channels <= 0 || info.samples == 0 || info.hz <= 0
        || info.samples > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        free(info.buffer);
        return false;
    }

    const auto num_frames = static_cast<juce::int64>(info.samples) / info.channels;

    if (!isSaneAudio(info.channels, num_frames, static_cast<double>(info.hz))) {
        free(info.buffer);
        return false;
    }

    try {
        outBuffer.setSize(info.channels, static_cast<int>(num_frames));
    } catch (const std::bad_alloc&) {
        free(info.buffer);
        return false;
    }

    for (size_t i = 0; i < info.samples; ++i) {
        size_t channel = i % static_cast<size_t>(info.channels);
        outBuffer.setSample((int) channel, static_cast<int>(i / static_cast<size_t>(info.channels)),
                            (float) info.buffer[i] / 32768.0f);
    }

    outSampleRate = static_cast<double>(info.hz);
    free(info.buffer);
    return true;
}

} // namespace AudioUtils