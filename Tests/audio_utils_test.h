//
// The checks that stand between an audio file's own header and an allocation. A header is
// input from whoever made the file, so the numbers in it are limited before they size a buffer.
//

#ifndef QUARRY_AUDIO_UTILS_TEST_H
#define QUARRY_AUDIO_UTILS_TEST_H

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "AudioUtils.h"

namespace audio_utils_test_utils
{
static int failures = 0;

static void expect(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}

static void writeLittleEndian(juce::MemoryOutputStream& out, juce::uint32 value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.writeByte(static_cast<char>((value >> (8 * i)) & 0xff));
}
} // namespace audio_utils_test_utils

inline bool audio_utils_test()
{
    using namespace audio_utils_test_utils;
    failures = 0;

    expect(AudioUtils::isSaneAudio(2, 44100, 44100.0), "ordinary stereo accepted");
    expect(!AudioUtils::isSaneAudio(0, 44100, 44100.0), "zero channels rejected");
    expect(!AudioUtils::isSaneAudio(65, 44100, 44100.0), "too many channels rejected");
    expect(!AudioUtils::isSaneAudio(2, 0, 44100.0), "zero length rejected");
    expect(!AudioUtils::isSaneAudio(2, -5, 44100.0), "negative length rejected");
    expect(!AudioUtils::isSaneAudio(2, 44100, 0.0), "zero sample rate rejected");
    expect(!AudioUtils::isSaneAudio(2, 44100, 1.0e9), "huge sample rate rejected");
    expect(!AudioUtils::isSaneAudio(2, 44100, std::numeric_limits<double>::quiet_NaN()), "NaN sample rate rejected");
    expect(!AudioUtils::isSaneAudio(2, 0xFFFFFFFFll, 44100.0), "4 Gi frames rejected");
    expect(!AudioUtils::isSaneAudio(2, 0x7FFFFFFFll, 44100.0), "length times channels past an int rejected");
    expect(AudioUtils::isSaneAudio(1, 0x7FFFFFFFll, 44100.0), "the largest mono length is accepted");

    // A 44 byte header that claims a gigabyte of data and carries none.
    {
        juce::TemporaryFile temp(".wav");
        juce::MemoryOutputStream header;
        header.write("RIFF", 4);
        writeLittleEndian(header, 0x7FFFFFF0u, 4);
        header.write("WAVEfmt ", 8);
        writeLittleEndian(header, 16, 4);
        writeLittleEndian(header, 1, 2);
        writeLittleEndian(header, 2, 2);
        writeLittleEndian(header, 44100, 4);
        writeLittleEndian(header, 44100 * 4, 4);
        writeLittleEndian(header, 4, 2);
        writeLittleEndian(header, 16, 2);
        header.write("data", 4);
        writeLittleEndian(header, 0x7FFFFFC0u, 4);

        expect(temp.getFile().replaceWithData(header.getData(), header.getDataSize()), "wrote test wav");

        juce::AudioBuffer<float> buffer;
        double rate = 0.0;
        expect(!AudioUtils::loadAudioFile(temp.getFile(), buffer, rate), "wav claiming more than it holds rejected");
    }

    // Not an mp3 at all: no frame to decode, so no channel count to divide by.
    {
        juce::TemporaryFile temp(".mp3");
        const char junk[64] = {};
        expect(temp.getFile().replaceWithData(junk, sizeof(junk)), "wrote test mp3");

        juce::AudioBuffer<float> buffer;
        double rate = 0.0;
        expect(!AudioUtils::loadAudioFile(temp.getFile(), buffer, rate), "undecodable mp3 rejected");
    }

    if (failures == 0)
        std::cout << "audio_utils_test passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_AUDIO_UTILS_TEST_H
