#pragma once

#include <juce_core/juce_core.h>

// Recording one application's audio, on Windows, via WASAPI process loopback.
//
// WasapiLoopback.h records an endpoint: everything the machine mixes, including the
// notification you did not want. This records a single process and its children, so what
// lands in the buffer is one app and nothing else. That is what makes a captured sample
// worth labelling: the source is known rather than inferred.
//
// Windows 10 build 20348 and later. Older builds get a clear failure from start() rather
// than silence, and isSupported() answers before you offer it in a menu.
//
// Header-only and Windows-only for the same reasons as WasapiLoopback.h, and it reuses that
// header's LoopbackSink, detail::ComPtr and detail::ComScope rather than growing its own.
//
// Measured behaviour that shaped the code below (Quarry sampler spike, 2026-08-16):
//  - PROCESS_LOOPBACK_MODE has exactly two values, INCLUDE_TARGET_PROCESS_TREE and
//    EXCLUDE_TARGET_PROCESS_TREE. There is no way to capture one process without its
//    children, so the tree is the only unit of inclusion there is. EXCLUDE is a different
//    feature, not a fallback: it records the machine *except* that tree.
//  - Unlike endpoint loopback, this stream is a clock. A target that plays nothing still
//    delivers zero-filled packets at the requested rate, so the gap correction below is
//    insurance that was never observed to fire.
//  - The format is caller-specified and Windows converts rather than refusing. Asking for
//    48 kHz on a 44.1 kHz endpoint opens and delivers 48 kHz. Ask for the endpoint's own
//    mix format and nothing is converted at all.
//  - The capture is post-session-volume: an app at 50% is captured 6 dB down, permanently.
//    sessions() reports each volume so a UI can say so before a take is wasted.

#if JUCE_WINDOWS

#include "WasapiLoopback.h"

#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#if defined(_MSC_VER)
 #pragma comment(lib, "mmdevapi.lib")
#endif

namespace okstudio::capture
{

namespace detail
{
/** Completion handler for ActivateAudioInterfaceAsync.

    Hand-rolled rather than WRL's RuntimeClass, to keep the kit's dependencies where they
    are: this is thirty lines and WRL is a whole header tree.

    IAgileObject is the load-bearing part. The completion callback arrives on an MTA thread
    that is not the one that made the call, and without the agility marker COM would try to
    marshal back to the caller's apartment. Everything here already runs on the capture
    thread inside a ComScope, so there is nothing to marshal to.

    It owns its own event rather than borrowing the caller's, because a timed-out activation
    still completes eventually: COM holds a reference and fires into an object that must
    still be alive and must not touch anything the caller has since destroyed. */
class ProcessLoopbackActivation : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ProcessLoopbackActivation() : finished(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    //==========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *object = static_cast<IAgileObject*>(this);
        else
        {
            *object = nullptr;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references;

        if (remaining == 0)
            delete this;

        return (ULONG) remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activation = E_UNEXPECTED;
        IUnknown* raw      = nullptr;

        const auto hr = operation->GetActivateResult(&activation, &raw);
        result = FAILED(hr) ? hr : activation;

        if (SUCCEEDED(result) && raw != nullptr)
            raw->QueryInterface(__uuidof(IAudioClient), (void**) client.resetAndGetPointerAddress());

        if (raw != nullptr)
            raw->Release();

        SetEvent(finished);
        return S_OK;
    }

    /** Blocks until the async activation lands, or gives up. */
    bool waitFor(DWORD milliseconds) const
    {
        return finished != nullptr && WaitForSingleObject(finished, milliseconds) == WAIT_OBJECT_0;
    }

    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> client;

private:
    ~ProcessLoopbackActivation()
    {
        if (finished != nullptr)
            CloseHandle(finished);
    }

    HANDLE finished = nullptr;
    std::atomic<long> references { 1 };

    JUCE_DECLARE_NON_COPYABLE(ProcessLoopbackActivation)
};
} // namespace detail

/** One application with an audio session on the default playback endpoint.

    `volume` is the app's own slider, the one in Windows' volume mixer, and it is baked into
    anything captured from that app. Anything below 1.0 is loss no format can undo. */
struct AudioSession
{
    juce::uint32 processId = 0;
    juce::String processName;    // "chrome.exe"
    juce::String executablePath;
    float volume = 1.0f;
    float peak   = 0.0f;         // right now, for a meter

    /** The session is in AudioSessionStateActive. Do not read this as "making sound":
        Premiere, Resolve and a wallpaper engine all sit here at a peak of exactly zero,
        holding a stream open against the moment they need it. A picker that offers "what
        is playing" has to rank on `peak`, which is why sessions() sorts by it. */
    bool isPlaying = false;
};

/**
 * One process-loopback capture stream.
 *
 * start() and stop() are message thread; everything else, COM included, happens on a
 * private thread. That is deliberate and differs from WasapiLoopback: activation here is
 * asynchronous, so doing it on the caller's thread would mean blocking the message thread
 * on a COM callback. Instead the capture thread initialises COM, activates, and reports
 * back through a WaitableEvent, so no apartment question ever reaches the caller.
 */
class WasapiProcessLoopback : private juce::Thread
{
public:
    /** Which side of the target tree to record. Not two ways of saying the same thing:
        see the note at the top of this header. */
    enum class Scope
    {
        targetTree,     // the target process and its children, and nothing else
        everythingElse  // the whole machine except the target process and its children
    };

    WasapiProcessLoopback() : juce::Thread("okstudio process loopback") {}

    ~WasapiProcessLoopback() override { stop(); }

    //==========================================================================
    /** Whether this Windows can do process loopback at all. Check before offering it. */
    static bool isSupported()
    {
        // GetVersionEx reports Windows 8 without an application manifest that says
        // otherwise, which would take this feature away from every machine that has it.
        // RtlGetVersion is not shimmed and tells the truth.
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

        auto* ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return false;

        auto* rtlGetVersion = (RtlGetVersionFn) (void*) GetProcAddress(ntdll, "RtlGetVersion");
        if (rtlGetVersion == nullptr)
            return false;

        OSVERSIONINFOW info {};
        info.dwOSVersionInfoSize = sizeof(info);

        if (rtlGetVersion(&info) != 0)
            return false;

        return info.dwMajorVersion > 10
            || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 20348);
    }

    /** Everything with a session on the default playback endpoint, loudest first among
        those actually playing. Message thread; talks to COM. */
    static std::vector<AudioSession> sessions()
    {
        std::vector<AudioSession> found;
        detail::ComScope com;

        detail::ComPtr<IAudioSessionEnumerator> enumerator;
        if (! sessionEnumerator(enumerator))
            return found;

        int count = 0;
        if (FAILED(enumerator->GetCount(&count)))
            return found;

        for (int i = 0; i < count; ++i)
        {
            detail::ComPtr<IAudioSessionControl> control;
            if (FAILED(enumerator->GetSession(i, control.resetAndGetPointerAddress())))
                continue;

            detail::ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                               (void**) control2.resetAndGetPointerAddress())))
                continue;

            AudioSession session;

            DWORD pid = 0;
            control2->GetProcessId(&pid);
            session.processId = (juce::uint32) pid;

            session.executablePath = executablePath(pid);
            session.processName    = session.executablePath.fromLastOccurrenceOf("\\", false, false);

            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(control->GetState(&state)))
                session.isPlaying = state == AudioSessionStateActive;

            detail::ComPtr<ISimpleAudioVolume> volume;
            if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                  (void**) volume.resetAndGetPointerAddress())))
                volume->GetMasterVolume(&session.volume);

            detail::ComPtr<IAudioMeterInformation> meter;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioMeterInformation),
                                                  (void**) meter.resetAndGetPointerAddress())))
                meter->GetPeakValue(&session.peak);

            if (session.processName.isEmpty())
                session.processName = pid == 0 ? "System" : ("Process " + juce::String((int) pid));

            found.push_back(session);
        }

        // What is making noise right now, first: that is what someone came here to record.
        std::stable_sort(found.begin(), found.end(), [](const AudioSession& a, const AudioSession& b)
        {
            if (a.isPlaying != b.isPlaying)
                return a.isPlaying;

            return a.peak > b.peak;
        });

        return found;
    }

    /** That app's own volume slider, or -1 if it has no session. */
    static float sessionVolume(juce::uint32 processId)
    {
        float found = -1.0f;
        withSessionVolume(processId, [&found](ISimpleAudioVolume& volume) { volume.GetMasterVolume(&found); });
        return found;
    }

    /** Sets that app's own volume slider. The one honest fix for a quiet capture. */
    static bool setSessionVolume(juce::uint32 processId, float newVolume)
    {
        const auto clamped = juce::jlimit(0.0f, 1.0f, newVolume);
        return withSessionVolume(processId,
                                 [clamped](ISimpleAudioVolume& volume) { volume.SetMasterVolume(clamped, nullptr); });
    }

    //==========================================================================
    /**
     * Records `targetProcessId` (and its children) into `sinkToUse`, which must outlive
     * the capture.
     *
     * `channelCap` of 0 takes every channel the endpoint mixes; a positive value takes the
     * first N, so a stereo file off a 5.1 endpoint costs no downmix nobody asked for.
     */
    juce::Result start(juce::uint32 targetProcessId, LoopbackSink& sinkToUse,
                       Scope scopeToUse = Scope::targetTree, int channelCap = 0)
    {
        stop();

        if (! isSupported())
            return juce::Result::fail("This version of Windows cannot record a single application");

        if (targetProcessId == 0)
            return juce::Result::fail("No application chosen to record");

        targetPid   = targetProcessId;
        scope       = scopeToUse;
        requestedChannelCap = channelCap;
        sink.store(&sinkToUse);
        startupError = {};
        deviceFailed.store(false);
        startupComplete.reset();

        startThread(juce::Thread::Priority::high);

        // The thread does the COM work and reports back. Long enough that an activation is
        // never cut short, short enough that a wedged audio service is a message rather
        // than a hang. Every wait the capture thread performs is derived from this same
        // budget, so start() cannot give up while that thread is still inside one.
        if (! startupComplete.wait(startupBudgetMs))
        {
            stop();
            return juce::Result::fail("Windows did not answer in time");
        }

        if (startupError.isNotEmpty())
        {
            const auto message = startupError;
            stop();
            return juce::Result::fail(message);
        }

        return juce::Result::ok();
    }

    /** Message thread. Stops the stream and joins the capture thread. */
    void stop()
    {
        signalThreadShouldExit();
        stopThread(teardownWaitMs);
        sink.store(nullptr);
    }

    double sampleRate() const noexcept { return rate; }
    int channelCount() const noexcept { return channels; }
    /** Channels the endpoint mixes, before `channelCap` trimmed it. */
    int mixChannels() const noexcept { return mixChannelCount; }
    bool hasFailed() const noexcept { return deviceFailed.load(); }

private:
    //==========================================================================
    /** The session enumerator for the default playback endpoint. */
    static bool sessionEnumerator(detail::ComPtr<IAudioSessionEnumerator>& out)
    {
        detail::ComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    (void**) devices.resetAndGetPointerAddress())))
            return false;

        detail::ComPtr<IMMDevice> endpoint;
        if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.resetAndGetPointerAddress())))
            return false;

        detail::ComPtr<IAudioSessionManager2> manager;
        if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                      (void**) manager.resetAndGetPointerAddress())))
            return false;

        return SUCCEEDED(manager->GetSessionEnumerator(out.resetAndGetPointerAddress()));
    }

    /** Runs `apply` against the volume of whichever session belongs to `processId`. */
    template <typename Apply>
    static bool withSessionVolume(juce::uint32 processId, Apply&& apply)
    {
        detail::ComScope com;

        detail::ComPtr<IAudioSessionEnumerator> enumerator;
        if (! sessionEnumerator(enumerator))
            return false;

        int count = 0;
        if (FAILED(enumerator->GetCount(&count)))
            return false;

        for (int i = 0; i < count; ++i)
        {
            detail::ComPtr<IAudioSessionControl> control;
            if (FAILED(enumerator->GetSession(i, control.resetAndGetPointerAddress())))
                continue;

            detail::ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                               (void**) control2.resetAndGetPointerAddress())))
                continue;

            DWORD pid = 0;
            if (FAILED(control2->GetProcessId(&pid)) || (juce::uint32) pid != processId)
                continue;

            detail::ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                               (void**) volume.resetAndGetPointerAddress())))
                return false;

            apply(*volume.get());
            return true;
        }

        return false;
    }

    static juce::String executablePath(DWORD pid)
    {
        if (pid == 0)
            return {};

        auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
            return {};

        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
        CloseHandle(process);

        return ok ? juce::String(path) : juce::String();
    }

    /** The default endpoint's mix format, which is what we then ask process loopback for so
        that Windows has nothing to convert. */
    static bool defaultMixFormat(int& channelsOut, double& rateOut)
    {
        detail::ComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    (void**) devices.resetAndGetPointerAddress())))
            return false;

        detail::ComPtr<IMMDevice> endpoint;
        if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.resetAndGetPointerAddress())))
            return false;

        detail::ComPtr<IAudioClient> probe;
        if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      (void**) probe.resetAndGetPointerAddress())))
            return false;

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(probe->GetMixFormat(&mix)) || mix == nullptr)
            return false;

        channelsOut = (int) mix->nChannels;
        rateOut     = (double) mix->nSamplesPerSec;
        CoTaskMemFree(mix);

        return channelsOut > 0 && rateOut > 0.0;
    }

    //==========================================================================
    /** Activation, initialisation and the capture loop, all on this thread so that COM is
        initialised, used and torn down in one place. */
    void run() override
    {
        // Stamped before any COM call, so every wait below spends the same budget start()
        // is counting down.
        startupDeadline = juce::Time::getMillisecondCounterHiRes() + (double) startupBudgetMs;

        detail::ComScope com;

        detail::ComPtr<IAudioClient> client;
        detail::ComPtr<IAudioCaptureClient> capture;
        HANDLE ready = nullptr;

        const auto giveUp = [&](const juce::String& why)
        {
            startupError = why;
            startupComplete.signal();

            if (ready != nullptr)
                CloseHandle(ready);
        };

        int mixChannelsFound = 0;
        double mixRate = 0.0;
        if (! defaultMixFormat(mixChannelsFound, mixRate))
            return giveUp("Could not read this computer's audio format");

        if (! activate(client))
            return giveUp("Windows would not let us record that application");

        WAVEFORMATEX format {};
        format.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels       = (WORD) mixChannelsFound;
        format.nSamplesPerSec  = (DWORD) mixRate;
        format.wBitsPerSample  = 32;
        format.nBlockAlign     = (WORD) (mixChannelsFound * 4);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        format.cbSize          = 0;

        // 200 ms of ring, and event-driven rather than polled: this path is less travelled
        // than endpoint loopback and Microsoft's own sample drives it from the buffer event.
        const REFERENCE_TIME bufferDuration = 2'000'000;

        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK
                                          | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                          | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                          | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      bufferDuration, 0, &format, nullptr)))
            return giveUp("Windows refused a recording stream for that application");

        ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ready == nullptr || FAILED(client->SetEventHandle(ready)))
            return giveUp("Could not attach to the audio stream");

        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      (void**) capture.resetAndGetPointerAddress())))
            return giveUp("Could not start capturing that application");

        mixChannelCount = mixChannelsFound;
        channels = requestedChannelCap > 0 ? juce::jmin(requestedChannelCap, mixChannelsFound) : mixChannelsFound;
        rate     = mixRate;

        resize(juce::jmax(1024, (int) (mixRate * 0.2)));

        nextExpectedPosition = 0;
        havePosition = false;

        if (FAILED(client->Start()))
            return giveUp("Windows would not start the recording");

        startupComplete.signal();

        while (! threadShouldExit())
        {
            // A timeout rather than an infinite wait, so exiting never depends on the
            // target making a sound.
            WaitForSingleObject(ready, 200);

            UINT32 packetFrames = 0;
            while (! threadShouldExit()
                   && SUCCEEDED(capture->GetNextPacketSize(&packetFrames))
                   && packetFrames > 0)
            {
                BYTE* data      = nullptr;
                UINT32 frames   = 0;
                DWORD flags     = 0;
                UINT64 position = 0;

                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, nullptr)))
                {
                    fail();
                    break;
                }

                // Insurance, not a fix for something seen: this stream clocks through
                // silence, so unlike endpoint loopback there has never been a gap to close.
                if (havePosition && position > nextExpectedPosition)
                    deliverSilence((juce::int64) (position - nextExpectedPosition));

                deliver(data, (int) frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);

                capture->ReleaseBuffer(frames);

                nextExpectedPosition = position + frames;
                havePosition = true;
            }
        }

        client->Stop();

        if (ready != nullptr)
            CloseHandle(ready);
    }

    /** The async half of opening a process-loopback client. */
    bool activate(detail::ComPtr<IAudioClient>& clientOut)
    {
        AUDIOCLIENT_ACTIVATION_PARAMS params {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = (DWORD) targetPid;
        params.ProcessLoopbackParams.ProcessLoopbackMode
            = scope == Scope::targetTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                         : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT blob {};
        blob.vt             = VT_BLOB;
        blob.blob.cbSize    = sizeof(params);
        blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto* handler = new detail::ProcessLoopbackActivation();

        detail::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        const auto hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                    __uuidof(IAudioClient), &blob, handler,
                                                    operation.resetAndGetPointerAddress());

        bool ok = false;

        // Whatever is LEFT of start()'s budget, capped at activationWaitMs - never a flat
        // 3000. A flat number is only shorter than start()'s five seconds when everything
        // ahead of it on this thread was instant, and defaultMixFormat() is an untimed COM
        // call: a slow front half used to push this wait past start()'s deadline, and then
        // stop() ran stopThread() against a thread parked right here. Running out of budget
        // now fails the activation, which start() reports, instead of racing it.
        if (SUCCEEDED(hr) && handler->waitFor(remainingStartupMs()) && SUCCEEDED(handler->result)
            && handler->client)
        {
            *clientOut.resetAndGetPointerAddress() = handler->client.get();
            handler->client->AddRef();
            ok = true;
        }

        handler->Release();
        return ok;
    }

    /** What is left of start()'s budget, in milliseconds, capped at activationWaitMs.
        Zero when it is already spent, which turns a wait into an immediate failure. */
    DWORD remainingStartupMs() const
    {
        const auto left = startupDeadline - juce::Time::getMillisecondCounterHiRes();
        return (DWORD) juce::jlimit(0, activationWaitMs, (int) left);
    }

    //==========================================================================
    void resize(int frames)
    {
        scratch.assign((size_t) juce::jmax(1, frames) * (size_t) juce::jmax(1, channels), 0.0f);
        pointers.resize((size_t) juce::jmax(1, channels));
        scratchFrames = frames;
    }

    float* channelStart(int channel) { return scratch.data() + (size_t) channel * (size_t) scratchFrames; }

    /** Deinterleaves one packet into the scratch planes and hands it over. The format is
        always float32 because we asked for it, so there is no sample type to guess at. */
    void deliver(const BYTE* data, int frames, bool silent)
    {
        // Loaded ONCE. Reading the member again at the bottom could see the nullptr that
        // stop() writes in between, which is the crash this is here to avoid.
        auto* destination = sink.load();

        if (frames <= 0 || destination == nullptr)
            return;

        if (frames > scratchFrames)
            resize(frames);

        const auto* interleaved = reinterpret_cast<const float*>(data);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = channelStart(ch);

            if (silent || interleaved == nullptr)
            {
                std::fill(dest, dest + frames, 0.0f);
                continue;
            }

            const auto* source = interleaved + ch;

            for (int i = 0; i < frames; ++i)
                dest[i] = source[(size_t) i * (size_t) mixChannelCount];
        }

        for (int ch = 0; ch < channels; ++ch)
            pointers[(size_t) ch] = channelStart(ch);

        destination->loopbackBlock(pointers.data(), channels, frames);
    }

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

    void fail()
    {
        deviceFailed.store(true);

        if (auto* destination = sink.load())
            destination->loopbackFailed();
    }

    //==========================================================================
    static constexpr double maxSilencePadSeconds = 30.0;

    // ONE BUDGET, three numbers derived from it, because they only work as a nest.
    //
    // start() waits startupBudgetMs for the capture thread to report. The thread's own
    // waits have to finish inside that, or start() gives up on a thread that is still
    // blocked - and then stop() runs stopThread() against it. juce::Thread::stopThread
    // TERMINATES a thread that misses its timeout, and terminating one parked in COM
    // leaves the apartment corrupt for the whole process, host included.
    //
    // Bounding the activation wait alone is not enough: everything before it on that
    // thread (defaultMixFormat, Initialize, Start) is an untimed COM call, so a slow
    // front half used to push the activation past start()'s deadline. The activation
    // deadline is therefore taken from what is LEFT of the budget, never a fixed 3000.
    static constexpr int startupBudgetMs   = 5000;
    static constexpr int activationWaitMs  = 3000;
    static constexpr int teardownWaitMs    = activationWaitMs + 2000; // > any wait it can be in

    // Read on the capture thread, written on the message thread by start() and stop().
    // Atomic because stop() clears it after a stopThread() that may have timed out, and
    // the one path where that matters is the path where the thread is still running.
    std::atomic<LoopbackSink*> sink { nullptr };

    juce::uint32 targetPid = 0;
    Scope scope = Scope::targetTree;
    int requestedChannelCap = 0;

    double rate         = 0.0;
    int channels        = 0;
    int mixChannelCount = 0;
    int scratchFrames   = 0;

    std::vector<float> scratch;
    std::vector<const float*> pointers;

    UINT64 nextExpectedPosition = 0;
    bool havePosition           = false;

    // Written on the capture thread before startupComplete is signalled, read on the
    // message thread after. The event is the handoff, so no further guard is needed.
    juce::WaitableEvent startupComplete { true };
    juce::String startupError;
    double startupDeadline = 0.0; // capture thread only, set first thing in run()

    std::atomic<bool> deviceFailed { false };

    JUCE_DECLARE_NON_COPYABLE(WasapiProcessLoopback)
};

} // namespace okstudio::capture

#endif // JUCE_WINDOWS
