//
// Screen-capture spike for Quarry.
//
// Proves the capture and encode path for general screen recording (any window or a whole
// monitor, to an mp4 with sound) using Windows APIs only, before any of it goes into the app.
// No third-party code of any kind: Windows.Graphics.Capture (C++/WinRT), Direct3D 11,
// Media Foundation and WASAPI, written from Microsoft's documentation.
//
// Questions this spike answers (answers are filled in from the verification runs, see the
// RESULTS block at the end of this comment):
//
//   1. Does Windows.Graphics.Capture + IMFSinkWriter give a constant-frame-rate H.264 mp4
//      when the capture only delivers a frame on change? A pacing thread ticks at exactly
//      fps on QueryPerformanceCounter and submits the newest frame each tick (repeating it
//      when nothing changed), so the sample time is tick / fps and never the capture time.
//   2. Do frames stay on the GPU all the way into the encoder? Frames are copied into
//      textures owned by an IMFVideoSampleAllocatorEx pool (WGC recycles its surfaces and
//      the sink writer encodes asynchronously), and the sink writer gets an
//      IMFDXGIDeviceManager. --cpu is the safety net: read back to system memory, RGB32.
//   3. Which encoder actually runs (hardware or software)? Asked through
//      IMFSinkWriterEx::GetTransformForStream after BeginWriting.
//   4. Does a window that is resized mid-recording survive? The encode size is fixed at the
//      first size; later frames are copied over their overlapping region.
//   5. Can WASAPI loopback of the default render endpoint be written as a second AAC stream
//      that stays in sync with the video? Loopback delivers nothing while nothing plays, so
//      gaps are filled with silence from the same QPC clock; audio and video share one QPC
//      origin (the first video tick) and audio captured before it is discarded.
//
// Usage:
//   screen_spike [--list] [--monitor N | --window "title substring"] [--seconds S]
//                [--fps F] [--out path.mp4] [--no-audio] [--cpu]
//
// RESULTS (RTX 5090, Windows 11 build 26300, 3840x2160 primary monitor, 5 s runs):
//
//   1. ANSWERED, yes. Monitor at 60 fps: ffprobe shows h264 High, r_frame_rate 60/1,
//      300 frames, duration 5.000 s. Typical run: 300 of 300 ticks, 20 to 60 repeats on a
//      mostly static desktop, 0 late ticks. Some captured frames are never used when WGC
//      delivers faster than the tick or in bursts (27 to 46 of about 300 on the monitor).
//   2. ANSWERED, yes. GPU path ran on "NVIDIA H.264 Encoder MFT" with only that transform
//      in the chain, so the BGRA to NV12 conversion happens inside the hardware encoder
//      MFT. The --cpu path inserts a software colour converter in front of the same encoder.
//   3. ANSWERED: GetTransformForStream works after BeginWriting; MFT_FRIENDLY_NAME_Attribute
//      is present on the encoder's attribute store.
//   4. NOT VERIFIED in a run (no safe way to resize someone's window from a console tool).
//      The code path exists: frame pool Recreate plus a clipped CopySubresourceRegion.
//   5. ANSWERED, yes for the loopback path: AAC 44.1 kHz stereo, audio length 5.015 s for a
//      5.000 s video (AAC pads to whole 1024-sample frames), both start_time 0. A sound
//      played 1.5 s in came out at 1.56 s in the file. The "no packets at all" gap fill is
//      written but was not exercised: this endpoint delivers silent-flagged packets.
//
// LIMITS found reviewing this file. They are for the app to settle, not this spike:
//
//   - Audio is placed on the QPC timeline and corrected whenever it strays 10 ms from the
//     running sample count, by inserting silence or skipping samples. Two clocks 50 ppm
//     apart reach 10 ms in about 200 s, so a long recording would click. The app must count
//     samples and never edit them.
//   - Idle silence is written up to 30 ms behind the clock. A packet that arrives later than
//     that is partly discarded as overlap.
//   - Sync was checked with one played sound, to about 0.06 s. It was not measured.
//   - A process that dies before Finalize leaves an mp4 with no index.
//   - --seconds is clamped to 6 because of how this was run, not because of the method.
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <codecapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{

//==============================================================================
// Small helpers

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed, HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

std::string toUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

int64_t qpcNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpcFreq()
{
    static const int64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }();
    return f;
}

// QPC ticks to 100 ns units without overflowing 64 bits.
int64_t qpcTo100ns(int64_t q)
{
    const int64_t f = qpcFreq();
    return (q / f) * 10000000 + ((q % f) * 10000000) / f;
}

// 100 ns time of frame index n at the given rate, so durations never accumulate rounding.
int64_t frameTime(int64_t n, int64_t rate)
{
    return (n * 10000000) / rate;
}

// The documented COM interface for getting the D3D texture behind a WinRT Direct3D surface.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(REFIID id, void** object) = 0;
};

//==============================================================================
// Options

struct Options
{
    bool list = false;
    int monitor = -1;           // -1 means the primary monitor
    std::wstring windowSubstr;  // non-empty means capture a window
    int seconds = 5;
    int fps = 60;
    std::wstring out = L"tools/screen_spike/build/spike.mp4";
    bool audio = true;
    bool cpu = false;
};

bool parseArgs(int argc, wchar_t** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& dst) { if (i + 1 >= argc) return false; dst = argv[++i]; return true; };
        std::wstring v;
        if (a == L"--list") o.list = true;
        else if (a == L"--no-audio") o.audio = false;
        else if (a == L"--cpu") o.cpu = true;
        else if (a == L"--monitor") { if (!next(v)) return false; o.monitor = _wtoi(v.c_str()); }
        else if (a == L"--window") { if (!next(o.windowSubstr)) return false; }
        else if (a == L"--seconds") { if (!next(v)) return false; o.seconds = _wtoi(v.c_str()); }
        else if (a == L"--fps") { if (!next(v)) return false; o.fps = _wtoi(v.c_str()); }
        else if (a == L"--out") { if (!next(o.out)) return false; }
        else return false;
    }
    o.seconds = std::max(1, std::min(o.seconds, 6));  // spike rule: runs stay short
    o.fps = std::max(1, std::min(o.fps, 240));
    return true;
}

//==============================================================================
// Monitors and windows

struct MonitorInfo { HMONITOR h; RECT rc; bool primary; std::wstring name; };

std::vector<MonitorInfo> listMonitors()
{
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(h, &mi);
            reinterpret_cast<std::vector<MonitorInfo>*>(lp)->push_back(
                { h, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.szDevice });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
    return out;
}

struct WindowInfo { HWND h; std::wstring title; RECT rc; };

std::vector<WindowInfo> listWindows()
{
    std::vector<WindowInfo> out;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr)
            return TRUE;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
            return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked)
            return TRUE;
        const int len = GetWindowTextLengthW(h);
        if (len <= 0)
            return TRUE;
        std::wstring t(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        t.resize(static_cast<size_t>(len));
        RECT rc{};
        GetWindowRect(h, &rc);
        reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back({ h, t, rc });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

void printList()
{
    const auto mons = listMonitors();
    std::printf("Monitors:\n");
    for (size_t i = 0; i < mons.size(); ++i)
        std::printf("  [%zu] %s  %ldx%ld at (%ld,%ld)%s\n", i, toUtf8(mons[i].name).c_str(),
                    mons[i].rc.right - mons[i].rc.left, mons[i].rc.bottom - mons[i].rc.top,
                    mons[i].rc.left, mons[i].rc.top, mons[i].primary ? "  [primary]" : "");
    std::printf("Windows:\n");
    for (const auto& w : listWindows())
        std::printf("  0x%p  %ldx%ld  %s\n", static_cast<void*>(w.h), w.rc.right - w.rc.left,
                    w.rc.bottom - w.rc.top, toUtf8(w.title).c_str());
}

//==============================================================================
// Capture: Windows.Graphics.Capture into a texture this program owns

class Capturer
{
public:
    Capturer(ComPtr<ID3D11Device> dev, ComPtr<ID3D11DeviceContext> ctx, wgc::GraphicsCaptureItem it)
        : device(std::move(dev)), context(std::move(ctx)), item(std::move(it))
    {
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi), "QI IDXGIDevice");
        winrt::com_ptr<::IInspectable> insp;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()), "CreateDirect3D11DeviceFromDXGIDevice");
        winrtDevice = insp.as<wgd3::IDirect3DDevice>();

        const auto sz = item.Size();
        srcW = sz.Width;
        srcH = sz.Height;
        encW = srcW & ~1;
        encH = srcH & ~1;
        if (encW < 2 || encH < 2)
            throw std::runtime_error("capture item has no size (minimised window?)");

        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(encW);
        d.Height = static_cast<UINT>(encH);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&d, nullptr, &latest), "CreateTexture2D(latest)");
        clearLatest();

        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, sz);
        poolW = srcW;
        poolH = srcH;
        session = pool.CreateCaptureSession(item);
        try { session.IsCursorCaptureEnabled(true); } catch (...) {}
        try
        {
            session.IsBorderRequired(false);
            borderOff = true;
        }
        catch (...) { borderOff = false; }

        revoker = pool.FrameArrived(winrt::auto_revoke,
            [this](wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
                onFrame(sender);
            });
        session.StartCapture();
    }

    ~Capturer() { stop(); }

    void stop()
    {
        if (stopped)
            return;
        stopped = true;
        revoker.revoke();
        try { session.Close(); } catch (...) {}
        try { pool.Close(); } catch (...) {}
    }

    bool waitFirstFrame(int ms)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end)
        {
            {
                std::lock_guard<std::mutex> g(m);
                if (haveFrame)
                    return true;
            }
            Sleep(2);
        }
        return false;
    }

    // Copies the newest frame into dst (a texture the caller owns). Returns false if there
    // has been no frame yet. repeated is set when the same frame was already submitted.
    bool copyLatestTo(ID3D11Texture2D* dst, UINT dstSub, bool& repeated)
    {
        std::lock_guard<std::mutex> g(m);
        if (!haveFrame)
            return false;
        repeated = used;
        used = true;
        context->CopySubresourceRegion(dst, dstSub, 0, 0, 0, latest.Get(), 0, nullptr);
        context->Flush();
        return true;
    }

    ID3D11Texture2D* latestTexture() { return latest.Get(); }

    int encW = 0, encH = 0;
    bool borderOff = false;
    std::atomic<uint64_t> captured{ 0 };
    std::atomic<uint64_t> unusedFrames{ 0 };
    std::atomic<uint64_t> resizes{ 0 };

private:
    void clearLatest()
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device->CreateRenderTargetView(latest.Get(), nullptr, &rtv)))
        {
            const float black[4] = { 0, 0, 0, 1 };
            context->ClearRenderTargetView(rtv.Get(), black);
        }
    }

    void onFrame(wgc::Direct3D11CaptureFramePool const& sender)
    {
        for (;;)
        {
            wgc::Direct3D11CaptureFrame frame{ nullptr };
            try { frame = sender.TryGetNextFrame(); } catch (...) { return; }
            if (!frame)
                return;
            try
            {
                const auto cs = frame.ContentSize();
                ComPtr<ID3D11Texture2D> tex;
                IDxgiInterfaceAccess* raw = nullptr;
                check(winrt::get_unknown(frame.Surface())->QueryInterface(__uuidof(IDxgiInterfaceAccess),
                          reinterpret_cast<void**>(&raw)), "QI IDirect3DDxgiInterfaceAccess");
                ComPtr<IDxgiInterfaceAccess> access;
                access.Attach(raw);
                check(access->GetInterface(IID_PPV_ARGS(&tex)), "GetInterface(texture)");

                {
                    std::lock_guard<std::mutex> g(m);
                    if (cs.Width != encW || cs.Height != encH)
                        clearLatest();  // content is a different size: pad with black
                    D3D11_BOX box{};
                    box.right = static_cast<UINT>(std::min(cs.Width, encW));
                    box.bottom = static_cast<UINT>(std::min(cs.Height, encH));
                    box.back = 1;
                    context->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                    if (haveFrame && !used)
                        ++unusedFrames;
                    used = false;
                    haveFrame = true;
                    ++captured;
                }

                if (cs.Width != poolW || cs.Height != poolH)
                {
                    poolW = cs.Width;
                    poolH = cs.Height;
                    ++resizes;
                    sender.Recreate(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
                }
            }
            catch (...) {}
            frame.Close();
        }
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    wgc::GraphicsCaptureItem item;
    wgd3::IDirect3DDevice winrtDevice{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker revoker;
    ComPtr<ID3D11Texture2D> latest;
    int srcW = 0, srcH = 0, poolW = 0, poolH = 0;
    std::mutex m;
    bool haveFrame = false;
    bool used = false;
    bool stopped = false;
};

//==============================================================================
// Muxer: one IMFSinkWriter with a video stream and an optional audio stream

class Encoder
{
public:
    struct Config
    {
        int w, h, fps;
        bool gpu;
        bool hardware;
        bool audio;
        UINT32 audioRate;
        std::wstring path;
        ID3D11Device* device;
    };

    ~Encoder() = default;

    void open(const Config& c)
    {
        cfg = c;
        ComPtr<IMFAttributes> at;
        check(MFCreateAttributes(&at, 6), "MFCreateAttributes");
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, c.hardware ? TRUE : FALSE);
        at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        if (c.gpu)
        {
            UINT token = 0;
            check(MFCreateDXGIDeviceManager(&token, &devMgr), "MFCreateDXGIDeviceManager");
            check(devMgr->ResetDevice(c.device, token), "IMFDXGIDeviceManager::ResetDevice");
            at->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devMgr.Get());
        }
        check(MFCreateSinkWriterFromURL(c.path.c_str(), nullptr, at.Get(), &writer), "MFCreateSinkWriterFromURL");

        // Video output: H.264, bitrate about 0.1 bit per pixel per frame, 4..60 Mbps.
        const double bps = 0.1 * c.w * c.h * c.fps;
        bitrate = static_cast<UINT32>(std::max(4e6, std::min(60e6, bps)));
        ComPtr<IMFMediaType> vo;
        check(MFCreateMediaType(&vo), "MFCreateMediaType");
        vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vo->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vo->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(vo.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(c.w), static_cast<UINT32>(c.h));
        MFSetAttributeRatio(vo.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(c.fps), 1);
        MFSetAttributeRatio(vo.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        check(writer->AddStream(vo.Get(), &videoStream), "AddStream(video)");

        // Video input: GPU path hands over BGRA textures, CPU path hands over RGB32 memory.
        ComPtr<IMFMediaType> vi;
        check(MFCreateMediaType(&vi), "MFCreateMediaType");
        vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vi->SetGUID(MF_MT_SUBTYPE, c.gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32);
        vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vi->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(c.w * 4));  // positive: top-down
        MFSetAttributeSize(vi.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(c.w), static_cast<UINT32>(c.h));
        MFSetAttributeRatio(vi.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(c.fps), 1);
        MFSetAttributeRatio(vi.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        check(writer->SetInputMediaType(videoStream, vi.Get(), nullptr), "SetInputMediaType(video)");

        if (c.audio)
        {
            ComPtr<IMFMediaType> ao;
            check(MFCreateMediaType(&ao), "MFCreateMediaType");
            ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, c.audioRate);
            ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
            ao->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            ao->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            check(writer->AddStream(ao.Get(), &audioStream), "AddStream(audio)");

            ComPtr<IMFMediaType> ai;
            check(MFCreateMediaType(&ai), "MFCreateMediaType");
            ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, c.audioRate);
            ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, c.audioRate * 4);
            check(writer->SetInputMediaType(audioStream, ai.Get(), nullptr), "SetInputMediaType(audio)");
        }

        if (c.gpu)
        {
            // A recycled pool of DXGI-backed samples. The sink writer returns a sample to the
            // pool when it has finished with it, so each tick owns its texture until then.
            check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator)), "MFCreateVideoSampleAllocatorEx");
            check(allocator->SetDirectXManager(devMgr.Get()), "SetDirectXManager");
            ComPtr<IMFAttributes> aa;
            check(MFCreateAttributes(&aa, 3), "MFCreateAttributes");
            aa->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
            aa->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
            aa->SetUINT32(MF_SA_BUFFERS_PER_SAMPLE, 1);
            check(allocator->InitializeSampleAllocatorEx(4, 12, aa.Get(), vi.Get()), "InitializeSampleAllocatorEx");
        }
        else
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(c.w);
            d.Height = static_cast<UINT>(c.h);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(c.device->CreateTexture2D(&d, nullptr, &staging), "CreateTexture2D(staging)");
            c.device->GetImmediateContext(&ctx);
        }

        check(writer->BeginWriting(), "BeginWriting");
        describeEncoder();
    }

    // Submits one video frame. Returns false if it had to be dropped.
    bool submitVideo(Capturer& cap, int64_t tick, bool& repeated)
    {
        ComPtr<IMFSample> s;
        if (cfg.gpu)
        {
            HRESULT hr = E_FAIL;
            for (int tries = 0; tries < 100; ++tries)
            {
                hr = allocator->AllocateSample(&s);
                if (SUCCEEDED(hr))
                    break;
                Sleep(1);
            }
            if (FAILED(hr))
            {
                ++poolEmptyDrops;
                return false;
            }
            ComPtr<IMFMediaBuffer> buf;
            check(s->GetBufferByIndex(0, &buf), "GetBufferByIndex");
            ComPtr<IMFDXGIBuffer> dxb;
            check(buf.As(&dxb), "QI IMFDXGIBuffer");
            ComPtr<ID3D11Texture2D> tex;
            check(dxb->GetResource(IID_PPV_ARGS(&tex)), "IMFDXGIBuffer::GetResource");
            UINT sub = 0;
            dxb->GetSubresourceIndex(&sub);
            if (!cap.copyLatestTo(tex.Get(), sub, repeated))
                return false;
            buf->SetCurrentLength(static_cast<DWORD>(cfg.w * cfg.h * 4));
        }
        else
        {
            if (!cap.copyLatestTo(staging.Get(), 0, repeated))
                return false;
            D3D11_MAPPED_SUBRESOURCE map{};
            check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map(staging)");
            const DWORD bytes = static_cast<DWORD>(cfg.w * cfg.h * 4);
            ComPtr<IMFMediaBuffer> buf;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
            if (SUCCEEDED(hr))
            {
                BYTE* dst = nullptr;
                buf->Lock(&dst, nullptr, nullptr);
                for (int y = 0; y < cfg.h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(cfg.w) * 4,
                                static_cast<const BYTE*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                                static_cast<size_t>(cfg.w) * 4);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
            }
            ctx->Unmap(staging.Get(), 0);
            check(hr, "MFCreateMemoryBuffer");
            check(MFCreateSample(&s), "MFCreateSample");
            s->AddBuffer(buf.Get());
        }
        const int64_t t0 = frameTime(tick, cfg.fps);
        const int64_t t1 = frameTime(tick + 1, cfg.fps);
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        std::lock_guard<std::mutex> g(writeMutex);
        check(writer->WriteSample(videoStream, s.Get()), "WriteSample(video)");
        return true;
    }

    // Writes interleaved 16-bit stereo frames at the audio frame index "pos".
    void writeAudio(const int16_t* pcm, uint32_t frames, uint64_t pos)
    {
        if (frames == 0)
            return;
        const DWORD bytes = frames * 4;
        ComPtr<IMFMediaBuffer> buf;
        check(MFCreateMemoryBuffer(bytes, &buf), "MFCreateMemoryBuffer(audio)");
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        if (pcm)
            std::memcpy(dst, pcm, bytes);
        else
            std::memset(dst, 0, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ComPtr<IMFSample> s;
        check(MFCreateSample(&s), "MFCreateSample");
        s->AddBuffer(buf.Get());
        const int64_t rate = cfg.audioRate;
        const int64_t t0 = frameTime(static_cast<int64_t>(pos), rate);
        const int64_t t1 = frameTime(static_cast<int64_t>(pos + frames), rate);
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        std::lock_guard<std::mutex> g(writeMutex);
        check(writer->WriteSample(audioStream, s.Get()), "WriteSample(audio)");
    }

    void finish()
    {
        std::lock_guard<std::mutex> g(writeMutex);
        check(writer->Finalize(), "Finalize");
    }

    UINT32 bitrate = 0;
    std::string encoderName = "unknown";
    bool encoderHardware = false;
    std::string transformChain;
    uint64_t poolEmptyDrops = 0;

private:
    void describeEncoder()
    {
        ComPtr<IMFSinkWriterEx> ex;
        if (FAILED(writer.As(&ex)))
            return;
        for (DWORD i = 0; i < 16; ++i)
        {
            GUID cat{};
            ComPtr<IMFTransform> t;
            if (FAILED(ex->GetTransformForStream(videoStream, i, &cat, &t)) || !t)
                break;
            std::string name;
            bool hw = false;
            ComPtr<IMFAttributes> a;
            if (SUCCEEDED(t->GetAttributes(&a)) && a)
            {
                WCHAR buf[256] = {};
                UINT32 len = 0;
                if (SUCCEEDED(a->GetString(MFT_FRIENDLY_NAME_Attribute, buf, 256, &len)))
                    name = toUtf8(buf);
                UINT32 asyncFlag = 0;
                if (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag)) && asyncFlag)
                    hw = true;
                WCHAR url[256] = {};
                if (SUCCEEDED(a->GetString(MFT_ENUM_HARDWARE_URL_Attribute, url, 256, &len)))
                    hw = true;
                WCHAR vendor[64] = {};
                if (name.empty() && SUCCEEDED(a->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, vendor, 64, &len)))
                    name = "hardware transform, vendor id " + toUtf8(vendor);
            }
            const bool isEncoder = (cat == MFT_CATEGORY_VIDEO_ENCODER);
            if (!transformChain.empty())
                transformChain += " -> ";
            transformChain += (isEncoder ? "[encoder] " : "[other] ") + (name.empty() ? std::string("(unnamed)") : name)
                              + (hw ? " (hw)" : " (sw)");
            if (isEncoder)
            {
                encoderName = name.empty() ? "(unnamed)" : name;
                encoderHardware = hw;
            }
        }
    }

    Config cfg{};
    ComPtr<IMFDXGIDeviceManager> devMgr;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> ctx;
    DWORD videoStream = 0, audioStream = 0;
    std::mutex writeMutex;
};

//==============================================================================
// Audio: WASAPI loopback of the default render endpoint, silence-filled

class LoopbackAudio
{
public:
    // Returns false (with a message already printed) if audio cannot be recorded.
    bool init()
    {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
            return fail("cannot create the device enumerator");
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            return fail("no default render endpoint");
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
            return fail("cannot activate IAudioClient");
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix)
            return fail("GetMixFormat failed");
        rate = mix->nSamplesPerSec;
        channels = mix->nChannels;
        bits = mix->wBitsPerSample;
        isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        const bool formatOk = (isFloat && bits == 32) || (!isFloat && bits == 16);
        if (rate != 44100 && rate != 48000)
        {
            std::printf("Audio: endpoint rate is %u Hz, only 44100 and 48000 are supported. Recording without audio.\n", rate);
            CoTaskMemFree(mix);
            return false;
        }
        if (!formatOk || channels == 0)
        {
            std::printf("Audio: unsupported mix format (%u bits, float=%d, %u channels). Recording without audio.\n",
                        bits, isFloat ? 1 : 0, channels);
            CoTaskMemFree(mix);
            return false;
        }
        const HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                              2000000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr))
            return fail("IAudioClient::Initialize(loopback) failed");
        if (FAILED(client->GetService(IID_PPV_ARGS(&capture))))
            return fail("GetService(IAudioCaptureClient) failed");
        return true;
    }

    // The thread starts draining at once but discards everything until the origin is set.
    void start(Encoder* enc, uint64_t totalFramesToWrite)
    {
        encoder = enc;
        total = totalFramesToWrite;
        client->Start();
        thread = std::thread([this] { run(); });
    }

    void setOrigin(int64_t qpc) { originQpc.store(qpc); }

    void stopAndFlush()
    {
        stopFlag = true;
        if (thread.joinable())
            thread.join();
        client->Stop();
        // Pad the tail with silence so the audio track ends exactly where the video does.
        fillTo(total, true);
    }

    uint32_t rate = 0;
    uint64_t framesWritten = 0;   // total frames in the track, audio plus silence
    uint64_t silenceFrames = 0;   // of which inserted as silence
    uint64_t realFrames = 0;

private:
    bool fail(const char* why)
    {
        std::printf("Audio: %s. Recording without audio.\n", why);
        return false;
    }

    void emit(const int16_t* pcm, uint64_t frames, bool silence)
    {
        while (frames > 0 && framesWritten < total)
        {
            const uint64_t n = std::min<uint64_t>({ frames, total - framesWritten, rate });
            encoder->writeAudio(pcm, static_cast<uint32_t>(n), framesWritten);
            framesWritten += n;
            if (silence)
                silenceFrames += n;
            else
                realFrames += n;
            frames -= n;
            if (pcm)
                pcm += n * 2;
        }
    }

    void fillTo(uint64_t target, bool)
    {
        target = std::min(target, total);
        if (target > framesWritten)
            emit(nullptr, target - framesWritten, true);
    }

    void convert(const BYTE* data, uint32_t frames, bool silent, std::vector<int16_t>& out)
    {
        out.assign(static_cast<size_t>(frames) * 2, 0);
        if (silent || !data)
            return;
        auto clip = [](float v) {
            v = std::max(-1.0f, std::min(1.0f, v));
            return static_cast<int16_t>(std::lrintf(v * 32767.0f));
        };
        for (uint32_t i = 0; i < frames; ++i)
        {
            float ch[8] = {};
            const uint32_t nc = std::min<uint32_t>(channels, 8);
            for (uint32_t c = 0; c < nc; ++c)
            {
                if (isFloat)
                    ch[c] = reinterpret_cast<const float*>(data)[static_cast<size_t>(i) * channels + c];
                else
                    ch[c] = reinterpret_cast<const int16_t*>(data)[static_cast<size_t>(i) * channels + c] / 32768.0f;
            }
            float l, r;
            if (channels == 1)
                l = r = ch[0];
            else if (channels == 2)
            {
                l = ch[0];
                r = ch[1];
            }
            else
            {
                // Layout FL FR FC LFE BL BR SL SR; fold centre and surrounds into the front pair.
                l = ch[0] + 0.707f * ch[2];
                r = ch[1] + 0.707f * ch[2];
                if (nc > 5) { l += 0.5f * ch[4]; r += 0.5f * ch[5]; }
                if (nc > 7) { l += 0.5f * ch[6]; r += 0.5f * ch[7]; }
                l *= 0.5f;
                r *= 0.5f;
            }
            out[static_cast<size_t>(i) * 2] = clip(l);
            out[static_cast<size_t>(i) * 2 + 1] = clip(r);
        }
    }

    void run()
    {
        const int64_t tolerance = rate / 100;      // 10 ms: treat as contiguous
        const int64_t idleMargin = rate * 3 / 100; // 30 ms: how far behind "now" idle silence may reach
        std::vector<int16_t> pcm;
        while (!stopFlag)
        {
            Sleep(4);
            for (;;)
            {
                UINT32 n = 0;
                if (FAILED(capture->GetNextPacketSize(&n)) || n == 0)
                    break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos)))
                    break;
                const int64_t origin = originQpc.load();
                if (origin != 0 && frames > 0)
                {
                    convert(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, pcm);
                    const int64_t rel100 = static_cast<int64_t>(qpcPos) - qpcTo100ns(origin);
                    int64_t start = (rel100 * static_cast<int64_t>(rate)) / 10000000;
                    int64_t skip = 0;
                    const int64_t written = static_cast<int64_t>(framesWritten);
                    if (start > written + tolerance)
                        emit(nullptr, static_cast<uint64_t>(start - written), true);  // gap: silence
                    else if (start < written - tolerance)
                        skip = written - start;  // overlaps what was already written
                    if (skip < static_cast<int64_t>(frames))
                        emit(pcm.data() + skip * 2, frames - static_cast<uint64_t>(skip), false);
                }
                capture->ReleaseBuffer(frames);
            }
            const int64_t origin = originQpc.load();
            if (origin != 0)
            {
                // Loopback is silent when nothing plays: advance the track from the clock.
                const int64_t nowFrames = ((qpcTo100ns(qpcNow()) - qpcTo100ns(origin)) * static_cast<int64_t>(rate)) / 10000000
                                          - idleMargin;
                if (nowFrames > static_cast<int64_t>(framesWritten))
                    emit(nullptr, static_cast<uint64_t>(nowFrames) - framesWritten, true);
            }
        }
    }

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    uint32_t channels = 0, bits = 0;
    bool isFloat = true;
    Encoder* encoder = nullptr;
    uint64_t total = 0;
    std::thread thread;
    std::atomic<bool> stopFlag{ false };
    std::atomic<int64_t> originQpc{ 0 };
};

//==============================================================================

HANDLE makeTimer()
{
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t)
        t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    return t;
}

std::string describeEncoderHardwareList()
{
    std::string out;
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                            nullptr, &outType, &acts, &count)))
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            WCHAR nm[256] = {};
            UINT32 len = 0;
            if (SUCCEEDED(acts[i]->GetString(MFT_FRIENDLY_NAME_Attribute, nm, 256, &len)))
                out += (out.empty() ? "" : "; ") + toUtf8(nm);
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return out;
}

int run(const Options& opt)
{
    if (opt.list)
    {
        printList();
        return 0;
    }

    // Pick the capture target.
    wgc::GraphicsCaptureItem item{ nullptr };
    std::string targetDesc;
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (!opt.windowSubstr.empty())
    {
        const std::wstring needle = lower(opt.windowSubstr);
        HWND found = nullptr;
        std::wstring title;
        for (const auto& w : listWindows())
            if (lower(w.title).find(needle) != std::wstring::npos) { found = w.h; title = w.title; break; }
        if (!found)
            throw std::runtime_error("no visible window title contains that text (try --list)");
        check(interop->CreateForWindow(found, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)),
              "CreateForWindow");
        targetDesc = "window \"" + toUtf8(title) + "\"";
    }
    else
    {
        const auto mons = listMonitors();
        HMONITOR hm = nullptr;
        if (opt.monitor < 0)
            hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        else if (static_cast<size_t>(opt.monitor) < mons.size())
            hm = mons[static_cast<size_t>(opt.monitor)].h;
        else
            throw std::runtime_error("monitor index out of range (try --list)");
        check(interop->CreateForMonitor(hm, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)),
              "CreateForMonitor");
        targetDesc = opt.monitor < 0 ? "primary monitor" : "monitor " + std::to_string(opt.monitor);
    }

    // Direct3D 11 device with video and BGRA support, shared by capture and the encoder.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    {
        const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                fl, 2, D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(device.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
    }

    Capturer cap(device, context, item);
    const int w = cap.encW, h = cap.encH;

    // Audio probe before the writer exists, because the writer needs to know the rate.
    LoopbackAudio audio;
    bool haveAudio = false;
    if (opt.audio)
        haveAudio = audio.init();

    auto encPtr = std::make_unique<Encoder>();
    Encoder::Config cfg{ w, h, opt.fps, !opt.cpu, true, haveAudio, haveAudio ? audio.rate : 48000, opt.out, device.Get() };
    try
    {
        encPtr->open(cfg);
    }
    catch (const std::exception& e)
    {
        std::printf("Encoder open failed (%s). Retrying with software transforms.\n", e.what());
        encPtr = std::make_unique<Encoder>();
        cfg.hardware = false;
        encPtr->open(cfg);  // a failed open leaves a partial file; this overwrites it
    }
    Encoder& enc = *encPtr;

    if (!cap.waitFirstFrame(2000))
        throw std::runtime_error("no frame arrived from the capture source within 2 s (nothing changing on a window?)");

    const int64_t fps = opt.fps;
    const int64_t totalTicks = static_cast<int64_t>(opt.seconds) * fps;
    HANDLE timer = makeTimer();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    timeBeginPeriod(1);

    if (haveAudio)
        audio.start(&enc, static_cast<uint64_t>(opt.seconds) * audio.rate);

    const int64_t freq = qpcFreq();
    const int64_t origin = qpcNow();
    if (haveAudio)
        audio.setOrigin(origin);

    uint64_t submitted = 0, repeats = 0, lateTicks = 0, dropped = 0;
    const int64_t wallStart = qpcNow();
    for (int64_t i = 0; i < totalTicks; ++i)
    {
        const int64_t due = origin + (i * freq) / fps;
        for (;;)
        {
            const int64_t rem = due - qpcNow();
            if (rem <= 0)
                break;
            if (rem > freq / 500 && timer)
            {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -((rem - freq / 1000) * 10000000 / freq);  // wake ~1 ms early
                SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 50);
            }
            else
                YieldProcessor();
        }
        if (qpcNow() - due > freq / fps)
            ++lateTicks;
        bool repeated = false;
        if (enc.submitVideo(cap, i, repeated))
        {
            ++submitted;
            if (repeated)
                ++repeats;
        }
        else
            ++dropped;
    }

    // Let the audio thread catch up to the end of the video before closing it.
    const int64_t endQpc = origin + (totalTicks * freq) / fps + freq / 20;
    while (qpcNow() < endQpc)
        Sleep(2);
    timeEndPeriod(1);

    cap.stop();
    if (haveAudio)
        audio.stopAndFlush();
    enc.finish();
    const double wall = static_cast<double>(qpcNow() - wallStart) / static_cast<double>(freq);
    if (timer)
        CloseHandle(timer);

    // Report.
    uint64_t size = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(opt.out.c_str(), GetFileExInfoStandard, &fa))
            size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }
    std::printf("---- report ----\n");
    std::printf("target:            %s\n", targetDesc.c_str());
    std::printf("path:              %s\n", opt.cpu ? "CPU readback (RGB32 system memory)" : "GPU (DXGI samples)");
    std::printf("resolution:        %dx%d @ %d fps, %.1f Mbps target\n", w, h, opt.fps, enc.bitrate / 1e6);
    std::printf("capture border off: %s\n", cap.borderOff ? "yes" : "no (IsBorderRequired(false) refused)");
    std::printf("ticks submitted:   %llu of %lld (dropped %llu, pool-empty drops %llu)\n",
                static_cast<unsigned long long>(submitted), static_cast<long long>(totalTicks),
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(enc.poolEmptyDrops));
    std::printf("repeated ticks:    %llu\n", static_cast<unsigned long long>(repeats));
    std::printf("captured frames:   %llu, never used: %llu, size changes: %llu\n",
                static_cast<unsigned long long>(cap.captured.load()),
                static_cast<unsigned long long>(cap.unusedFrames.load()),
                static_cast<unsigned long long>(cap.resizes.load()));
    std::printf("late ticks:        %llu (woke more than one frame late)\n", static_cast<unsigned long long>(lateTicks));
    std::printf("encoder:           %s, %s\n", enc.encoderHardware ? "hardware" : "software", enc.encoderName.c_str());
    std::printf("transform chain:   %s\n", enc.transformChain.empty() ? "(none reported)" : enc.transformChain.c_str());
    const std::string hwList = describeEncoderHardwareList();
    std::printf("hw H.264 encoders: %s\n", hwList.empty() ? "(none enumerated)" : hwList.c_str());
    if (haveAudio)
        std::printf("audio:             %u Hz stereo AAC, %llu frames written (%llu real, %llu silence inserted)\n", audio.rate,
                    static_cast<unsigned long long>(audio.framesWritten),
                    static_cast<unsigned long long>(audio.realFrames),
                    static_cast<unsigned long long>(audio.silenceFrames));
    else
        std::printf("audio:             none\n");
    std::printf("wall time:         %.3f s\n", wall);
    std::printf("output:            %s (%llu bytes)\n", toUtf8(opt.out).c_str(), static_cast<unsigned long long>(size));
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDPIAware();  // so --list shows physical pixels, matching what the capture delivers
    Options opt;
    if (!parseArgs(argc, argv, opt))
    {
        std::printf("usage: screen_spike [--list] [--monitor N | --window \"title substring\"] [--seconds S]\n"
                    "                    [--fps F] [--out path.mp4] [--no-audio] [--cpu]\n");
        return 2;
    }
    int rc = 1;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        check(MFStartup(MF_VERSION), "MFStartup");
        rc = run(opt);
        MFShutdown();
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("error: WinRT 0x%08lX %s\n", static_cast<unsigned long>(e.code().value), toUtf8(e.message().c_str()).c_str());
    }
    catch (const std::exception& e)
    {
        std::printf("error: %s\n", e.what());
    }
    return rc;
}
