#pragma once

#include <juce_core/juce_core.h>

// Recording what the machine is playing, on Windows, via WASAPI loopback.
//
// This exists because JUCE has none of it: as of 8.0.8, WASAPIDeviceMode is
// shared/exclusive/sharedLowLatency and the string "loopback" appears nowhere in
// juce_audio_devices. So the kit owns an IAudioClient opened with
// AUDCLNT_STREAMFLAGS_LOOPBACK, polled on its own thread.
//
// Header-only and Windows-only, both deliberate: it keeps the kit's public surface the
// shape it already is, and everything below is COM against ole32, which juce_audio_devices
// already links on Windows. AudioCapture.h is the thing you should actually use; this is
// its loopback backend, useful on its own only if you want the raw stream.
//
// Non-Windows builds get an empty header, so callers guard with JUCE_WINDOWS rather than
// with an okstudio-specific macro.

#if JUCE_WINDOWS

#include "CaptureMath.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <vector>

#if defined(_MSC_VER)
 #pragma comment(lib, "ole32.lib")
#endif

namespace okstudio::capture
{

namespace detail
{
/** A minimal owning COM pointer.

    juce::ComSmartPtr would do the job, but it is only compiled when
    JUCE_CORE_INCLUDE_COM_SMART_PTR is defined *before* juce_core.h is first included.
    A header further down the include graph cannot arrange that, and making every
    consumer set a JUCE config macro to use one kit header is a worse trade than
    twenty lines of Release(). */
template <typename ComClass>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&)            = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComClass* get() const noexcept { return ptr; }
    ComClass* operator->() const noexcept { return ptr; }
    explicit operator bool() const noexcept { return ptr != nullptr; }

    /** Releases whatever is held and hands back somewhere to write the new pointer,
        which is the shape every COM factory call wants. */
    ComClass** resetAndGetPointerAddress() noexcept
    {
        reset();
        return &ptr;
    }

    void reset() noexcept
    {
        if (ptr != nullptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

private:
    ComClass* ptr = nullptr;
};
} // namespace detail

/** Where loopback audio goes. Called from the capture thread, never the message thread. */
struct LoopbackSink
{
    virtual ~LoopbackSink() = default;

    /** Deinterleaved float, one pointer per channel, always `numChannels` of them. */
    virtual void loopbackBlock(const float* const* channels, int numChannels, int numSamples) = 0;

    /** The stream died: the endpoint was unplugged, or its format changed underneath us. */
    virtual void loopbackFailed() {}
};

/** A render endpoint whose output can be recorded. `id` is the WASAPI endpoint id, which
    survives the user renaming the device; `name` is what to put in the menu. */
struct LoopbackEndpoint
{
    juce::String id;
    juce::String name;
    bool isDefault = false;
};

/**
 * One loopback capture stream.
 *
 * start() and stop() are message thread. Everything else happens on a private thread that
 * polls IAudioCaptureClient and pushes deinterleaved float into the sink.
 *
 * Two things about loopback that shape the code below:
 *  - Nothing arrives while the endpoint is idle. A loopback stream is not a clock; if no
 *    app is playing, WASAPI produces no packets at all. Without correction, a recording
 *    made across a silent gap would splice the two halves together and be shorter than the
 *    wall-clock time it took. The device position from GetBuffer is what closes that gap.
 *  - The format is the endpoint's shared-mode mix format, take it or leave it. You cannot
 *    ask for a different sample rate; asking fails to open rather than resampling.
 */
class WasapiLoopback : private juce::Thread
{
public:
    WasapiLoopback() : juce::Thread("okstudio loopback") {}

    ~WasapiLoopback() override { stop(); }

    //==========================================================================
    /** Every active render endpoint, default first. Message thread; talks to COM. */
    static std::vector<LoopbackEndpoint> endpoints()
    {
        std::vector<LoopbackEndpoint> found;
        ComScope com;

        detail::ComPtr<IMMDeviceEnumerator> enumerator;
        if (! createEnumerator(enumerator))
            return found;

        juce::String defaultId;
        {
            detail::ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                              defaultDevice.resetAndGetPointerAddress())))
                defaultId = endpointId(defaultDevice);
        }

        detail::ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  collection.resetAndGetPointerAddress())))
            return found;

        UINT count = 0;
        if (FAILED(collection->GetCount(&count)))
            return found;

        for (UINT i = 0; i < count; ++i)
        {
            detail::ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.resetAndGetPointerAddress())))
                continue;

            LoopbackEndpoint e;
            e.id = endpointId(device);
            if (e.id.isEmpty())
                continue;

            e.name      = friendlyName(device);
            e.isDefault = e.id == defaultId;

            if (e.name.isEmpty())
                e.name = "Output " + juce::String((int) i + 1);

            found.push_back(e);
        }

        // Default first: it is what "record the computer" means to the person asking.
        std::stable_sort(found.begin(), found.end(),
                         [](const LoopbackEndpoint& a, const LoopbackEndpoint& b) { return a.isDefault > b.isDefault; });

        return found;
    }

    //==========================================================================
    /**
     * Opens `endpointId` in loopback mode and starts pushing blocks into `sinkToUse`,
     * which must outlive the capture.
     *
     * `channelCap` of 0 takes every channel the endpoint mixes; a positive value takes
     * the first N, which is how you get a stereo file off a 5.1 endpoint without a
     * downmix nobody asked for.
     */
    juce::Result start(const juce::String& endpointIdToOpen, LoopbackSink& sinkToUse, int channelCap = 0)
    {
        stop();

        ComScope com;

        detail::ComPtr<IMMDeviceEnumerator> enumerator;
        if (! createEnumerator(enumerator))
            return juce::Result::fail("Could not reach the Windows audio service");

        detail::ComPtr<IMMDevice> device;
        if (endpointIdToOpen.isNotEmpty())
        {
            if (FAILED(enumerator->GetDevice(endpointIdToOpen.toWideCharPointer(), device.resetAndGetPointerAddress())))
                device.reset(); // saved endpoint has gone: fall through to the default
        }

        if (! device
            && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.resetAndGetPointerAddress())))
            return juce::Result::fail("No playback device to record from");

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void**) client.resetAndGetPointerAddress())))
            return juce::Result::fail("Could not open that output for recording");

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
        {
            client.reset();
            return juce::Result::fail("Could not read the output's audio format");
        }

        const auto format = classify(*mix);
        const auto mixChannels = (int) mix->nChannels;
        const auto mixRate     = (double) mix->nSamplesPerSec;
        const auto frameBytes  = (int) mix->nBlockAlign;

        // A 200 ms ring is plenty: we poll at a quarter of it and this is not a
        // latency-sensitive path, only a lossless one.
        const REFERENCE_TIME bufferDuration = 2'000'000;
        const auto hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0,
                                           mix, nullptr);
        CoTaskMemFree(mix);

        if (FAILED(hr))
        {
            client.reset();
            return juce::Result::fail("Windows refused a loopback stream on that output");
        }

        if (format == SampleFormat::unsupported || mixChannels <= 0 || mixRate <= 0.0)
        {
            client.reset();
            return juce::Result::fail("That output mixes in a format we cannot record");
        }

        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**) capture.resetAndGetPointerAddress())))
        {
            client.reset();
            return juce::Result::fail("Could not start capturing that output");
        }

        UINT32 ringFrames = 0;
        client->GetBufferSize(&ringFrames);

        sampleFormat  = format;
        bytesPerFrame = frameBytes;
        mixChannelCount = mixChannels;
        channels = channelCap > 0 ? juce::jmin(channelCap, mixChannels) : mixChannels;
        rate     = mixRate;
        sink     = &sinkToUse;

        pollMs = juce::jlimit(1, 50, (int) ((double) ringFrames * 1000.0 / rate / 4.0));
        resize((int) juce::jmax((UINT32) 1024, ringFrames));

        nextExpectedPosition = 0;
        havePosition         = false;
        deviceFailed.store(false);

        if (FAILED(client->Start()))
        {
            releaseCom();
            return juce::Result::fail("Windows would not start the loopback stream");
        }

        startThread(juce::Thread::Priority::high);
        return juce::Result::ok();
    }

    /** Message thread. Stops the stream and joins the capture thread. */
    void stop()
    {
        signalThreadShouldExit();
        stopThread(2000);

        if (client)
            client->Stop();

        releaseCom();
        sink = nullptr;
    }

    double sampleRate() const noexcept { return rate; }
    int channelCount() const noexcept { return channels; }
    /** Channels the endpoint actually mixes, before `channelCap` trimmed it. */
    int mixChannels() const noexcept { return mixChannelCount; }
    bool hasFailed() const noexcept { return deviceFailed.load(); }

private:
    //==========================================================================
    /** CoInitializeEx that only uninitializes when it was the one that initialized.
        The message thread is already STA thanks to JUCE, and stamping on that would
        break every other COM user in the process. */
    struct ComScope
    {
        ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
        ~ComScope() { if (owned) CoUninitialize(); }
        const bool owned;
        JUCE_DECLARE_NON_COPYABLE(ComScope)
    };

    enum class SampleFormat { float32, int16, int32, unsupported };

    /** The shared-mode mix format is IEEE float in practice; the integer paths are here
        because "in practice" is not "always" and a wrong guess writes noise. */
    static SampleFormat classify(const WAVEFORMATEX& f)
    {
        auto tag = f.wFormatTag;

        if (tag == WAVE_FORMAT_EXTENSIBLE)
        {
            if (f.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
                return SampleFormat::unsupported;

            // KSDATAFORMAT_SUBTYPE_* are the wave format tag widened into a GUID, so the
            // tag is Data1 and we can skip including ksmedia.h for two constants.
            tag = (WORD) reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(f).SubFormat.Data1;
        }

        if (tag == WAVE_FORMAT_IEEE_FLOAT && f.wBitsPerSample == 32)
            return SampleFormat::float32;

        if (tag == WAVE_FORMAT_PCM && f.wBitsPerSample == 16)
            return SampleFormat::int16;

        if (tag == WAVE_FORMAT_PCM && f.wBitsPerSample == 32)
            return SampleFormat::int32;

        return SampleFormat::unsupported;
    }

    /** The MMDevice enumerator, or nothing. Kept in one place because both the scan and
        the open need it and neither wants the CoCreateInstance boilerplate. */
    static bool createEnumerator(detail::ComPtr<IMMDeviceEnumerator>& out)
    {
        return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator),
                                          (void**) out.resetAndGetPointerAddress()));
    }

    static juce::String endpointId(const detail::ComPtr<IMMDevice>& device)
    {
        if (! device)
            return {};

        LPWSTR raw = nullptr;
        if (FAILED(device->GetId(&raw)) || raw == nullptr)
            return {};

        juce::String id(raw);
        CoTaskMemFree(raw);
        return id;
    }

    static juce::String friendlyName(const detail::ComPtr<IMMDevice>& device)
    {
        // PKEY_Device_FriendlyName. Spelled out rather than including
        // functiondiscoverykeys_devpkey.h, which only defines the key when INITGUID is
        // set and otherwise leaves an unresolved external.
        static const PROPERTYKEY friendlyNameKey = {
            { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
        };

        detail::ComPtr<IPropertyStore> props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, props.resetAndGetPointerAddress())))
            return {};

        PROPVARIANT value;
        PropVariantInit(&value);

        juce::String name;
        if (SUCCEEDED(props->GetValue(friendlyNameKey, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            name = juce::String(value.pwszVal);

        PropVariantClear(&value);
        return name;
    }

    void releaseCom()
    {
        capture.reset();
        client.reset();
    }

    void resize(int frames)
    {
        scratch.assign((size_t) juce::jmax(1, frames) * (size_t) juce::jmax(1, channels), 0.0f);
        pointers.resize((size_t) juce::jmax(1, channels));
        scratchFrames = frames;
    }

    float* channelStart(int channel) { return scratch.data() + (size_t) channel * (size_t) scratchFrames; }

    /** Deinterleaves one packet into the scratch planes and hands it over. */
    void deliver(const BYTE* data, int frames, bool silent)
    {
        if (frames <= 0 || sink == nullptr)
            return;

        if (frames > scratchFrames)
            resize(frames);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = channelStart(ch);

            if (silent || data == nullptr)
            {
                std::fill(dest, dest + frames, 0.0f);
                continue;
            }

            const auto* frameBytes = data + (size_t) ch * (size_t) (bytesPerFrame / juce::jmax(1, mixChannelCount));

            for (int i = 0; i < frames; ++i)
            {
                const auto* s = frameBytes + (size_t) i * (size_t) bytesPerFrame;

                switch (sampleFormat)
                {
                    case SampleFormat::float32:
                        dest[i] = *reinterpret_cast<const float*>(s);
                        break;
                    case SampleFormat::int16:
                        dest[i] = (float) *reinterpret_cast<const juce::int16*>(s) / 32768.0f;
                        break;
                    case SampleFormat::int32:
                        dest[i] = (float) ((double) *reinterpret_cast<const juce::int32*>(s) / 2147483648.0);
                        break;
                    case SampleFormat::unsupported:
                    default:
                        dest[i] = 0.0f;
                        break;
                }
            }
        }

        for (int ch = 0; ch < channels; ++ch)
            pointers[(size_t) ch] = channelStart(ch);

        sink->loopbackBlock(pointers.data(), channels, frames);
    }

    /** Writes `frames` of silence, in scratch-sized chunks. See the class comment: an idle
        endpoint produces no packets, and without this the recording would lose that time. */
    void deliverSilence(juce::int64 frames)
    {
        const auto cap = (juce::int64) (rate * maxSilencePadSeconds);
        auto remaining = juce::jlimit((juce::int64) 0, cap, frames);

        while (remaining > 0)
        {
            const auto chunk = (int) juce::jmin((juce::int64) scratchFrames, remaining);
            deliver(nullptr, chunk, true);
            remaining -= chunk;
        }
    }

    //==========================================================================
    void run() override
    {
        ComScope com;

        while (! threadShouldExit())
        {
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames)))
            {
                fail();
                return;
            }

            if (packetFrames == 0)
            {
                wait(pollMs);
                continue;
            }

            while (packetFrames > 0 && ! threadShouldExit())
            {
                BYTE* data          = nullptr;
                UINT32 frames       = 0;
                DWORD flags         = 0;
                UINT64 position     = 0;

                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, nullptr)))
                {
                    fail();
                    return;
                }

                if (havePosition && position > nextExpectedPosition)
                    deliverSilence((juce::int64) (position - nextExpectedPosition));

                deliver(data, (int) frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);

                capture->ReleaseBuffer(frames);

                nextExpectedPosition = position + frames;
                havePosition         = true;

                if (FAILED(capture->GetNextPacketSize(&packetFrames)))
                {
                    fail();
                    return;
                }
            }
        }
    }

    void fail()
    {
        deviceFailed.store(true);

        if (sink != nullptr)
            sink->loopbackFailed();
    }

    //==========================================================================
    static constexpr double maxSilencePadSeconds = 30.0;

    detail::ComPtr<IAudioClient> client;
    detail::ComPtr<IAudioCaptureClient> capture;

    LoopbackSink* sink = nullptr;
    SampleFormat sampleFormat = SampleFormat::unsupported;

    double rate         = 0.0;
    int channels        = 0;
    int mixChannelCount = 0;
    int bytesPerFrame   = 0;
    int scratchFrames   = 0;
    int pollMs          = 10;

    std::vector<float> scratch;
    std::vector<const float*> pointers;

    UINT64 nextExpectedPosition = 0;
    bool havePosition           = false;
    std::atomic<bool> deviceFailed { false };

    JUCE_DECLARE_NON_COPYABLE(WasapiLoopback)
};

} // namespace okstudio::capture

#endif // JUCE_WINDOWS
