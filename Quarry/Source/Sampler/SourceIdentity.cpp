//
// Asking Windows what an application is.
//

#include "SourceIdentity.h"

#if JUCE_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

// Before uiautomation.h, and both are needed. WIN32_LEAN_AND_MEAN leaves the OLE headers
// out of windows.h, so `interface` is undefined and UIAutomationCore.h fails on its very
// first forward declaration; oleacc.h supplies the IAccessible it goes on to reference.
#include <objbase.h>
#include <oleacc.h>

#include <tlhelp32.h>
#include <uiautomation.h>

#if defined(_MSC_VER)
 #pragma comment(lib, "gdi32.lib")
 #pragma comment(lib, "user32.lib")
 #pragma comment(lib, "ole32.lib")
#endif

// Windows 8.1 and later. Spelled out rather than raising the SDK target, because everything
// else here works without it and a missing constant should not cost the whole feature.
#ifndef PW_RENDERFULLCONTENT
 #define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace quarry::sampler
{

namespace
{
/** A minimal owning COM pointer, the same twenty lines as the kit's, kept local so this file
    does not depend on a capture header for a smart pointer. */
template <typename ComClass>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComClass* get() const noexcept { return ptr; }
    ComClass* operator->() const noexcept { return ptr; }
    explicit operator bool() const noexcept { return ptr != nullptr; }

    ComClass** put() noexcept
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

struct WindowSearch
{
    DWORD processId = 0;
    HWND best = nullptr;
    long long bestArea = 0;
};

BOOL CALLBACK pickLargestWindow(HWND window, LPARAM userData)
{
    auto* search = reinterpret_cast<WindowSearch*>(userData);

    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);

    if (owner != search->processId || ! IsWindowVisible(window))
        return TRUE;

    // Owned windows are tooltips, popups and menus. The title is on the frame.
    if (GetWindow(window, GW_OWNER) != nullptr || GetWindowTextLengthW(window) == 0)
        return TRUE;

    RECT bounds {};
    if (! GetWindowRect(window, &bounds))
        return TRUE;

    const auto area = (long long) (bounds.right - bounds.left) * (long long) (bounds.bottom - bounds.top);

    if (area > search->bestArea)
    {
        search->bestArea = area;
        search->best = window;
    }

    return TRUE;
}

HWND largestWindowOwnedBy(DWORD processId)
{
    if (processId == 0)
        return nullptr;

    WindowSearch search;
    search.processId = processId;
    EnumWindows(pickLargestWindow, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

DWORD parentOf(DWORD processId)
{
    if (processId == 0)
        return 0;

    auto* snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    DWORD parent = 0;

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == processId)
            {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return parent;
}

/** The window to describe a capture by: the process's own, or the nearest ancestor's. */
HWND windowFor(juce::uint32 processId)
{
    auto current = (DWORD) processId;

    for (int generation = 0; generation < 4 && current != 0; ++generation)
    {
        if (auto* window = largestWindowOwnedBy(current))
            return window;

        current = parentOf(current);
    }

    return nullptr;
}

juce::String titleOf(HWND window)
{
    if (window == nullptr)
        return {};

    wchar_t title[512] = {};
    const auto length = GetWindowTextW(window, title, (int) juce::numElementsInArray(title));

    return length > 0 ? juce::String(title) : juce::String();
}

/** Text that looks like somewhere you could go. The omnibox holds a search query just as
    happily as an address, and a search for "drum loop" is not a URL. */
bool looksLikeAnAddress(const juce::String& text)
{
    if (text.isEmpty() || text.containsChar(' '))
        return false;

    return text.startsWithIgnoreCase("http://") || text.startsWithIgnoreCase("https://")
        || (text.containsChar('.') && ! text.startsWithChar('.'));
}

/**
 * The address bar of a browser window, via UI Automation.
 *
 * Deliberately searches the children of the window rather than every descendant. The
 * omnibox is browser chrome, a few levels down at most, while a descendant search can walk
 * into the rendered page and take as long as the page is large. This runs when a take
 * starts, so it has to be quick or not happen.
 */
juce::String addressBarOf(HWND window)
{
    if (window == nullptr)
        return {};

    ComPtr<IUIAutomation> automation;

    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.put()))))
        return {};

    ComPtr<IUIAutomationElement> root;

    if (FAILED(automation->ElementFromHandle(window, root.put())) || ! root)
        return {};

    VARIANT wanted {};
    wanted.vt = VT_I4;
    wanted.lVal = UIA_EditControlTypeId;

    ComPtr<IUIAutomationCondition> isEdit;

    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, wanted, isEdit.put())))
        return {};

    ComPtr<IUIAutomationElementArray> edits;

    if (FAILED(root->FindAll(TreeScope_Subtree, isEdit.get(), edits.put())) || ! edits)
        return {};

    int count = 0;

    if (FAILED(edits->get_Length(&count)))
        return {};

    // Every edit box, not the first one.
    //
    // The omnibox is reliably first today, but asking UI Automation anything is what makes
    // Chrome switch on renderer accessibility, and from the next call onwards the page's own
    // fields are in this tree too. Taking whatever came first would eventually mean reading
    // a text box on a web page. So: skip anything marked as a password outright, and accept
    // only a value that actually parses as an address.
    for (int i = 0; i < count && i < 32; ++i)
    {
        ComPtr<IUIAutomationElement> edit;

        if (FAILED(edits->GetElement(i, edit.put())) || ! edit)
            continue;

        BOOL isPassword = FALSE;

        if (FAILED(edit->get_CurrentIsPassword(&isPassword)) || isPassword)
            continue;

        ComPtr<IUIAutomationValuePattern> value;

        if (FAILED(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.put()))) || ! value)
            continue;

        BSTR raw = nullptr;

        if (FAILED(value->get_CurrentValue(&raw)) || raw == nullptr)
            continue;

        const juce::String text = juce::String(raw).trim();
        SysFreeString(raw);

        if (! looksLikeAnAddress(text))
            continue;

        // Chrome hides the scheme in the omnibox. Put back what it is showing you, not what
        // it literally said, so the sidecar holds something you can paste.
        return text.startsWithIgnoreCase("http") ? text : "https://" + text;
    }

    return {};
}
} // namespace

//==============================================================================
SourceIdentity identifySource(juce::uint32 processId)
{
    SourceIdentity identity;

    auto* window = windowFor(processId);

    if (window == nullptr)
        return identity;

    identity.windowTitle = titleOf(window);
    identity.url = addressBarOf(window);

    return identity;
}

juce::Image captureWindowImage(juce::uint32 processId)
{
    auto* window = windowFor(processId);

    if (window == nullptr)
        return {};

    RECT bounds {};

    if (! GetWindowRect(window, &bounds))
        return {};

    const auto width = (int) (bounds.right - bounds.left);
    const auto height = (int) (bounds.bottom - bounds.top);

    // A minimised window reports a nonsense rectangle, and an enormous one is a bug
    // somewhere else that should not become an enormous allocation here.
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return {};

    auto* screenDc = GetDC(nullptr);

    if (screenDc == nullptr)
        return {};

    juce::Image result;

    if (auto* memoryDc = CreateCompatibleDC(screenDc))
    {
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // negative: top down, so no flip afterwards
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;

        if (auto* bitmap = CreateDIBSection(memoryDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0))
        {
            auto* previous = SelectObject(memoryDc, bitmap);

            // PW_RENDERFULLCONTENT is what makes this work on a hardware composited window.
            // Without it a browser comes back black, which is the only kind of screenshot
            // worse than none.
            const auto drawn = PrintWindow(window, memoryDc, PW_RENDERFULLCONTENT) != 0;

            if (drawn && pixels != nullptr)
            {
                juce::Image grabbed(juce::Image::ARGB, width, height, false);
                const juce::Image::BitmapData data(grabbed, juce::Image::BitmapData::writeOnly);

                const auto* source = static_cast<const uint8_t*>(pixels);

                for (int y = 0; y < height; ++y)
                {
                    const auto* row = source + (size_t) y * (size_t) width * 4;
                    auto* destination = data.getLinePointer(y);

                    for (int x = 0; x < width; ++x)
                    {
                        // BGRA out of GDI, and its alpha is meaningless here: PrintWindow
                        // leaves it zero on plenty of windows, which would save a picture
                        // that is entirely transparent.
                        destination[0] = row[(size_t) x * 4 + 0];
                        destination[1] = row[(size_t) x * 4 + 1];
                        destination[2] = row[(size_t) x * 4 + 2];
                        destination[3] = 0xff;
                        destination += 4;
                    }
                }

                constexpr int thumbnailWidth = 480;

                result = width > thumbnailWidth
                           ? grabbed.rescaled(thumbnailWidth,
                                              juce::jmax(1, height * thumbnailWidth / width),
                                              juce::Graphics::highResamplingQuality)
                           : grabbed;
            }

            SelectObject(memoryDc, previous);
            DeleteObject(bitmap);
        }

        DeleteDC(memoryDc);
    }

    ReleaseDC(nullptr, screenDc);
    return result;
}

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
