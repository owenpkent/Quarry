//
// Recording a window, or the main screen, to an mp4 beside a take.
//
// The video path is the one proven in tools/screen_spike: Windows.Graphics.Capture into a
// texture this code owns, a counter-paced thread that submits the newest frame at exactly the
// frame rate (repeating it when nothing changed), and a Media Foundation sink writer that
// encodes H.264 and AAC. Windows APIs only; nothing here is taken from any other project.
//
// The sound is not the spike's. It arrives from the Sample page's recorder through
// loopbackBlock(), so the wav and the mp4 hold the same samples, and its rules are the ones
// a music tool needs: samples are counted and never edited once the stream has started.
//

// Before any JUCE include, deliberately: C++/WinRT wants to see the Windows headers first, and
// this file mentions juce:: by name everywhere rather than using the namespace, which is what
// keeps windows.h's Rectangle() from making juce::Rectangle ambiguous.
//
// Which is why the outer guard is the compiler's _WIN32 and not JUCE_WINDOWS: the sources are
// globbed on every platform, and JUCE's macro does not exist yet this high in the file.
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS

#include <windows.h>

#include <codecapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <juce_audio_basics/juce_audio_basics.h>

#include "ScreenRecorder.h"

#if JUCE_WINDOWS

#include "ScreenMath.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace quarry::sampler
{

namespace
{

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3 = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace sm = screenmath;

//==============================================================================
// Small helpers

void mfCheck(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s failed, HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buffer);
    }
}

juce::int64 qpcNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

juce::int64 qpcFrequency()
{
    static const juce::int64 f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }();
    return f;
}

/** 100 ns time of item n at the given rate, computed from n each time so durations never
    accumulate rounding. */
juce::int64 timeOf(juce::int64 n, juce::int64 rate)
{
    return (n * 10000000) / rate;
}

/** The documented COM interface for getting the D3D texture behind a WinRT Direct3D surface. */
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(REFIID id, void** object) = 0;
};

struct TimerHandle
{
    HANDLE handle = nullptr;

    TimerHandle()
    {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (handle == nullptr)
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    ~TimerHandle()
    {
        if (handle != nullptr)
            CloseHandle(handle);
    }
};

/** Holds the system timer at 1 ms for as long as it lives, so the pacing wait can sleep. */
struct HighResolutionTimerScope
{
    HighResolutionTimerScope() { timeBeginPeriod(1); }
    ~HighResolutionTimerScope() { timeEndPeriod(1); }
};

//==============================================================================
// Capture: Windows.Graphics.Capture into a texture this code owns

class Capturer
{
public:
    Capturer(ComPtr<ID3D11Device> dev, ComPtr<ID3D11DeviceContext> ctx, wgc::GraphicsCaptureItem it)
        : device(std::move(dev)), context(std::move(ctx)), item(std::move(it))
    {
        ComPtr<IDXGIDevice> dxgi;
        mfCheck(device.As(&dxgi), "QI IDXGIDevice");
        winrt::com_ptr<::IInspectable> inspectable;
        mfCheck(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "CreateDirect3D11DeviceFromDXGIDevice");
        winrtDevice = inspectable.as<wgd3::IDirect3DDevice>();

        const auto size = item.Size();
        const auto encode = sm::encodeSize(size.Width, size.Height);
        encW = encode.width;
        encH = encode.height;

        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(encW);
        d.Height = static_cast<UINT>(encH);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        mfCheck(device->CreateTexture2D(&d, nullptr, &latest), "CreateTexture2D(latest)");
        clearLatest();

        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        poolW = size.Width;
        poolH = size.Height;
        session = pool.CreateCaptureSession(item);

        // Both are refused on some Windows builds, and neither is worth refusing the take for.
        try { session.IsCursorCaptureEnabled(true); } catch (...) {}
        try { session.IsBorderRequired(false); } catch (...) {}

        frameRevoker = pool.FrameArrived(winrt::auto_revoke,
            [this](wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
                onFrame(sender);
            });
        closedRevoker = item.Closed(winrt::auto_revoke,
            [this](wgc::GraphicsCaptureItem const&, winrt::Windows::Foundation::IInspectable const&) {
                sourceClosed.store(true);
            });
        session.StartCapture();
    }

    ~Capturer() { stop(); }

    void stop()
    {
        if (stopped)
            return;

        stopped = true;
        frameRevoker.revoke();
        closedRevoker.revoke();

        try { session.Close(); } catch (...) {}
        try { pool.Close(); } catch (...) {}
    }

    /** Waits for the first frame, but not past an abandon or a stop request. */
    bool waitFirstFrame(int milliseconds, const std::atomic<bool>& abandon, const std::atomic<bool>& stop)
    {
        return sm::waitUntilReady(
            milliseconds,
            [this] { std::lock_guard<std::mutex> g(m); return haveFrame; },
            [&] { return abandon.load() || stop.load(); });
    }

    /** Copies the newest frame into `dst`, a texture the caller owns. False if there has been
        no frame yet. `repeated` is set when that frame was already submitted once. */
    bool copyLatestTo(ID3D11Texture2D* dst, UINT dstSub, bool& repeated)
    {
        std::lock_guard<std::mutex> g(m);

        if (! haveFrame)
            return false;

        repeated = used;
        used = true;
        context->CopySubresourceRegion(dst, dstSub, 0, 0, 0, latest.Get(), 0, nullptr);
        context->Flush();
        return true;
    }

    int encW = 0, encH = 0;
    std::atomic<bool> sourceClosed { false };

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

            if (! frame)
                return;

            try
            {
                const auto contentSize = frame.ContentSize();
                ComPtr<ID3D11Texture2D> texture;
                IDxgiInterfaceAccess* raw = nullptr;
                mfCheck(winrt::get_unknown(frame.Surface())->QueryInterface(__uuidof(IDxgiInterfaceAccess),
                            reinterpret_cast<void**>(&raw)), "QI IDirect3DDxgiInterfaceAccess");
                ComPtr<IDxgiInterfaceAccess> access;
                access.Attach(raw);
                mfCheck(access->GetInterface(IID_PPV_ARGS(&texture)), "GetInterface(texture)");

                {
                    std::lock_guard<std::mutex> g(m);

                    // The window changed size after the first frame. The encode size is fixed,
                    // so a larger window is clipped and a smaller one is padded with black.
                    if (contentSize.Width != encW || contentSize.Height != encH)
                        clearLatest();

                    // The surface is still the size of the pool it came from, which after a
                    // resize can be smaller than the content it reports, so the copy is bounded
                    // by the surface as well. Such a transition frame copies what fits.
                    D3D11_TEXTURE2D_DESC sourceDesc{};
                    texture->GetDesc(&sourceDesc);
                    const auto extent = sm::copyExtent({ contentSize.Width, contentSize.Height },
                                                       { static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height) },
                                                       { encW, encH });

                    if (extent.width > 0 && extent.height > 0)
                    {
                        D3D11_BOX box{};
                        box.right = static_cast<UINT>(extent.width);
                        box.bottom = static_cast<UINT>(extent.height);
                        box.back = 1;
                        context->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
                        used = false;
                        haveFrame = true;
                    }
                }

                if (contentSize.Width != poolW || contentSize.Height != poolH)
                {
                    poolW = contentSize.Width;
                    poolH = contentSize.Height;
                    sender.Recreate(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
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
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker;
    wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
    ComPtr<ID3D11Texture2D> latest;
    int poolW = 0, poolH = 0;
    std::mutex m;
    bool haveFrame = false;
    bool used = false;
    bool stopped = false;
};

//==============================================================================
// Muxer: one IMFSinkWriter with an H.264 video stream and an AAC audio stream

class Encoder
{
public:
    struct Config
    {
        int w = 0, h = 0, fps = 0;
        bool gpu = true;
        bool hardware = true;
        UINT32 audioRate = 48000;
        std::wstring path;
        ID3D11Device* device = nullptr;
    };

    void open(const Config& c)
    {
        cfg = c;

        ComPtr<IMFAttributes> attributes;
        mfCheck(MFCreateAttributes(&attributes, 6), "MFCreateAttributes");
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, c.hardware ? TRUE : FALSE);
        attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        if (c.gpu)
        {
            UINT token = 0;
            mfCheck(MFCreateDXGIDeviceManager(&token, &devMgr), "MFCreateDXGIDeviceManager");
            mfCheck(devMgr->ResetDevice(c.device, token), "IMFDXGIDeviceManager::ResetDevice");
            attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devMgr.Get());
        }

        mfCheck(MFCreateSinkWriterFromURL(c.path.c_str(), nullptr, attributes.Get(), &writer), "MFCreateSinkWriterFromURL");

        ComPtr<IMFMediaType> videoOut;
        mfCheck(MFCreateMediaType(&videoOut), "MFCreateMediaType");
        videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        videoOut->SetUINT32(MF_MT_AVG_BITRATE, sm::bitrate(c.w, c.h, c.fps));
        videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        videoOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(videoOut.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(c.w), static_cast<UINT32>(c.h));
        MFSetAttributeRatio(videoOut.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(c.fps), 1);
        MFSetAttributeRatio(videoOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        mfCheck(writer->AddStream(videoOut.Get(), &videoStream), "AddStream(video)");

        // The GPU path hands over BGRA textures, the CPU path RGB32 memory.
        ComPtr<IMFMediaType> videoIn;
        mfCheck(MFCreateMediaType(&videoIn), "MFCreateMediaType");
        videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoIn->SetGUID(MF_MT_SUBTYPE, c.gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32);
        videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        videoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(c.w * 4));  // positive: top-down
        MFSetAttributeSize(videoIn.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(c.w), static_cast<UINT32>(c.h));
        MFSetAttributeRatio(videoIn.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(c.fps), 1);
        MFSetAttributeRatio(videoIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        mfCheck(writer->SetInputMediaType(videoStream, videoIn.Get(), nullptr), "SetInputMediaType(video)");

        ComPtr<IMFMediaType> audioOut;
        mfCheck(MFCreateMediaType(&audioOut), "MFCreateMediaType");
        audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, c.audioRate);
        audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
        audioOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        audioOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        mfCheck(writer->AddStream(audioOut.Get(), &audioStream), "AddStream(audio)");

        ComPtr<IMFMediaType> audioIn;
        mfCheck(MFCreateMediaType(&audioIn), "MFCreateMediaType");
        audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, c.audioRate);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, c.audioRate * 4);
        mfCheck(writer->SetInputMediaType(audioStream, audioIn.Get(), nullptr), "SetInputMediaType(audio)");

        if (c.gpu)
        {
            // A recycled pool of DXGI-backed samples. The sink writer returns a sample to the
            // pool when it has finished with it, so each tick owns its texture until then.
            mfCheck(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator)), "MFCreateVideoSampleAllocatorEx");
            mfCheck(allocator->SetDirectXManager(devMgr.Get()), "SetDirectXManager");
            ComPtr<IMFAttributes> allocatorAttributes;
            mfCheck(MFCreateAttributes(&allocatorAttributes, 3), "MFCreateAttributes");
            allocatorAttributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
            allocatorAttributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
            allocatorAttributes->SetUINT32(MF_SA_BUFFERS_PER_SAMPLE, 1);
            mfCheck(allocator->InitializeSampleAllocatorEx(4, 12, allocatorAttributes.Get(), videoIn.Get()),
                    "InitializeSampleAllocatorEx");
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
            mfCheck(c.device->CreateTexture2D(&d, nullptr, &staging), "CreateTexture2D(staging)");
            c.device->GetImmediateContext(&ctx);
        }

        mfCheck(writer->BeginWriting(), "BeginWriting");
    }

    /** Submits one video frame at tick time. False if it had to be dropped. */
    bool submitVideo(Capturer& capturer, juce::int64 tick)
    {
        ComPtr<IMFSample> sample;
        bool repeated = false;

        if (cfg.gpu)
        {
            HRESULT hr = E_FAIL;

            for (int tries = 0; tries < 100; ++tries)
            {
                hr = allocator->AllocateSample(&sample);

                if (SUCCEEDED(hr))
                    break;

                Sleep(1);
            }

            if (FAILED(hr))
                return false;

            ComPtr<IMFMediaBuffer> buffer;
            mfCheck(sample->GetBufferByIndex(0, &buffer), "GetBufferByIndex");
            ComPtr<IMFDXGIBuffer> dxgiBuffer;
            mfCheck(buffer.As(&dxgiBuffer), "QI IMFDXGIBuffer");
            ComPtr<ID3D11Texture2D> texture;
            mfCheck(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)), "IMFDXGIBuffer::GetResource");
            UINT subresource = 0;
            dxgiBuffer->GetSubresourceIndex(&subresource);

            if (! capturer.copyLatestTo(texture.Get(), subresource, repeated))
                return false;

            buffer->SetCurrentLength(static_cast<DWORD>(cfg.w * cfg.h * 4));
        }
        else
        {
            if (! capturer.copyLatestTo(staging.Get(), 0, repeated))
                return false;

            D3D11_MAPPED_SUBRESOURCE map{};
            mfCheck(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map(staging)");
            const DWORD bytes = static_cast<DWORD>(cfg.w * cfg.h * 4);
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);

            if (SUCCEEDED(hr))
            {
                BYTE* dst = nullptr;
                buffer->Lock(&dst, nullptr, nullptr);

                for (int y = 0; y < cfg.h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(cfg.w) * 4,
                                static_cast<const BYTE*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                                static_cast<size_t>(cfg.w) * 4);

                buffer->Unlock();
                buffer->SetCurrentLength(bytes);
            }

            ctx->Unmap(staging.Get(), 0);
            mfCheck(hr, "MFCreateMemoryBuffer");
            mfCheck(MFCreateSample(&sample), "MFCreateSample");
            sample->AddBuffer(buffer.Get());
        }

        const auto t0 = timeOf(tick, cfg.fps);
        const auto t1 = timeOf(tick + 1, cfg.fps);
        sample->SetSampleTime(t0);
        sample->SetSampleDuration(t1 - t0);
        mfCheck(writer->WriteSample(videoStream, sample.Get()), "WriteSample(video)");
        return true;
    }

    /** Writes interleaved 16 bit stereo at audio frame index `position`; null pcm is silence. */
    void writeAudio(const juce::int16* pcm, juce::uint32 frames, juce::uint64 position)
    {
        if (frames == 0)
            return;

        const DWORD bytes = frames * 4;
        ComPtr<IMFMediaBuffer> buffer;
        mfCheck(MFCreateMemoryBuffer(bytes, &buffer), "MFCreateMemoryBuffer(audio)");
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);

        if (pcm != nullptr)
            std::memcpy(dst, pcm, bytes);
        else
            std::memset(dst, 0, bytes);

        buffer->Unlock();
        buffer->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        mfCheck(MFCreateSample(&sample), "MFCreateSample");
        sample->AddBuffer(buffer.Get());
        const juce::int64 rate = cfg.audioRate;
        const auto t0 = timeOf(static_cast<juce::int64>(position), rate);
        const auto t1 = timeOf(static_cast<juce::int64>(position + frames), rate);
        sample->SetSampleTime(t0);
        sample->SetSampleDuration(t1 - t0);
        mfCheck(writer->WriteSample(audioStream, sample.Get()), "WriteSample(audio)");
    }

    void finalize() { mfCheck(writer->Finalize(), "Finalize"); }

private:
    Config cfg;
    ComPtr<IMFDXGIDeviceManager> devMgr;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> ctx;
    DWORD videoStream = 0, audioStream = 0;
};

//==============================================================================
// Sound: sample-rate conversion, which only the odd endpoint rates need

/** Turns interleaved stereo float at one rate into interleaved stereo float at another, in a
    stream. Kept apart so that it can be reasoned about on its own.

    The AAC encoder takes 44100 or 48000, and an endpoint can mix at 96000 or 192000. Input is
    held back until enough has arrived for the interpolator to read without running off the
    end, and whatever it did not consume is carried to the next call, so the stream has no
    seam at a block boundary. */
class StereoResampler
{
public:
    StereoResampler(double inputRate, double outputRate)
        : ratio(inputRate / outputRate)
    {
    }

    /** Appends the output for `frames` more input frames to `out`, interleaved. */
    void process(const float* interleaved, int frames, std::vector<float>& out)
    {
        for (int i = 0; i < frames; ++i)
        {
            pending[0].push_back(interleaved[2 * i]);
            pending[1].push_back(interleaved[2 * i + 1]);
        }

        // Two frames of margin, so the interpolator never reads past what has arrived.
        const auto available = static_cast<int>(pending[0].size());
        const auto produce = static_cast<int>(std::floor((static_cast<double>(available) - 2.0) / ratio));

        if (produce <= 0)
            return;

        int used = 0;

        for (int c = 0; c < 2; ++c)
        {
            scratch[c].assign(static_cast<size_t>(produce), 0.0f);
            used = interpolator[c].process(ratio, pending[c].data(), scratch[c].data(), produce);
        }

        used = std::min(used, available);

        for (int c = 0; c < 2; ++c)
            pending[c].erase(pending[c].begin(), pending[c].begin() + used);

        const auto base = out.size();
        out.resize(base + static_cast<size_t>(produce) * 2);

        for (int i = 0; i < produce; ++i)
        {
            out[base + static_cast<size_t>(i) * 2] = scratch[0][static_cast<size_t>(i)];
            out[base + static_cast<size_t>(i) * 2 + 1] = scratch[1][static_cast<size_t>(i)];
        }
    }

private:
    double ratio;
    juce::WindowedSincInterpolator interpolator[2];
    std::vector<float> pending[2];
    std::vector<float> scratch[2];
};

} // namespace

//==============================================================================
struct ScreenRecorder::Impl
{
    explicit Impl(ScreenRecorder& o) : owner(o) {}

    ScreenRecorder& owner;

    // Set by start(), read by the worker.
    Target target;
    juce::File tempFile;
    double inRate = 48000.0;
    int outRate = 48000;

    std::thread thread;
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> abandon { false };

    // The hand-off. Written by the capture thread only while `accepting`, read by the worker.
    // When the first block began is what the start alignment needs, and is written before the
    // block is published.
    juce::AbstractFifo fifo { 4 };
    std::vector<float> fifoData;   // interleaved stereo, 2 floats per frame
    std::atomic<bool> accepting { false };
    std::atomic<juce::int64> originQpc { 0 };
    std::atomic<juce::int64> firstBlockStartQpc { 0 };
    std::atomic<juce::int64> firstBlockFrames { 0 };

    // Silence the worker wrote for a stalled source, in the source's frames, which the capture
    // thread takes back out of the silence the source hands over late. Added to by the worker,
    // spent or cleared by the capture thread.
    std::atomic<juce::int64> silenceCredit { 0 };

    // Written by the worker before it exits, read by finish() after the join.
    Finished result;

    void prepare(const Target& t, const juce::File& file, double sampleRate)
    {
        target = t;
        tempFile = file;
        inRate = sampleRate;
        outRate = (sampleRate == 44100.0 || sampleRate == 48000.0) ? static_cast<int>(sampleRate) : 48000;

        stopRequested.store(false);
        abandon.store(false);
        accepting.store(false);
        originQpc.store(0);
        firstBlockStartQpc.store(0);
        firstBlockFrames.store(0);
        silenceCredit.store(0);
        result = Finished();

        // Nothing is in flight to the FIFO now: it is only touched while `accepting`, and that
        // is false until this take's worker sets it, after the capture thread of the last take
        // was joined by the recorder.
        const auto capacity = std::max(256, static_cast<int>(std::ceil(sm::fifoSeconds * sampleRate)));
        fifo.setTotalSize(capacity);
        fifo.reset();
        fifoData.assign(static_cast<size_t>(capacity) * 2, 0.0f);
    }

    //==========================================================================
    // Sound on the worker: everything from the FIFO to the AAC stream.
    struct AudioPipe
    {
        AudioPipe(Impl& i, Encoder& e)
            : impl(i), encoder(e), resample(i.inRate != static_cast<double>(i.outRate)),
              resampler(i.inRate, static_cast<double>(i.outRate))
        {
        }

        Impl& impl;
        Encoder& encoder;
        const bool resample;
        StereoResampler resampler;

        juce::int64 written = 0;        // frames in the track, at the output rate
        bool started = false;
        double lastDeliverySeconds = 0.0;  // picture time when the source last delivered anything
        juce::int64 dropRemaining = 0;  // frames of the first block still to skip, at the input rate

        std::vector<float> in, converted;
        std::vector<juce::int16> pcm;

        void writeSilence(juce::int64 frames)
        {
            while (frames > 0)
            {
                const auto n = std::min<juce::int64>(frames, impl.outRate);
                encoder.writeAudio(nullptr, static_cast<juce::uint32>(n), static_cast<juce::uint64>(written));
                written += n;
                frames -= n;
            }
        }

        void writeFloat(const float* interleaved, int frames)
        {
            const float* source = interleaved;
            int count = frames;

            if (resample)
            {
                converted.clear();
                resampler.process(interleaved, frames, converted);
                source = converted.data();
                count = static_cast<int>(converted.size() / 2);
            }

            if (count <= 0)
                return;

            pcm.resize(static_cast<size_t>(count) * 2);

            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = sm::floatToInt16(source[i]);

            encoder.writeAudio(pcm.data(), static_cast<juce::uint32>(count), static_cast<juce::uint64>(written));
            written += count;
        }

        /** Moves everything the capture thread has delivered into the stream. With `discard`
            it is emptied and thrown away, for a picture that has already died. `videoSeconds`
            is where the picture has got to, noted whenever the source delivers. */
        void drain(bool discard, double videoSeconds)
        {
            for (;;)
            {
                const auto ready = impl.fifo.getNumReady();

                if (ready <= 0)
                    break;

                lastDeliverySeconds = videoSeconds;

                const auto n = std::min(ready, 4096);
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                impl.fifo.prepareToRead(n, start1, size1, start2, size2);

                if (! discard)
                {
                    in.resize(static_cast<size_t>(size1 + size2) * 2);
                    std::memcpy(in.data(), impl.fifoData.data() + static_cast<size_t>(start1) * 2,
                                static_cast<size_t>(size1) * 2 * sizeof(float));

                    if (size2 > 0)
                        std::memcpy(in.data() + static_cast<size_t>(size1) * 2,
                                    impl.fifoData.data() + static_cast<size_t>(start2) * 2,
                                    static_cast<size_t>(size2) * 2 * sizeof(float));
                }

                impl.fifo.finishedRead(size1 + size2);

                if (discard)
                    continue;

                const auto frames = size1 + size2;

                // The first block after the origin decides how the sound lines up with the
                // picture. Unless the source was already silent for so long that the catch-up
                // rule started the stream, in which case the sound simply follows on.
                if (! started)
                {
                    started = true;
                    const auto align = sm::alignFirstBlockAt(impl.originQpc.load(), impl.firstBlockStartQpc.load(),
                                                             qpcFrequency(), impl.firstBlockFrames.load(), impl.inRate);

                    writeSilence(static_cast<juce::int64>(std::llround(
                        static_cast<double>(align.silenceFrames) * impl.outRate / impl.inRate)));
                    dropRemaining = align.dropFrames;
                }

                const auto skip = static_cast<int>(std::min<juce::int64>(dropRemaining, frames));
                dropRemaining -= skip;

                if (frames - skip > 0)
                    writeFloat(in.data() + static_cast<size_t>(skip) * 2, frames - skip);
            }
        }

        /** The catch-up rule: a source that has stopped delivering is filled as the picture
            goes on, so the file never waits on sound that is not coming. See sm::stallFill
            for why a stall is "nothing delivered", not "behind".

            What is written here is owed back. When the source wakes it hands over the missed
            silence itself before any sound, and the capture thread leaves out that much of it
            (sm::silenceAlreadyWritten), so the gap is in the track once and not twice. */
        void catchUp(double videoSeconds)
        {
            const auto silence = sm::stallFill(videoSeconds, lastDeliverySeconds, written, impl.outRate);

            if (silence > 0)
            {
                started = true;
                writeSilence(silence);
                impl.silenceCredit.fetch_add(static_cast<juce::int64>(std::llround(
                    static_cast<double>(silence) * impl.inRate / impl.outRate)));
            }
        }

        /** The end of the take: the sound is brought level with the picture, never past it. */
        void padTo(double videoSeconds)
        {
            const auto target = static_cast<juce::int64>(std::llround(videoSeconds * impl.outRate));

            if (target > written)
                writeSilence(target - written);
        }
    };

    //==========================================================================
    void run()
    {
        // C++/WinRT and Media Foundation both want to be started on the thread that uses
        // them, and shut down after every object they handed out has been released, which is
        // why the picture itself lives in a function of its own.
        bool apartment = false;

        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;

            mfCheck(MFStartup(MF_VERSION), "MFStartup");
            struct MediaFoundationScope
            {
                ~MediaFoundationScope() { MFShutdown(); }
            } mediaFoundation;

            runPicture();
        }
        catch (const winrt::hresult_error& e)
        {
            owner.raise("Windows refused it, code " + juce::String::toHexString(static_cast<int>(e.code().value)));
        }
        catch (const std::exception& e)
        {
            owner.raise(juce::String(e.what()));
        }
        catch (...)
        {
            owner.raise("an unknown error");
        }

        if (apartment)
            winrt::uninit_apartment();
    }

    // Everything that owns a Windows object lives inside this function, so that all of it has
    // been released by the time the caller shuts Media Foundation down.
    void runPicture()
    {
        bool wroteAnything = false;
        juce::int64 ticks = 0;
        juce::String stoppedBecause;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        // Direct3D 11 with video and BGRA support, shared by the capture and the encoder.
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            mfCheck(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                      levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context),
                    "D3D11CreateDevice");

            ComPtr<ID3D11Multithread> multithread;

            if (SUCCEEDED(device.As(&multithread)))
                multithread->SetMultithreadProtected(TRUE);
        }

        wgc::GraphicsCaptureItem item{ nullptr };
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

        if (target.windowHandle != 0)
            mfCheck(interop->CreateForWindow(reinterpret_cast<HWND>(static_cast<UINT_PTR>(target.windowHandle)),
                                             winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)),
                    "CreateForWindow");
        else
            mfCheck(interop->CreateForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                                              winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)),
                    "CreateForMonitor");

        Capturer capturer(device, context, item);

        // Hardware transforms first. If the open fails, software; if the GPU sample path
        // cannot be set up at all, CPU readback. A failed open leaves a partial file, which
        // the next attempt overwrites.
        Encoder encoder;
        {
            struct Attempt { bool gpu; bool hardware; };
            const Attempt attempts[] = { { true, true }, { true, false }, { false, false } };
            std::string lastError;
            bool opened = false;

            for (const auto& attempt : attempts)
            {
                try
                {
                    encoder = Encoder();
                    Encoder::Config config;
                    config.w = capturer.encW;
                    config.h = capturer.encH;
                    config.fps = sm::framesPerSecond;
                    config.gpu = attempt.gpu;
                    config.hardware = attempt.hardware;
                    config.audioRate = static_cast<UINT32>(outRate);
                    config.path = tempFile.getFullPathName().toWideCharPointer();
                    config.device = device.Get();
                    encoder.open(config);
                    opened = true;
                    break;
                }
                catch (const std::exception& e)
                {
                    lastError = e.what();
                }
            }

            if (! opened)
                throw std::runtime_error("could not open an encoder: " + lastError);
        }

        if (! capturer.waitFirstFrame(2000, abandon, stopRequested))
        {
            if (abandon.load())
                return;

            // Stopped before the picture began. The wait ends at once rather than running out
            // its two seconds, so the take is not held up (or its sound extended) behind it.
            if (stopRequested.load())
                throw std::runtime_error("the take was too short for a picture");

            throw std::runtime_error("nothing was drawn in two seconds");
        }

        const auto freq = qpcFrequency();
        const int fps = sm::framesPerSecond;
        TimerHandle timer;
        HighResolutionTimerScope timerScope;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        AudioPipe audio(*this, encoder);

        // The one origin the picture and the sound share. The sound is let in only after it
        // is set, so what arrived earlier is never half of the take.
        const auto origin = qpcNow();
        originQpc.store(origin);
        accepting.store(true);

        bool died = false;

        try
        {
            for (juce::int64 tick = 0; ! stopRequested.load(); ++tick)
            {
                const auto due = sm::tickDue(origin, tick, freq, fps);

                for (;;)
                {
                    if (stopRequested.load())
                        break;

                    const auto remaining = due - qpcNow();

                    if (remaining <= 0)
                        break;

                    if (remaining > freq / 500 && timer.handle != nullptr)
                    {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -((remaining - freq / 1000) * 10000000 / freq);  // wake about 1 ms early
                        SetWaitableTimer(timer.handle, &dueTime, 0, nullptr, nullptr, FALSE);
                        WaitForSingleObject(timer.handle, 50);
                    }
                    else
                    {
                        YieldProcessor();
                    }
                }

                if (stopRequested.load())
                    break;

                if (capturer.sourceClosed.load())
                    throw std::runtime_error("the window closed");

                if (encoder.submitVideo(capturer, tick))
                    wroteAnything = true;

                ticks = tick + 1;

                // Sound after picture each tick, and the catch-up last, once everything the
                // source has delivered is in.
                audio.drain(false, static_cast<double>(ticks) / fps);
                audio.catchUp(static_cast<double>(ticks) / fps);
            }
        }
        catch (const std::exception& e)
        {
            died = true;
            stoppedBecause = juce::String(e.what());
            owner.raise(stoppedBecause);
        }

        accepting.store(false);
        capturer.stop();

        if (abandon.load())
            return;

        if (! wroteAnything)
        {
            if (stoppedBecause.isEmpty())
                stoppedBecause = "the take was too short for a picture";

            owner.raise(stoppedBecause);
            return;
        }

        // The tail: what the source delivered last is written, then the sound is brought level
        // with where the picture ended. After a failure the FIFO is emptied unwritten, because
        // the writer may be what failed.
        try
        {
            audio.drain(died, static_cast<double>(ticks) / fps);
            audio.padTo(static_cast<double>(ticks) / fps);
            encoder.finalize();

            result.ok = true;
            result.file = tempFile;
            result.width = capturer.encW;
            result.height = capturer.encH;
            result.fps = fps;
            result.durationSec = static_cast<double>(ticks) / fps;
        }
        catch (const std::exception& e)
        {
            owner.raise(juce::String("the file could not be finished: ") + e.what());
        }
    }
};

//==============================================================================
ScreenRecorder::ScreenRecorder()
    : impl(std::make_unique<Impl>(*this))
{
}

ScreenRecorder::~ScreenRecorder()
{
    discard();
}

void ScreenRecorder::raise(const juce::String& reason)
{
    const juce::ScopedLock lock(failureLock);

    // The first reason is the cause; whatever follows is usually a consequence of it.
    if (failureText.isEmpty())
        failureText = reason;
}

juce::String ScreenRecorder::failure() const
{
    const juce::ScopedLock lock(failureLock);
    return failureText;
}

juce::Result ScreenRecorder::start(const Target& target, const juce::File& tempFile, double audioSampleRate)
{
    if (recording.load())
        return juce::Result::fail("Already recording the picture");

    if (audioSampleRate < 8000.0)
        return juce::Result::fail("No sound to go with the picture");

    if (target.windowHandle != 0)
    {
        const auto window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(target.windowHandle));

        if (! IsWindow(window))
            return juce::Result::fail("That window has closed");

        if (IsIconic(window))
            return juce::Result::fail("That window is minimized");
    }

    {
        const juce::ScopedLock lock(failureLock);
        failureText.clear();
    }

    droppedFrames.store(0);
    tempFile.deleteFile();
    impl->prepare(target, tempFile, audioSampleRate);

    try
    {
        impl->thread = std::thread([this] { impl->run(); });
    }
    catch (...)
    {
        return juce::Result::fail("Could not start the picture");
    }

    recording.store(true);
    return juce::Result::ok();
}

void ScreenRecorder::requestStop()
{
    // Both are atomics and nothing is joined, so this is as quick as the caller needs for it
    // to mark where the sound ends. The capture thread sees `accepting` on its next block.
    impl->accepting.store(false);
    impl->stopRequested.store(true);
}

void ScreenRecorder::stopWorker()
{
    requestStop();

    if (impl->thread.joinable())
        impl->thread.join();
}

ScreenRecorder::Finished ScreenRecorder::finish()
{
    Finished finished;

    if (! recording.load())
    {
        finished.message = "No picture was being recorded";
        return finished;
    }

    stopWorker();
    recording.store(false);

    finished = impl->result;

    if (! finished.ok)
    {
        const auto reason = failure();
        finished.message = reason.isNotEmpty() ? reason : juce::String("No picture was recorded");
        impl->tempFile.deleteFile();
        finished.file = juce::File();
    }

    return finished;
}

void ScreenRecorder::discard()
{
    impl->abandon.store(true);
    stopWorker();
    recording.store(false);

    if (impl->tempFile != juce::File())
        impl->tempFile.deleteFile();
}

//==============================================================================
void ScreenRecorder::loopbackBlock(const float* const* channels, int numChannels, int numSamples)
{
    loopbackBlockAt(channels, numChannels, numSamples, okstudio::capture::LoopbackBlockTime());
}

void ScreenRecorder::loopbackBlockAt(const float* const* channels, int numChannels, int numSamples,
                                     const okstudio::capture::LoopbackBlockTime& when)
{
    // The capture thread. Nothing here may block, allocate or wait: it converts, copies into
    // the FIFO and returns. Until the worker has set the origin, and once finish() has begun,
    // the block is not wanted and is ignored.
    if (! impl->accepting.load() || channels == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    // Time before the picture is not part of the take, and it is trimmed here, first, rather than
    // on the worker: the worker may already have begun filling a stall, and then it never
    // aligns a first block at all. Until the first block is published, the frames at the head of
    // a block that the stream dates before the origin are left out. A block wholly before it is
    // not a dropped block, so it is not counted. The origin is valid here because it is stored
    // before `accepting` is set. A block with no time from the stream is taken to end on
    // arrival, which is after the origin, so `early` is 0 for it and nothing changes.
    int early = 0;

    if (when.startQpc100ns > 0 && impl->firstBlockStartQpc.load(std::memory_order_relaxed) == 0)
        early = static_cast<int>(sm::framesBeforeOrigin(impl->originQpc.load(),
                                                         sm::qpcFrom100ns(when.startQpc100ns, qpcFrequency()),
                                                         qpcFrequency(), numSamples, impl->inRate));

    if (early >= numSamples)
        return;

    numSamples -= early;

    const auto usable = std::min(numChannels, 8);

    // The block as far as everything below is concerned: the same channels, past the early head.
    const float* head[8];

    for (int c = 0; c < usable; ++c)
        head[c] = channels[c] + early;

    // Silence the worker already wrote while this source was stalled is not written again.
    // Only a block of pure zeros is ever shortened, and the first block with sound in it ends
    // the credit, so no sound is ever left out. The scan only runs while there is credit.
    int skipped = 0;

    if (const auto credit = impl->silenceCredit.load(std::memory_order_relaxed); credit > 0)
    {
        if (sm::isSilent(head, usable, numSamples))
        {
            skipped = static_cast<int>(sm::silenceAlreadyWritten(credit, numSamples, true));
            impl->silenceCredit.fetch_sub(skipped, std::memory_order_relaxed);
        }
        else
        {
            impl->silenceCredit.store(0, std::memory_order_relaxed);
        }
    }

    numSamples -= skipped;

    if (numSamples <= 0)
        return;

    auto& fifo = impl->fifo;

    if (fifo.getFreeSpace() < numSamples)
    {
        droppedFrames.fetch_add(numSamples);
        return;
    }

    // When the first block began, not when it arrived: after an idle gap the stream hands over
    // the silence it missed in a burst, and that burst arrives long after the time it stands
    // for. Without a time from the stream the block is taken to have ended on arrival. A block
    // whose head was trimmed above now begins at the origin, so the worker neither drops nor
    // pads it a second time.
    if (impl->firstBlockStartQpc.load(std::memory_order_relaxed) == 0)
    {
        const auto start = early > 0 ? impl->originQpc.load()
                           : when.startQpc100ns > 0
                             ? sm::qpcFrom100ns(when.startQpc100ns, qpcFrequency())
                             : sm::blockStartFromArrival(qpcNow(), numSamples, impl->inRate, qpcFrequency());

        impl->firstBlockFrames.store(numSamples, std::memory_order_relaxed);
        impl->firstBlockStartQpc.store(std::max<juce::int64>(1, start), std::memory_order_relaxed);
    }

    constexpr int piece = 512;
    float stereo[piece * 2];
    const float* shifted[8];

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite(numSamples, start1, size1, start2, size2);

    int done = 0;

    while (done < size1 + size2)
    {
        const auto taking = std::min(piece, size1 + size2 - done);

        for (int c = 0; c < usable; ++c)
            shifted[c] = head[c] + skipped + done;

        sm::downmixToStereo(shifted, usable, taking, stereo);

        // Frame `done + i` of the block goes to index start1 + done + i, or into the second
        // region once the first is full.
        for (int i = 0; i < taking; ++i)
        {
            const auto frame = done + i;
            const auto index = frame < size1 ? start1 + frame : start2 + (frame - size1);
            impl->fifoData[static_cast<size_t>(index) * 2] = stereo[2 * i];
            impl->fifoData[static_cast<size_t>(index) * 2 + 1] = stereo[2 * i + 1];
        }

        done += taking;
    }

    fifo.finishedWrite(size1 + size2);
}

} // namespace quarry::sampler

#endif // JUCE_WINDOWS

#endif // _WIN32
