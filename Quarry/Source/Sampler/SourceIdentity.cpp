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

#include <vector>

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

    /** In z-order, topmost first, because that is the order EnumWindows walks in. */
    std::vector<HWND> candidates;
};

BOOL CALLBACK collectWindows(HWND window, LPARAM userData)
{
    auto* search = reinterpret_cast<WindowSearch*>(userData);

    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);

    if (owner != search->processId || ! IsWindowVisible(window))
        return TRUE;

    // Owned windows are tooltips, popups and menus. The title is on the frame.
    if (GetWindow(window, GW_OWNER) != nullptr || GetWindowTextLengthW(window) == 0)
        return TRUE;

    search->candidates.push_back(window);
    return TRUE;
}

struct WindowChoice
{
    HWND window = nullptr;

    /** The process owned more than one window that could have been the source. */
    bool ambiguous = false;
};

/**
 * The window to speak for a process, and whether that choice was really a choice.
 *
 * This used to take the largest window, which is not a signal: a browser with two windows
 * open decided it on a few percent of area, so the capture got named after whichever one
 * happened to be dragged wider. Z-order is a real signal, weak but real, because activating
 * a window raises it: among one application's windows the topmost is the one most recently
 * used, and that is the likeliest one to have been playing.
 *
 * The foreground window would be a better answer still, and is deliberately not used: this
 * runs when a take starts, moments after the record button was clicked, so the window in
 * front is always Quarry's own.
 *
 * When there is more than one candidate none of this rises above a guess, which is what
 * `ambiguous` is for. Saying "possibly this" is worth much more than confidently saying the
 * wrong thing.
 */
WindowChoice chooseWindowOwnedBy(DWORD processId)
{
    if (processId == 0)
        return {};

    WindowSearch search;
    search.processId = processId;
    EnumWindows(collectWindows, reinterpret_cast<LPARAM>(&search));

    if (search.candidates.empty())
        return {};

    WindowChoice choice;
    choice.window = search.candidates.front();
    choice.ambiguous = search.candidates.size() > 1;
    return choice;
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

/** An HWND back out of the integer it travels as, or null if it has since closed. */
HWND asWindow(juce::uint64 handle)
{
    auto* window = reinterpret_cast<HWND>((juce::pointer_sized_uint) handle);
    return (window != nullptr && IsWindow(window)) ? window : nullptr;
}

/** The window to describe a capture by: the one chosen, or the best guess at one. */
WindowChoice windowFor(juce::uint32 processId, juce::uint64 chosen)
{
    // A window somebody actually picked is not a guess, and stands as long as it is still
    // open. Only when it has closed under us does this fall back to guessing.
    if (auto* window = asWindow(chosen))
        return { window, false };

    auto current = (DWORD) processId;

    for (int generation = 0; generation < 4 && current != 0; ++generation)
    {
        if (const auto choice = chooseWindowOwnedBy(current); choice.window != nullptr)
            return choice;

        current = parentOf(current);
    }

    return {};
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
std::vector<SourceWindow> windowsOfSource(juce::uint32 processId)
{
    auto current = (DWORD) processId;

    for (int generation = 0; generation < 4 && current != 0; ++generation)
    {
        WindowSearch search;
        search.processId = current;
        EnumWindows(collectWindows, reinterpret_cast<LPARAM>(&search));

        if (! search.candidates.empty())
        {
            std::vector<SourceWindow> found;
            found.reserve(search.candidates.size());

            for (auto* window : search.candidates)
                found.push_back({ (juce::uint64) reinterpret_cast<juce::pointer_sized_uint>(window),
                                  titleOf(window) });

            return found;
        }

        current = parentOf(current);
    }

    return {};
}

SourceIdentity identifySource(juce::uint32 processId, juce::uint64 windowHandle)
{
    SourceIdentity identity;

    const auto choice = windowFor(processId, windowHandle);

    if (choice.window == nullptr)
        return identity;

    identity.windowTitle = titleOf(choice.window);
    identity.windowTitleIsAmbiguous = choice.ambiguous;
    identity.url = addressBarOf(choice.window);

    return identity;
}

juce::Image captureWindowImage(juce::uint32 processId, juce::uint64 windowHandle)
{
    auto* window = windowFor(processId, windowHandle).window;

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
