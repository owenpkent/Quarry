//
// Blocking client for the transcription sidecar protocol (version 1): newline-delimited JSON
// over a child process's stdio.
//

#include "SidecarClient.h"

#include <algorithm>
#include <cstring>

#if !JUCE_WINDOWS
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace
{
// Model loads are slow (multi-second, sometimes much more on first run while weights are
// fetched or a GPU context spins up), so the wait for "ready" is generous and separate from
// transcribe()'s own, per-request timeout.
constexpr int kReadyTimeoutMs = 120000;

// How long shutdown() waits for the child to exit on its own, after closing its stdin, before
// insisting.
constexpr int kShutdownWaitMs = 5000;
} // namespace

SidecarClient::SidecarClient(const juce::String& inCommandLine) : mCommandLine(inCommandLine) {}

SidecarClient::~SidecarClient()
{
    if (mStarted) {
        shutdown();
    }
}

bool SidecarClient::isRunning() const
{
#if JUCE_WINDOWS
    if (mProcessHandle == nullptr) {
        return false;
    }

    DWORD exit_code = 0;
    return GetExitCodeProcess(mProcessHandle, &exit_code) != FALSE && exit_code == STILL_ACTIVE;
#else
    if (mChildPid <= 0) {
        return false;
    }

    int status = 0;
    // WNOHANG: 0 means still running, >0 means it just exited (and got reaped here), <0 means it
    // was already reaped (by an earlier call) or never existed.
    return waitpid(mChildPid, &status, WNOHANG) == 0;
#endif
}

const juce::StringArray& SidecarClient::getAvailableEngines() const
{
    return mAvailableEngines;
}

bool SidecarClient::hasEngineList() const
{
    return mEngineListReported;
}

const juce::String& SidecarClient::getDevice() const
{
    return mDevice;
}

bool SidecarClient::_writeLine(const juce::String& inLine, juce::String& outError)
{
    // Named so the temporary outlives the raw pointer toRawUTF8() hands back.
    const juce::String line = inLine + "\n";
    const auto* payload = line.toRawUTF8();
    const auto num_bytes = static_cast<size_t>(strlen(payload));

#if JUCE_WINDOWS
    if (mChildStdinWrite == nullptr) {
        outError = "sidecar stdin is not open";
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(mChildStdinWrite, payload, static_cast<DWORD>(num_bytes), &written, nullptr)
        || written != num_bytes) {
        outError = "failed to write to sidecar stdin";
        return false;
    }
#else
    if (mChildStdinWrite < 0) {
        outError = "sidecar stdin is not open";
        return false;
    }

    size_t total_written = 0;

    while (total_written < num_bytes) {
        const auto n = ::write(mChildStdinWrite, payload + total_written, num_bytes - total_written);

        if (n <= 0) {
            outError = "failed to write to sidecar stdin";
            return false;
        }

        total_written += static_cast<size_t>(n);
    }
#endif

    return true;
}

bool SidecarClient::_readLine(juce::String& outLine, double inDeadlineStart, int inTimeoutMs, juce::String& outError)
{
    for (;;) {
        const auto newline_pos = mReadBuffer.find('\n');

        if (newline_pos != std::string::npos) {
            outLine = juce::String::fromUTF8(mReadBuffer.data(), static_cast<int>(newline_pos));
            mReadBuffer.erase(0, newline_pos + 1);
            return true;
        }

        if (juce::Time::getMillisecondCounterHiRes() - inDeadlineStart > static_cast<double>(inTimeoutMs)) {
            outError = "timed out waiting for the sidecar";
            return false;
        }

        char chunk[4096];

#if JUCE_WINDOWS
        DWORD available = 0;

        if (!PeekNamedPipe(mChildStdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
            outError = "sidecar stdout pipe closed";
            return false;
        }

        if (available == 0) {
            if (!isRunning()) {
                outError = "sidecar process exited unexpectedly";
                return false;
            }

            juce::Thread::sleep(1);
            continue;
        }

        DWORD num_read = 0;
        const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));

        if (!ReadFile(mChildStdoutRead, chunk, to_read, &num_read, nullptr) || num_read == 0) {
            outError = "failed to read sidecar stdout";
            return false;
        }

        mReadBuffer.append(chunk, num_read);
#else
        pollfd pfd{mChildStdoutRead, POLLIN, 0};
        // Short poll granularity, mirroring the Windows branch's 1 ms sleep: this loop's own
        // deadline check is what actually bounds the wait, not this number.
        const auto poll_result = ::poll(&pfd, 1, 5);

        if (poll_result < 0) {
            outError = "failed to poll sidecar stdout";
            return false;
        }

        if (poll_result == 0 || (pfd.revents & POLLIN) == 0) {
            if (!isRunning()) {
                outError = "sidecar process exited unexpectedly";
                return false;
            }

            continue;
        }

        const auto num_read = ::read(mChildStdoutRead, chunk, sizeof(chunk));

        if (num_read <= 0) {
            outError = "failed to read sidecar stdout";
            return false;
        }

        mReadBuffer.append(chunk, static_cast<size_t>(num_read));
#endif
    }
}

bool SidecarClient::_readResponse(const juce::String& inAwaitedId,
                                  juce::var& outResponse,
                                  int inTimeoutMs,
                                  juce::String& outError)
{
    const auto deadline_start = juce::Time::getMillisecondCounterHiRes();

    for (;;) {
        juce::String line;

        if (!_readLine(line, deadline_start, inTimeoutMs, outError)) {
            return false;
        }

        juce::var parsed;
        juce::JSON::parse(line, parsed);

        // Not a JSON object at all (parse failure, a bare array, stray non-protocol output on
        // stdout): garbage per the protocol's own terms. Skip and keep waiting.
        if (parsed.getDynamicObject() == nullptr) {
            continue;
        }

        if (parsed.getProperty("id", juce::var()).toString() != inAwaitedId) {
            continue;
        }

        outResponse = parsed;
        return true;
    }
}

void SidecarClient::_closeHandles()
{
#if JUCE_WINDOWS
    if (mChildStdinWrite != nullptr) {
        CloseHandle(mChildStdinWrite);
        mChildStdinWrite = nullptr;
    }

    if (mChildStdoutRead != nullptr) {
        CloseHandle(mChildStdoutRead);
        mChildStdoutRead = nullptr;
    }

    if (mProcessHandle != nullptr) {
        CloseHandle(mProcessHandle);
        mProcessHandle = nullptr;
    }
#else
    if (mChildStdinWrite >= 0) {
        close(mChildStdinWrite);
        mChildStdinWrite = -1;
    }

    if (mChildStdoutRead >= 0) {
        close(mChildStdoutRead);
        mChildStdoutRead = -1;
    }

    if (mChildPid > 0) {
        int status = 0;
        waitpid(mChildPid, &status, WNOHANG);
        mChildPid = -1;
    }
#endif
}

bool SidecarClient::start(juce::String& outError)
{
    if (mStarted) {
        outError = "sidecar already started";
        return false;
    }

#if JUCE_WINDOWS
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &security_attributes, 0)
        || !SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        outError = "failed to create sidecar stdin pipe";
        return false;
    }

    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &security_attributes, 0)
        || !SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        outError = "failed to create sidecar stdout pipe";
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        return false;
    }

    // stderr is logging per the protocol and must not block the sidecar even if nothing ever
    // looks at it. Sending it to NUL drains it for free, instead of a thread whose only job is
    // to discard a pipe.
    HANDLE nul_handle = CreateFileW(L"NUL",
                                    GENERIC_WRITE,
                                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                                    &security_attributes,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_stdin_read;
    startup_info.hStdOutput = child_stdout_write;
    startup_info.hStdError = nul_handle;

    PROCESS_INFORMATION process_info{};

    // CreateProcessW can write into its command-line argument, so this needs to be a real,
    // owned buffer and not a pointer into juce::String's own storage.
    const auto* wide_command_line = mCommandLine.toWideCharPointer();
    std::vector<wchar_t> command_buffer(wide_command_line, wide_command_line + mCommandLine.length() + 1);

    const auto created = CreateProcessW(nullptr,
                                        command_buffer.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup_info,
                                        &process_info)
        != FALSE;

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);

    if (nul_handle != nullptr && nul_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_handle);
    }

    if (!created) {
        outError = "failed to launch sidecar: " + mCommandLine;
        CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read);
        return false;
    }

    CloseHandle(process_info.hThread);

    mProcessHandle = process_info.hProcess;
    mChildStdinWrite = child_stdin_write;
    mChildStdoutRead = child_stdout_read;
#else
    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        outError = "failed to create sidecar pipes";
        return false;
    }

    const auto pid = fork();

    if (pid < 0) {
        outError = "failed to fork sidecar process";
        return false;
    }

    if (pid == 0) {
        // Child: wire the pipes onto the standard streams, discard stderr, and hand off to a
        // shell so the command line gets the same quoting/splitting a caller typing it would
        // expect.
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        const auto dev_null = open("/dev/null", O_WRONLY);

        if (dev_null >= 0) {
            dup2(dev_null, STDERR_FILENO);
        }

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        execl("/bin/sh", "sh", "-c", mCommandLine.toRawUTF8(), (char*) nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    mChildStdinWrite = stdin_pipe[1];
    mChildStdoutRead = stdout_pipe[0];
    mChildPid = pid;
#endif

    mStarted = true;

    const auto deadline_start = juce::Time::getMillisecondCounterHiRes();

    for (;;) {
        juce::String line;

        if (!_readLine(line, deadline_start, kReadyTimeoutMs, outError)) {
            _closeHandles();
            mStarted = false;
            return false;
        }

        juce::var parsed;
        juce::JSON::parse(line, parsed);

        if (parsed.getDynamicObject() == nullptr) {
            continue; // not a protocol line; keep waiting for "ready"
        }

        if (parsed.getProperty("event", juce::var()).toString() == "ready") {
            // Kept rather than dropped on the floor. This line is the only place the sidecar says
            // which engines its interpreter can import, and a picker that cannot see that has to
            // either offer every engine and let the failures teach the user, or offer none.
            mAvailableEngines.clear();
            mEngineListReported = false;

            if (const auto* engines = parsed.getProperty("engines", juce::var()).getArray()) {
                mEngineListReported = true;

                for (const auto& engine : *engines)
                    mAvailableEngines.add(engine.toString());
            }

            mDevice = parsed.getProperty("device", juce::var()).toString();

            return true;
        }
    }
}

bool SidecarClient::transcribe(const juce::File& inWav,
                               const juce::String& inEngine,
                               std::vector<SidecarNote>& outNotes,
                               std::vector<SidecarPedalEvent>& outPedal,
                               juce::String& outError,
                               int inTimeoutMs)
{
    outNotes.clear();
    outPedal.clear();

    if (!mStarted || !isRunning()) {
        outError = "sidecar is not running";
        return false;
    }

    const auto id = juce::String(++mNextId);

    auto* request = new juce::DynamicObject();
    request->setProperty("id", id);
    request->setProperty("cmd", "transcribe");
    request->setProperty("wav", inWav.getFullPathName());
    request->setProperty("engine", inEngine);
    request->setProperty("options", juce::var(new juce::DynamicObject()));

    if (!_writeLine(juce::JSON::toString(juce::var(request), true), outError)) {
        return false;
    }

    juce::var response;

    if (!_readResponse(id, response, inTimeoutMs, outError)) {
        return false;
    }

    if (!static_cast<bool>(response.getProperty("ok", false))) {
        outError = response.getProperty("error", "sidecar reported failure without a message").toString();
        return false;
    }

    // Named rather than chained straight into getArray(): var::getArray() hands back a pointer
    // into the var it was called on, so calling it on the unnamed temporary getProperty() returns
    // would leave that pointer dangling the moment the temporary is destroyed.
    const auto notes_property = response.getProperty("notes", juce::var());

    if (const auto* notes = notes_property.getArray()) {
        outNotes.reserve(static_cast<size_t>(notes->size()));

        for (const auto& entry: *notes) {
            if (entry.getDynamicObject() == nullptr) {
                continue;
            }

            SidecarNote note;
            note.onset = static_cast<double>(entry.getProperty("onset", 0.0));
            note.offset = static_cast<double>(entry.getProperty("offset", 0.0));
            note.pitch = static_cast<int>(entry.getProperty("pitch", 0));

            const auto velocity = entry.getProperty("velocity", juce::var());
            note.velocity = velocity.isVoid() ? -1 : static_cast<int>(velocity);

            outNotes.push_back(note);
        }
    }

    const auto pedal_property = response.getProperty("pedal", juce::var());

    if (const auto* pedal = pedal_property.getArray()) {
        outPedal.reserve(static_cast<size_t>(pedal->size()));

        for (const auto& entry: *pedal) {
            if (entry.getDynamicObject() == nullptr) {
                continue;
            }

            SidecarPedalEvent event;
            event.time = static_cast<double>(entry.getProperty("time", 0.0));
            event.value = static_cast<int>(entry.getProperty("value", 0));

            outPedal.push_back(event);
        }
    }

    return true;
}

void SidecarClient::shutdown()
{
    if (!mStarted) {
        return;
    }

    if (isRunning()) {
        auto* request = new juce::DynamicObject();
        request->setProperty("cmd", "shutdown");

        juce::String ignored_error;
        _writeLine(juce::JSON::toString(juce::var(request), true), ignored_error);
    }

    // Closing stdin is what actually ends the child per the protocol (EOF after "shutdown"), so
    // that happens before the wait below, not as part of the general handle cleanup.
#if JUCE_WINDOWS
    if (mChildStdinWrite != nullptr) {
        CloseHandle(mChildStdinWrite);
        mChildStdinWrite = nullptr;
    }

    if (mProcessHandle != nullptr && WaitForSingleObject(mProcessHandle, kShutdownWaitMs) != WAIT_OBJECT_0) {
        TerminateProcess(mProcessHandle, 0);
    }
#else
    if (mChildStdinWrite >= 0) {
        close(mChildStdinWrite);
        mChildStdinWrite = -1;
    }

    bool exited = false;

    if (mChildPid > 0) {
        for (int waited_ms = 0; waited_ms < kShutdownWaitMs && !exited; waited_ms += 20) {
            int status = 0;
            exited = waitpid(mChildPid, &status, WNOHANG) != 0;

            if (!exited) {
                usleep(20 * 1000);
            }
        }

        if (!exited) {
            kill(mChildPid, SIGKILL);
        }
    }
#endif

    _closeHandles();
    mReadBuffer.clear();
    mStarted = false;
}

std::vector<Notes::Event> toNotesEvents(const std::vector<SidecarNote>& inNotes)
{
    std::vector<Notes::Event> events;
    events.reserve(inNotes.size());

    for (const auto& note: inNotes) {
        // Aggregate-initialised, so every field not set below keeps Notes::Event's own default:
        // startFrame/endFrame/amplitude at 0 (no frame grid or model confidence to report), bends
        // empty, onsetConfidence at 0.0. See the declaration in SidecarClient.h for why velocity
        // is the field that gets the sidecar's number.
        Notes::Event event{};
        event.startTime = note.onset;
        event.endTime = note.offset;
        event.pitch = note.pitch;
        event.velocity = note.velocity < 0 ? 0.0 : static_cast<double>(note.velocity) / 127.0;

        events.push_back(event);
    }

    return events;
}
