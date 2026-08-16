//
// Process-loopback spike for the Quarry Sampler.
//
// Answers the three questions docs/SAMPLER.md leaves open before any of the sampler gets
// built:
//
//   1. Does per-process loopback need INCLUDE_TARGET_PROCESS_TREE to catch a browser?
//      ANSWERED, and the question was malformed: PROCESS_LOOPBACK_MODE has exactly two
//      values, INCLUDE_TARGET_PROCESS_TREE and EXCLUDE_TARGET_PROCESS_TREE. There is no
//      way to target one process without its tree, so inclusion is always tree-based.
//      EXCLUDE is not the opposite of INCLUDE on the same target: it captures everything
//      *except* that tree, which is a different feature (record the machine but not
//      Discord). --exclude is here to exercise that, not to compare against INCLUDE.
//   2. Is the capture before or after the app's own session volume? --volume-test halves
//      the target's session volume halfway through one continuous capture and compares
//      the two halves, so the same material is measured either side of the change.
//   3. Does asking for the endpoint's mix format avoid a resample, and what happens when
//      we ask for something else? ANSWERED: asking for 48k on a 44.1k endpoint opens and
//      delivers 48k, so Windows converts rather than refusing. Ask for the mix format to
//      avoid a conversion nobody asked for.
//
// Deliberately standalone: no JUCE, no Quarry, so it builds in seconds and a wrong answer
// costs nothing. Whatever it proves moves into okstudio/WasapiProcessLoopback.h next to
// the endpoint loopback that already exists.
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h> // IAudioMeterInformation, for the per-session peak
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace
{

//==============================================================================
// Small helpers

/** CoInitializeEx that only uninitializes when it was the one that initialized. */
struct ComScope
{
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() { if (owned) CoUninitialize(); }
    const bool owned;
};

std::string narrow(const wchar_t* wide)
{
    if (wide == nullptr)
        return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};

    std::vector<char> buffer((size_t) needed);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer.data(), needed, nullptr, nullptr);
    return std::string(buffer.data());
}

/** Exe name for a pid, or "" when the process is gone or out of reach. */
std::string processName(DWORD pid)
{
    if (pid == 0)
        return "(system)";

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return {};

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != 0;
    CloseHandle(h);

    if (! ok)
        return {};

    std::string full = narrow(path);
    const auto slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

double toDb(double linear)
{
    return linear <= 1.0e-9 ? -144.0 : 20.0 * std::log10(linear);
}

//==============================================================================
// A float32 WAV writer. Non-PCM wants an 18-byte fmt chunk and a fact chunk; plenty of
// readers cope without, but the sampler's whole point is files that open everywhere.

void put32(std::vector<unsigned char>& out, uint32_t v)
{
    out.push_back((unsigned char) (v & 0xff));
    out.push_back((unsigned char) ((v >> 8) & 0xff));
    out.push_back((unsigned char) ((v >> 16) & 0xff));
    out.push_back((unsigned char) ((v >> 24) & 0xff));
}

void put16(std::vector<unsigned char>& out, uint16_t v)
{
    out.push_back((unsigned char) (v & 0xff));
    out.push_back((unsigned char) ((v >> 8) & 0xff));
}

void putTag(std::vector<unsigned char>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

bool writeFloatWav(const std::string& path, const std::vector<float>& interleaved,
                   int channels, int sampleRate)
{
    const uint32_t dataBytes  = (uint32_t) (interleaved.size() * sizeof(float));
    const uint32_t frameCount = channels > 0 ? (uint32_t) (interleaved.size() / (size_t) channels) : 0;

    std::vector<unsigned char> header;
    header.reserve(64);

    putTag(header, "RIFF");
    put32(header, 4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    putTag(header, "WAVE");

    putTag(header, "fmt ");
    put32(header, 18);
    put16(header, 3); // WAVE_FORMAT_IEEE_FLOAT
    put16(header, (uint16_t) channels);
    put32(header, (uint32_t) sampleRate);
    put32(header, (uint32_t) (sampleRate * channels * 4));
    put16(header, (uint16_t) (channels * 4));
    put16(header, 32);
    put16(header, 0); // cbSize

    putTag(header, "fact");
    put32(header, 4);
    put32(header, frameCount);

    putTag(header, "data");
    put32(header, dataBytes);

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr)
        return false;

    const bool ok = fwrite(header.data(), 1, header.size(), f) == header.size()
                 && (dataBytes == 0 || fwrite(interleaved.data(), 1, dataBytes, f) == dataBytes);

    fclose(f);
    return ok;
}

//==============================================================================
// Listing what is currently making sound, which is also how you find a pid to pass in.
// This is build-order step 2 arriving early: the sampler's source picker is this same
// enumeration with a meter next to each row.

struct SessionRow
{
    DWORD pid = 0;
    std::string name;
    float volume = 1.0f;
    float peak = 0.0f;
    bool active = false;
};

std::vector<SessionRow> audioSessions()
{
    std::vector<SessionRow> rows;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return rows;

    ComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)))
        return rows;

    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)))
        return rows;

    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions)))
        return rows;

    int count = 0;
    if (FAILED(sessions->GetCount(&count)))
        return rows;

    for (int i = 0; i < count; ++i)
    {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, &control)))
            continue;

        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2)))
            continue;

        SessionRow row;
        control2->GetProcessId(&row.pid);
        row.name = processName(row.pid);

        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control->GetState(&state)))
            row.active = state == AudioSessionStateActive;

        ComPtr<ISimpleAudioVolume> volume;
        if (SUCCEEDED(control.As(&volume)))
            volume->GetMasterVolume(&row.volume);

        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(control.As(&meter)))
            meter->GetPeakValue(&row.peak);

        if (row.name.empty())
            row.name = "(pid " + std::to_string(row.pid) + ")";

        rows.push_back(row);
    }

    // Whatever is making noise right now, first. That is what you came to capture.
    std::stable_sort(rows.begin(), rows.end(), [](const SessionRow& a, const SessionRow& b)
    {
        if (a.active != b.active)
            return a.active;
        return a.peak > b.peak;
    });

    return rows;
}

/** The target's own session volume, or -1 when it has no session. */
float sessionVolume(DWORD pid, bool set = false, float newValue = 1.0f)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return -1.0f;

    ComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)))
        return -1.0f;

    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)))
        return -1.0f;

    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions)))
        return -1.0f;

    int count = 0;
    if (FAILED(sessions->GetCount(&count)))
        return -1.0f;

    for (int i = 0; i < count; ++i)
    {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, &control)))
            continue;

        ComPtr<IAudioSessionControl2> control2;
        DWORD sessionPid = 0;
        if (FAILED(control.As(&control2)) || FAILED(control2->GetProcessId(&sessionPid)) || sessionPid != pid)
            continue;

        ComPtr<ISimpleAudioVolume> volume;
        if (FAILED(control.As(&volume)))
            continue;

        if (set)
            volume->SetMasterVolume(newValue, nullptr);

        float current = -1.0f;
        volume->GetMasterVolume(&current);
        return current;
    }

    return -1.0f;
}

/** The default render endpoint's mix format, so we can ask process loopback for exactly
    that and find out whether it then has anything to convert. */
bool defaultMixFormat(int& channelsOut, int& rateOut)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return false;

    ComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)))
        return false;

    ComPtr<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
        return false;

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
        return false;

    channelsOut = (int) mix->nChannels;
    rateOut     = (int) mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    return true;
}

//==============================================================================
// Process loopback activation.
//
// Unlike an endpoint, a process-loopback client is not reached through IMMDevice. It is
// activated by name against the virtual device VAD\Process_Loopback, asynchronously, with
// the target pid handed over in a blob. The completion handler has to be agile because
// the callback arrives on an MTA thread that is not this one, which is what FtmBase is for.

class ActivationHandler
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
    ActivationHandler() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~ActivationHandler() override
    {
        if (done != nullptr)
            CloseHandle(done);
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activationResult = E_UNEXPECTED;
        ComPtr<IUnknown> raw;

        const HRESULT hr = operation->GetActivateResult(&activationResult, &raw);

        result = FAILED(hr) ? hr : activationResult;

        if (SUCCEEDED(result) && raw)
            raw.As(&client);

        SetEvent(done);
        return S_OK;
    }

    HANDLE done = nullptr;
    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> client;
};

/** Activates a process-loopback IAudioClient for `pid`, waiting for the async result. */
HRESULT activateProcessLoopback(DWORD pid, bool includeTree, ComPtr<IAudioClient>& clientOut)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        includeTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                    : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT blob = {};
    blob.vt           = VT_BLOB;
    blob.blob.cbSize  = sizeof(params);
    blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    if (! handler)
        return E_OUTOFMEMORY;

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                   __uuidof(IAudioClient),
                                                   &blob,
                                                   handler.Get(),
                                                   &operation);
    if (FAILED(hr))
        return hr;

    if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    if (FAILED(handler->result))
        return handler->result;

    clientOut = handler->client;
    return clientOut ? S_OK : E_NOINTERFACE;
}

//==============================================================================

struct CaptureOptions
{
    DWORD pid = 0;
    bool includeTree = true;
    bool volumeTest = false;
    double seconds = 10.0;
    int rate = 0;      // 0 means "the endpoint's mix rate"
    int channels = 0;  // 0 means "the endpoint's mix channel count"
    std::string outPath = "spike-capture.wav";
};

int capture(const CaptureOptions& options)
{
    int mixChannels = 2;
    int mixRate = 48000;
    const bool haveMix = defaultMixFormat(mixChannels, mixRate);

    const int channels = options.channels > 0 ? options.channels : mixChannels;
    const int rate     = options.rate > 0 ? options.rate : mixRate;

    printf("target        : pid %lu (%s)\n", options.pid, processName(options.pid).c_str());
    printf("tree mode     : %s\n", options.includeTree ? "INCLUDE_TARGET_PROCESS_TREE"
                                                       : "EXCLUDE_TARGET_PROCESS_TREE");
    if (haveMix)
        printf("endpoint mix  : %d ch @ %d Hz\n", mixChannels, mixRate);
    else
        printf("endpoint mix  : unavailable, assuming %d ch @ %d Hz\n", mixChannels, mixRate);

    printf("requesting    : %d ch @ %d Hz float32%s\n", channels, rate,
           (rate == mixRate && channels == mixChannels) ? " (matches mix, no conversion expected)"
                                                        : " (differs from mix, Windows must convert)");

    // Report the target's session volume before we start, because it is the single most
    // likely reason a capture comes back quieter than expected.
    for (const auto& row : audioSessions())
    {
        if (row.pid == options.pid)
        {
            printf("session vol   : %.0f%%%s\n", row.volume * 100.0f,
                   row.volume < 0.999f ? "  <-- not 100%, expect attenuation if capture is post-volume" : "");
            break;
        }
    }

    ComPtr<IAudioClient> client;
    const HRESULT activation = activateProcessLoopback(options.pid, options.includeTree, client);
    if (FAILED(activation))
    {
        printf("\nFAILED to activate process loopback: 0x%08lx\n", (unsigned long) activation);
        printf("If this is 0x80070490 the pid is gone; 0x80004001 means this Windows build\n"
               "predates process loopback (needs Windows 10 2004 / build 20348 or later).\n");
        return 1;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels       = (WORD) channels;
    format.nSamplesPerSec  = (DWORD) rate;
    format.wBitsPerSample  = 32;
    format.nBlockAlign     = (WORD) (channels * 4);
    format.nAvgBytesPerSec = (DWORD) (rate * channels * 4);
    format.cbSize          = 0;

    // 200 ms of ring. Event-driven rather than polled, matching Microsoft's own
    // ApplicationLoopback sample: this path is less travelled than endpoint loopback and
    // the spike should not be the place we discover a difference.
    const REFERENCE_TIME bufferDuration = 2'000'000;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK
                                        | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    bufferDuration,
                                    0,
                                    &format,
                                    nullptr);
    if (FAILED(hr))
    {
        printf("\nFAILED to initialize the stream: 0x%08lx\n", (unsigned long) hr);
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
            printf("Windows refused this format outright rather than converting to it.\n");
        return 1;
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready == nullptr || FAILED(client->SetEventHandle(ready)))
    {
        printf("\nFAILED to attach the buffer-ready event\n");
        return 1;
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture))))
    {
        printf("\nFAILED to get the capture service\n");
        return 1;
    }

    if (FAILED(client->Start()))
    {
        printf("\nFAILED to start the stream\n");
        return 1;
    }

    printf("\ncapturing %.1f s ... play something in the target now\n", options.seconds);

    std::vector<float> samples;
    samples.reserve((size_t) (options.seconds * rate * channels));

    const DWORD startTick = GetTickCount();
    const DWORD limitMs   = (DWORD) (options.seconds * 1000.0);

    uint64_t expectedPosition = 0;
    bool havePosition = false;
    uint64_t paddedFrames = 0;
    uint64_t silentFrames = 0;
    uint64_t realFrames = 0;

    // For the volume test: the sample index at which we halved the target's volume, so the
    // two halves of one continuous capture can be measured against each other.
    size_t volumeChangeIndex = 0;
    float originalVolume = -1.0f;
    bool volumeChanged = false;

    if (options.volumeTest)
    {
        originalVolume = sessionVolume(options.pid);
        printf("volume test   : will halve the session volume at the halfway point, then restore %.0f%%\n",
               originalVolume * 100.0f);
    }

    while (GetTickCount() - startTick < limitMs)
    {
        if (options.volumeTest && ! volumeChanged && GetTickCount() - startTick >= limitMs / 2)
        {
            volumeChangeIndex = samples.size();
            sessionVolume(options.pid, true, 0.5f * (originalVolume > 0.0f ? originalVolume : 1.0f));
            volumeChanged = true;
        }

        WaitForSingleObject(ready, 200);

        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0)
        {
            BYTE* data      = nullptr;
            UINT32 frames   = 0;
            DWORD flags     = 0;
            UINT64 position = 0;

            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, nullptr)))
                break;

            // A loopback stream is not a clock: nothing arrives while the target is idle.
            // The device position is what tells us how much wall clock we just skipped.
            if (havePosition && position > expectedPosition)
            {
                const uint64_t gap = std::min<uint64_t>(position - expectedPosition,
                                                        (uint64_t) (rate * 30));
                samples.insert(samples.end(), (size_t) (gap * (uint64_t) channels), 0.0f);
                paddedFrames += gap;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            if (silent || data == nullptr)
            {
                samples.insert(samples.end(), (size_t) frames * (size_t) channels, 0.0f);
                silentFrames += frames;
            }
            else
            {
                const auto* in = reinterpret_cast<const float*>(data);
                samples.insert(samples.end(), in, in + (size_t) frames * (size_t) channels);
                realFrames += frames;
            }

            capture->ReleaseBuffer(frames);

            expectedPosition = position + frames;
            havePosition = true;
        }
    }

    client->Stop();
    CloseHandle(ready);

    if (volumeChanged && originalVolume >= 0.0f)
        sessionVolume(options.pid, true, originalVolume);

    double peak = 0.0;
    double sumSquares = 0.0;
    for (const float s : samples)
    {
        peak = std::max(peak, (double) std::fabs(s));
        sumSquares += (double) s * (double) s;
    }
    const double rms = samples.empty() ? 0.0 : std::sqrt(sumSquares / (double) samples.size());

    const double durationSec = channels > 0 ? (double) samples.size() / (double) channels / (double) rate : 0.0;

    printf("\n--- result ---\n");
    printf("frames        : %llu real, %llu flagged silent, %llu padded across gaps\n",
           (unsigned long long) realFrames, (unsigned long long) silentFrames,
           (unsigned long long) paddedFrames);
    printf("duration      : %.2f s written for %.2f s of wall clock\n", durationSec, options.seconds);
    printf("peak          : %.6f  (%.2f dBFS)\n", peak, toDb(peak));
    printf("rms           : %.6f  (%.2f dBFS)\n", rms, toDb(rms));

    if (volumeChanged)
    {
        auto rmsOf = [&samples](size_t from, size_t to)
        {
            if (to <= from)
                return 0.0;

            double sum = 0.0;
            for (size_t i = from; i < to; ++i)
                sum += (double) samples[i] * (double) samples[i];
            return std::sqrt(sum / (double) (to - from));
        };

        const double before = rmsOf(0, volumeChangeIndex);
        const double after  = rmsOf(volumeChangeIndex, samples.size());
        const double deltaDb = toDb(after) - toDb(before);

        printf("\n--- session volume ---\n");
        printf("before halving: %.2f dBFS rms\n", toDb(before));
        printf("after halving : %.2f dBFS rms\n", toDb(after));
        printf("difference    : %+.2f dB\n", deltaDb);
        printf("verdict       : %s\n", deltaDb < -3.0
                   ? "POST-volume. The app's own slider is baked into the capture, so the\n"
                     "                source picker must warn when a session is below 100%."
                   : "PRE-volume, or the material changed too much to tell. Re-run on\n"
                     "                something steady before trusting this.");
    }

    if (peak <= 1.0e-7)
        printf("\nNOTHING WAS CAPTURED. Either the target made no sound, or its audio comes from\n"
               "a process outside the tree you targeted. Use the pid --list reports, which is\n"
               "the process that actually owns the render session.\n");
    else if (peak > 1.0)
        printf("\nPeak exceeds 0 dBFS, which float32 carries fine and is exactly why the design\n"
               "does not convert to an integer format.\n");

    if (! writeFloatWav(options.outPath, samples, channels, rate))
    {
        printf("\nFAILED to write %s\n", options.outPath.c_str());
        return 1;
    }

    printf("wrote         : %s (%d ch @ %d Hz float32)\n", options.outPath.c_str(), channels, rate);
    return 0;
}

int list()
{
    const auto rows = audioSessions();

    if (rows.empty())
    {
        printf("No audio sessions on the default playback endpoint.\n");
        return 0;
    }

    printf("%-8s %-28s %-8s %-8s %s\n", "PID", "PROCESS", "VOLUME", "PEAK", "STATE");
    for (const auto& row : rows)
        printf("%-8lu %-28s %-8.0f %-8.4f %s\n", row.pid, row.name.c_str(),
               row.volume * 100.0f, row.peak, row.active ? "active" : "idle");

    printf("\nPick the pid of the app you want and pass it to --pid.\n"
           "For a browser use the main process: tree mode should pull in its audio child.\n");
    return 0;
}

void usage()
{
    printf("Quarry Sampler process-loopback spike\n\n"
           "  loopback_spike --list\n"
           "      Everything with an audio session, with its volume and current peak.\n\n"
           "  loopback_spike --pid <n> [options]\n"
           "      --seconds <s>   how long to capture           (default 10)\n"
           "      --out <path>    where to write the wav        (default spike-capture.wav)\n"
           "      --exclude       capture everything EXCEPT this process tree\n"
           "      --volume-test   halve the target's session volume midway, compare halves\n"
           "      --rate <hz>     ask for a rate other than the endpoint mix rate\n"
           "      --channels <n>  ask for a channel count other than the mix's\n");
}

} // namespace

int main(int argc, char** argv)
{
    ComScope com;

    if (argc < 2)
    {
        usage();
        return 0;
    }

    const std::string first = argv[1];

    if (first == "--list")
        return list();

    if (first == "--help" || first == "-h")
    {
        usage();
        return 0;
    }

    CaptureOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;

        if (arg == "--pid" && hasNext)
            options.pid = (DWORD) strtoul(argv[++i], nullptr, 10);
        else if (arg == "--seconds" && hasNext)
            options.seconds = strtod(argv[++i], nullptr);
        else if (arg == "--out" && hasNext)
            options.outPath = argv[++i];
        else if (arg == "--rate" && hasNext)
            options.rate = atoi(argv[++i]);
        else if (arg == "--channels" && hasNext)
            options.channels = atoi(argv[++i]);
        else if (arg == "--exclude")
            options.includeTree = false;
        else if (arg == "--volume-test")
            options.volumeTest = true;
        else
        {
            printf("Unrecognised argument: %s\n\n", arg.c_str());
            usage();
            return 1;
        }
    }

    if (options.pid == 0)
    {
        printf("No --pid given. Run --list to find one.\n");
        return 1;
    }

    return capture(options);
}
