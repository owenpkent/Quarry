#include "UpdateInstaller.h"

#if JUCE_WINDOWS

// After JuceHeader.h, and only here: windows.h defines macros (TRANSPARENT) and a global
// Rectangle that collide with the UI headers if it spreads.
#include <windows.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <mutex>

namespace quarry::update
{
namespace
{
constexpr juce::int64 kMaxInstallerBytes = 300ll * 1024 * 1024;
constexpr juce::int64 kMaxChecksumBytes = 1024;
constexpr int kMaxHops = 5;
constexpr int kTimeoutMs = 15000;

// The installer, held open from before it is hashed until the process exits. Share mode read
// only means nobody can write to it, rename it or delete it while we hold it, so the bytes that
// were hashed are the bytes Windows runs. Process lifetime on purpose: the handle outlives the
// editor that started the update, and the OS closes it when we quit.
struct Pin
{
    HANDLE handle {INVALID_HANDLE_VALUE};
    juce::File file;
    juce::String version;

    void release()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        version = {};
    }
};

std::mutex gPinMutex;

Pin& pin()
{
    static Pin* p = new Pin(); // never destroyed, see above
    return *p;
}

void log(const juce::String& inText)
{
    juce::Logger::writeToLog("Update: " + inText);
}

// Reads a pinned file through the handle that pins it, rather than reopening it by path.
class HandleInputStream final : public juce::InputStream
{
public:
    explicit HandleInputStream(HANDLE inHandle)
        : mHandle(inHandle)
    {
        LARGE_INTEGER size {};
        if (GetFileSizeEx(inHandle, &size))
            mSize = size.QuadPart;
    }

    juce::int64 getTotalLength() override { return mSize; }
    bool isExhausted() override { return mPosition >= mSize; }
    juce::int64 getPosition() override { return mPosition; }

    bool setPosition(juce::int64 inNewPosition) override
    {
        LARGE_INTEGER to {};
        to.QuadPart = inNewPosition;
        if (!SetFilePointerEx(mHandle, to, nullptr, FILE_BEGIN))
            return false;
        mPosition = inNewPosition;
        return true;
    }

    int read(void* outBuffer, int inMaxBytes) override
    {
        DWORD got = 0;
        if (!ReadFile(mHandle, outBuffer, static_cast<DWORD>(inMaxBytes), &got, nullptr))
            return 0;
        mPosition += got;
        return static_cast<int>(got);
    }

private:
    HANDLE mHandle;
    juce::int64 mSize {0};
    juce::int64 mPosition {0};
};

juce::String findHeader(const juce::StringPairArray& inHeaders, const juce::String& inName)
{
    for (const auto& key: inHeaders.getAllKeys()) {
        if (key.equalsIgnoreCase(inName))
            return inHeaders[key].trim();
    }
    return {};
}

// Opens a download, following redirects by hand so that every hop is checked against the
// allowlist before a connection is made to it.
std::unique_ptr<juce::InputStream> openFollowing(const juce::String& inUrl, juce::String& outError)
{
    auto current = inUrl;

    for (int hop = 0; hop <= kMaxHops; ++hop) {
        if (!isSafeDownloadUrl(current)) {
            log("refused URL " + current.substring(0, 200));
            outError = "Download refused";
            return nullptr;
        }

        int status = 0;
        juce::StringPairArray headers;
        auto stream = makeRawUrl(current).createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(kTimeoutMs)
                .withNumRedirectsToFollow(0)
                .withStatusCode(&status)
                .withResponseHeaders(&headers)
                .withExtraHeaders("Accept: application/octet-stream\r\n"));

        if (stream == nullptr) {
            log("no connection to " + hostOf(current));
            outError = "Download failed";
            return nullptr;
        }

        log("hop " + juce::String(hop) + ": " + hostOf(current) + " -> HTTP " + juce::String(status));

        if (status == 200)
            return stream;

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            auto location = findHeader(headers, "Location");
            if (location.startsWithChar('/') && !location.startsWith("//"))
                location = "https://" + hostOf(current) + location;
            current = location;
            continue;
        }

        log("HTTP " + juce::String(status) + " from " + hostOf(current));
        outError = "Download failed";
        return nullptr;
    }

    log("more than " + juce::String(kMaxHops) + " redirects");
    outError = "Download failed";
    return nullptr;
}

// Reads at most inMaxBytes, failing on more rather than truncating.
bool readSmall(const juce::String& inUrl, juce::int64 inMaxBytes, juce::String& outText, juce::String& outError)
{
    auto stream = openFollowing(inUrl, outError);
    if (stream == nullptr)
        return false;

    if (stream->getTotalLength() > inMaxBytes) {
        outError = "Download refused";
        return false;
    }

    juce::MemoryOutputStream body;
    body.writeFromInputStream(*stream, inMaxBytes + 1);
    if (static_cast<juce::int64>(body.getDataSize()) > inMaxBytes) {
        outError = "Download refused";
        return false;
    }

    outText = body.toUTF8();
    return true;
}

bool downloadToFile(const juce::String& inUrl,
                    const juce::File& inDestination,
                    const UpdateInstaller::ProgressCallback& inProgress,
                    const std::atomic<bool>& inCancel,
                    juce::String& outError)
{
    auto stream = openFollowing(inUrl, outError);
    if (stream == nullptr)
        return false;

    const auto total = stream->getTotalLength();
    if (total > kMaxInstallerBytes) {
        log("Content-Length " + juce::String(total) + " is over the cap");
        outError = "Download refused";
        return false;
    }

    juce::FileOutputStream out(inDestination);
    if (out.failedToOpen()) {
        log("cannot write " + inDestination.getFullPathName());
        outError = "Could not save the installer";
        return false;
    }
    out.setPosition(0);
    out.truncate();

    juce::HeapBlock<char> buffer(65536);
    juce::int64 written = 0;

    for (;;) {
        if (inCancel.load()) {
            outError = "Cancelled";
            return false;
        }

        const auto got = stream->read(buffer, 65536);
        if (got < 0 || (got == 0 && !stream->isExhausted())) {
            log("read failed after " + juce::String(written) + " bytes");
            outError = "Download failed";
            return false;
        }
        if (got == 0)
            break;

        // Checked as it arrives, because Content-Length is the server's word and absent on a chunked reply.
        written += got;
        if (written > kMaxInstallerBytes) {
            outError = "Download refused";
            return false;
        }

        if (!out.write(buffer, static_cast<size_t>(got))) {
            outError = "Could not save the installer";
            return false;
        }

        if (inProgress && total > 0)
            inProgress(static_cast<double>(written) / static_cast<double>(total));
    }

    out.flush();

    if (total > 0 && written != total) {
        log("short download: " + juce::String(written) + " of " + juce::String(total));
        outError = "Download failed";
        return false;
    }
    return written > 0;
}

// X.Y.Z.W from the fixed part of the version resource, which is the binary the loader reads
// and not the display string next to it.
juce::String readFileVersion(const juce::File& inFile)
{
    const auto path = inFile.getFullPathName().toWideCharPointer();
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0)
        return {};

    juce::HeapBlock<char> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data))
        return {};

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data, L"\\", reinterpret_cast<LPVOID*>(&info), &length) || info == nullptr
        || length < sizeof(VS_FIXEDFILEINFO))
        return {};

    return juce::String(HIWORD(info->dwFileVersionMS)) + "." + juce::String(LOWORD(info->dwFileVersionMS)) + "."
           + juce::String(HIWORD(info->dwFileVersionLS)) + "." + juce::String(LOWORD(info->dwFileVersionLS));
}

juce::File sharedVst3Folder()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &raw)) || raw == nullptr)
        return {};
    const juce::File common {juce::String(raw)};
    CoTaskMemFree(raw);
    return common.getChildFile("VST3");
}

juce::File freshDownloadFolder()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);

    // Earlier updates' folders. One that still holds a pinned installer refuses to go, which is fine.
    for (const auto& old: temp.findChildFiles(juce::File::findDirectories, false, "QuarryUpdate-*"))
        old.deleteRecursively();

    auto folder = temp.getChildFile("QuarryUpdate-" + juce::String::toHexString(juce::Random().nextInt64()));
    return folder.createDirectory().wasOk() ? folder : juce::File();
}
} // namespace

bool UpdateInstaller::fetchSmallText(const juce::String& inUrl,
                                     juce::int64 inMaxBytes,
                                     juce::String& outText,
                                     juce::String& outError)
{
    return readSmall(inUrl, inMaxBytes, outText, outError);
}

InstallerResult UpdateInstaller::prepare(const ReleaseInfo& inRelease,
                                         const ProgressCallback& inProgress,
                                         const std::atomic<bool>& inCancel)
{
    std::lock_guard<std::mutex> lock(gPinMutex);

    if (!inRelease.hasInstallableAssets())
        return {false, "No installer in this release"};

    auto& held = pin();
    if (held.handle != INVALID_HANDLE_VALUE && held.version == inRelease.version)
        return {true, {}};

    held.release();

    juce::String error;
    const auto installer_name = installerFileName(inRelease.version);

    juce::String checksum_text;
    if (!readSmall(inRelease.checksumUrl, kMaxChecksumBytes, checksum_text, error))
        return {false, error};

    const auto expected_hash = parseChecksumFile(checksum_text, installer_name);
    if (expected_hash.isEmpty()) {
        log("checksum file is not in the expected format");
        return {false, "Checksum file invalid"};
    }

    const auto folder = freshDownloadFolder();
    if (folder == juce::File())
        return {false, "Could not save the installer"};

    const auto installer = folder.getChildFile(installer_name);

    if (!downloadToFile(inRelease.installerUrl, installer, inProgress, inCancel, error)) {
        installer.deleteFile();
        return {false, error};
    }

    // Pin first, hash second: the hash is of the bytes that are now locked in place.
    const HANDLE handle = CreateFileW(installer.getFullPathName().toWideCharPointer(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        log("could not pin the installer, error " + juce::String(static_cast<int>(GetLastError())));
        installer.deleteFile();
        return {false, "Could not verify the installer"};
    }

    held.handle = handle;
    held.file = installer;

    HandleInputStream pinned_stream(handle);
    const auto actual_hash = juce::SHA256(pinned_stream).toHexString();

    if (actual_hash != expected_hash) {
        log("checksum mismatch: wanted " + expected_hash + ", got " + actual_hash);
        held.release();
        installer.deleteFile();
        return {false, "Checksum did not match"};
    }

    // A correctly hashed old installer served under a new name would pass the check above if the
    // release's checksum file were regenerated for it; its own version resource still gives it away.
    const auto file_version = readFileVersion(installer);
    if (file_version != expectedFileVersion(inRelease.version)) {
        log("installer reports version \"" + file_version + "\", wanted " + expectedFileVersion(inRelease.version));
        held.release();
        installer.deleteFile();
        return {false, "Installer version did not match"};
    }

    held.version = inRelease.version;
    if (inProgress)
        inProgress(1.0);
    return {true, {}};
}

juce::String UpdateInstaller::findHostHoldingPlugin()
{
    const auto binary = sharedVst3Folder().getChildFile("Quarry.vst3/Contents/x86_64-win/Quarry.vst3");
    if (!binary.existsAsFile())
        return {};

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS)
        return {};

    juce::String names;
    const auto binary_path = binary.getFullPathName();
    LPCWSTR path = binary_path.toWideCharPointer();
    bool ok = RmRegisterResources(session, 1, &path, 0, nullptr, 0, nullptr) == ERROR_SUCCESS;

    if (ok) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        // The first call only sizes the list; it can grow between calls, so ask again if it did.
        DWORD result = RmGetList(session, &needed, &count, nullptr, &reasons);
        for (int attempt = 0; attempt < 3 && result == ERROR_MORE_DATA; ++attempt) {
            juce::HeapBlock<RM_PROCESS_INFO> list(needed);
            count = needed;
            result = RmGetList(session, &needed, &count, list, &reasons);
            if (result == ERROR_SUCCESS) {
                for (UINT i = 0; i < count; ++i) {
                    if (list[i].Process.dwProcessId == GetCurrentProcessId())
                        continue;
                    const auto name = juce::String(list[i].strAppName).trim();
                    if (name.isNotEmpty() && !names.containsWholeWord(name))
                        names << (names.isEmpty() ? "" : ", ") << name;
                }
            }
        }
        if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA)
            log("Restart Manager list failed, error " + juce::String(static_cast<int>(result)));
    } else {
        log("Restart Manager could not register the plugin");
    }

    RmEndSession(session);
    return names;
}

bool UpdateInstaller::isPluginInstalled()
{
    return sharedVst3Folder().getChildFile("Quarry.vst3").exists();
}

InstallerResult UpdateInstaller::launch()
{
    std::lock_guard<std::mutex> lock(gPinMutex);

    auto& held = pin();
    if (held.handle == INVALID_HANDLE_VALUE || !held.file.existsAsFile())
        return {false, "Installer not ready"};

    const auto parameters =
        installerParameters(static_cast<int>(GetCurrentProcessId()), isPluginInstalled());

    // "open", not "runas". The installer asks for administrator rights itself, which leaves this
    // process unelevated, and that is what lets it start Quarry again as the ordinary user.
    SHELLEXECUTEINFOW info {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = held.file.getFullPathName().toWideCharPointer();
    info.lpParameters = parameters.toWideCharPointer();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        const auto error = GetLastError();
        log("ShellExecuteEx failed, error " + juce::String(static_cast<int>(error)));
        return {false, error == ERROR_CANCELLED ? "Install cancelled" : "Could not start the installer"};
    }

    return {true, {}};
}
} // namespace quarry::update

#endif // JUCE_WINDOWS
