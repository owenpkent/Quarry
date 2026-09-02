//
// Blocking client for the transcription sidecar protocol (tools/sidecar/PROTOCOL.md, version 2):
// newline-delimited JSON over a child process's stdio.
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

// How long shutdown()/kill() wait for the stderr pump thread to notice it should stop and return
// from run(). The pump's own read loop polls on a much shorter grain than this (see StderrPump::
// run()), so in practice this is a ceiling that is never actually reached.
constexpr int kStderrPumpStopMs = 2000;
} // namespace

/**
 * Drains the child's stderr pipe on its own thread, from just after the process launches (see
 * start()) until shutdown()/kill() stops it, and hands each complete line to the owning client's
 * onStderrLine sink.
 *
 * Why a pump exists at all: stderr is logging per the protocol (a caller must never parse it),
 * but it used to be discarded outright (sent to NUL / /dev/null) because nothing looked at it. A
 * serve-mode caller now wants to see it -- piped into its own activity log -- so it has to be
 * read from somewhere. The pipe still must never be allowed to fill regardless of whether
 * anything is listening (a full pipe would block the sidecar's own stderr writes and,
 * transitively, whatever engine call it is logging from), so this reads continuously the same
 * way it always drained for free into NUL, and only the destination of the bytes has changed.
 *
 * Lines are split on '\n', with a trailing '\r' stripped so Windows-style "\r\n" and plain "\n"
 * both produce the same line; a chunk that ends mid-line is held over to be completed by the next
 * read rather than delivered early. Whatever partial line is left when the pipe closes (the
 * child's last line, if it did not end with its own newline) is still delivered rather than lost.
 */
class SidecarClient::StderrPump : public juce::Thread
{
public:
    explicit StderrPump(SidecarClient& inOwner) : juce::Thread("sidecar stderr pump"), mOwner(inOwner) {}

    void run() override
    {
        char chunk[4096];

        for (;;) {
            if (threadShouldExit()) {
                break;
            }

#if JUCE_WINDOWS
            DWORD num_read = 0;
            bool nothing_available = false;

            {
                // Every touch of the handle happens under the owner's I/O lock, so shutdown()/
                // kill() cannot close it between the load here and the call that uses it.
                const juce::ScopedLock lock(mOwner.mIoLock);

                const HANDLE stderr_read = mOwner.mChildStderrRead.load();
                DWORD available = 0;

                if (stderr_read == nullptr
                    || !PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr)) {
                    break; // pipe closed: the child exited, or shutdown()/kill() closed our own end
                }

                if (available == 0) {
                    nothing_available = true;
                } else {
                    const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));

                    if (!ReadFile(stderr_read, chunk, to_read, &num_read, nullptr) || num_read == 0) {
                        break;
                    }
                }
            }

            if (nothing_available) {
                // 20 ms rather than the 2 ms this started at, matching the POSIX branch's poll
                // timeout below. PeekNamedPipe reports nothing available for almost all of a
                // sidecar's life -- it logs in bursts and is idle between requests -- so the
                // shorter grain bought no latency anybody could perceive (the drawer that reads
                // these lines repaints at 10 Hz) and cost 500 wakeups a second, per loaded plugin
                // instance, for as long as the child lived.
                juce::Thread::sleep(20);
                continue;
            }

            mBuffer.append(chunk, num_read);
#else
            ssize_t num_read = 0;
            bool nothing_to_read = false;

            {
                const juce::ScopedLock lock(mOwner.mIoLock);

                const int stderr_read = mOwner.mChildStderrRead.load();

                if (stderr_read < 0) {
                    break;
                }

                pollfd pfd{stderr_read, POLLIN, 0};
                const auto poll_result = ::poll(&pfd, 1, 20);

                if (poll_result < 0) {
                    break;
                }

                // POLLIN is tested before POLLHUP, and the hangup is only honoured once there is
                // nothing left to read. A child that writes a traceback and exits sets both flags
                // at once, and a loop that breaks on the hangup first throws away the very lines
                // this pump exists to collect: the drawer would show a sidecar that died with no
                // word about why, which is the state this whole feed was added to end.
                if ((pfd.revents & POLLIN) == 0) {
                    if (pfd.revents & (POLLHUP | POLLERR)) {
                        break; // far end closed and drained: child gone, or we closed our end
                    }

                    nothing_to_read = true; // poll timed out
                } else {
                    num_read = ::read(stderr_read, chunk, sizeof(chunk));

                    if (num_read <= 0) {
                        break; // EOF: the writer is gone and the pipe is empty
                    }
                }
            }

            if (nothing_to_read) {
                continue;
            }

            mBuffer.append(chunk, static_cast<size_t>(num_read));
#endif

            _deliverCompleteLines();
        }

        // The child's very last line, if it did not end in its own newline, would otherwise be
        // silently dropped just because the pipe closed a moment too soon.
        if (!mBuffer.empty() && mOwner.onStderrLine) {
            mOwner.onStderrLine(juce::String::fromUTF8(mBuffer.data(), static_cast<int>(mBuffer.size())));
        }
    }

private:
    void _deliverCompleteLines()
    {
        for (;;) {
            const auto newline_pos = mBuffer.find('\n');

            if (newline_pos == std::string::npos) {
                return;
            }

            auto line = mBuffer.substr(0, newline_pos);
            mBuffer.erase(0, newline_pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (mOwner.onStderrLine) {
                mOwner.onStderrLine(juce::String::fromUTF8(line.data(), static_cast<int>(line.size())));
            }
        }
    }

    SidecarClient& mOwner;
    std::string mBuffer;
};

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

int SidecarClient::getProtocolVersion() const
{
    return mProtocolVersion;
}

SidecarClient::LineKind SidecarClient::classifyLine(const juce::var& inParsed, SidecarStage& outStage)
{
    // Not a JSON object at all: parse failure, a bare array, or stray non-protocol output on
    // stdout. Nothing to read fields from, and definitely not a stage event.
    if (inParsed.getDynamicObject() == nullptr) {
        return LineKind::Other;
    }

    if (inParsed.getProperty("event", juce::var()).toString() != "stage") {
        // Not a stage event: could be the response a caller is waiting for (transcribe/download's
        // {"id":...,"ok":...}, or start()'s {"event":"ready",...}), or some event type this client
        // does not know about. Classification stops here either way -- each call site already has
        // its own "is this actually what I'm waiting for" check (an id match, or event=="ready"),
        // and a line that fails that check is skipped there exactly as an unknown event would be.
        return LineKind::Other;
    }

    outStage.stage = inParsed.getProperty("stage", juce::var()).toString();
    outStage.text = inParsed.getProperty("text", juce::var()).toString();
    outStage.t = static_cast<double>(inParsed.getProperty("t", 0.0));

    const auto fraction = inParsed.getProperty("fraction", juce::var());
    outStage.fraction = fraction.isVoid() ? -1.0 : static_cast<double>(fraction);

    return LineKind::Stage;
}

bool SidecarClient::_writeLine(const juce::String& inLine, juce::String& outError)
{
    // Named so the temporary outlives the raw pointer toRawUTF8() hands back.
    const juce::String line = inLine + "\n";
    const auto* payload = line.toRawUTF8();
    const auto num_bytes = static_cast<size_t>(strlen(payload));

    // Under the I/O lock for the same reason the reads are: shutdown()/kill() closes stdin, and
    // between the "is it open" check and the write there is otherwise nothing stopping it.
    const juce::ScopedLock lock(mIoLock);

#if JUCE_WINDOWS
    const HANDLE stdin_write = mChildStdinWrite.load();

    if (stdin_write == nullptr) {
        outError = "sidecar stdin is not open";
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(stdin_write, payload, static_cast<DWORD>(num_bytes), &written, nullptr)
        || written != num_bytes) {
        outError = "failed to write to sidecar stdin";
        return false;
    }
#else
    const int stdin_write = mChildStdinWrite.load();

    if (stdin_write < 0) {
        outError = "sidecar stdin is not open";
        return false;
    }

    size_t total_written = 0;

    while (total_written < num_bytes) {
        const auto n = ::write(stdin_write, payload + total_written, num_bytes - total_written);

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
        DWORD num_read = 0;
        bool nothing_available = false;

        {
            // The handle is loaded and used under the same lock _closeHandles() takes, so kill()
            // on another thread cannot close it between the two. Held for one Peek and one Read,
            // never across the sleep below, so kill() waits microseconds rather than on a
            // response that may never come.
            const juce::ScopedLock lock(mIoLock);

            const HANDLE stdout_read = mChildStdoutRead.load();
            DWORD available = 0;

            if (stdout_read == nullptr
                || !PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
                // Reported once the far end (the child) is gone -- either it exited on its own, or
                // kill() just terminated it; mKilled tells the two apart for outError's sake.
                outError = mKilled ? "sidecar terminated" : "sidecar stdout pipe closed";
                return false;
            }

            if (available == 0) {
                nothing_available = true;
            } else {
                const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));

                if (!ReadFile(stdout_read, chunk, to_read, &num_read, nullptr) || num_read == 0) {
                    outError = mKilled ? "sidecar terminated" : "failed to read sidecar stdout";
                    return false;
                }
            }
        }

        if (nothing_available) {
            if (!isRunning()) {
                outError = mKilled ? "sidecar terminated" : "sidecar process exited unexpectedly";
                return false;
            }

            juce::Thread::sleep(1);
            continue;
        }

        mReadBuffer.append(chunk, num_read);
#else
        ssize_t num_read = 0;
        bool nothing_to_read = false;

        {
            const juce::ScopedLock lock(mIoLock);

            const int stdout_read = mChildStdoutRead.load();

            if (stdout_read < 0) {
                outError = mKilled ? "sidecar terminated" : "sidecar stdout pipe closed";
                return false;
            }

            pollfd pfd{stdout_read, POLLIN, 0};
            // Short poll granularity, mirroring the Windows branch's 1 ms sleep: this loop's own
            // deadline check is what actually bounds the wait, not this number.
            const auto poll_result = ::poll(&pfd, 1, 5);

            if (poll_result < 0) {
                outError = mKilled ? "sidecar terminated" : "failed to poll sidecar stdout";
                return false;
            }

            // POLLIN before POLLHUP, and the hangup honoured only once the pipe is drained. A
            // sidecar that writes its response and exits in the same breath sets both at once,
            // and reading the hangup first turned a transcribe that had actually succeeded into
            // "sidecar stdout pipe closed" -- the answer was sitting in the pipe, unread.
            if ((pfd.revents & POLLIN) == 0) {
                if (pfd.revents & (POLLHUP | POLLERR)) {
                    outError = mKilled ? "sidecar terminated" : "sidecar stdout pipe closed";
                    return false;
                }

                nothing_to_read = true; // poll timed out
            } else {
                num_read = ::read(stdout_read, chunk, sizeof(chunk));

                if (num_read <= 0) {
                    outError = mKilled ? "sidecar terminated" : "failed to read sidecar stdout";
                    return false;
                }
            }
        }

        if (nothing_to_read) {
            if (!isRunning()) {
                outError = mKilled ? "sidecar terminated" : "sidecar process exited unexpectedly";
                return false;
            }

            continue;
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

        SidecarStage stage;
        const auto kind = classifyLine(parsed, stage);

        if (kind == LineKind::Stage) {
            if (onStage) {
                onStage(stage);
            }

            continue; // same deadline_start/inTimeoutMs: a stage event does not buy more time
        }

        // Not a stage event: either garbage (malformed line, stray non-protocol output -- not a
        // JSON object at all, so getProperty below just falls back to its default) or a
        // well-formed line that is not this request's response. Either way, skip and keep
        // waiting.
        if (parsed.getProperty("id", juce::var()).toString() != inAwaitedId) {
            continue;
        }

        outResponse = parsed;
        return true;
    }
}

void SidecarClient::_stopStderrPump()
{
    if (mStderrPump != nullptr) {
        mStderrPump->stopThread(kStderrPumpStopMs);
        mStderrPump.reset();
    }
}

void SidecarClient::_closeHandles()
{
    // The other half of mIoLock's contract: a reader or writer holding this lock is inside a
    // syscall on one of these handles right now, and closing one under it would hand that thread
    // a descriptor the OS is free to give to something else. Waiting here costs a single Peek or
    // a 5 ms poll, which is nothing next to the child this is tearing down.
    const juce::ScopedLock lock(mIoLock);

#if JUCE_WINDOWS
    if (mChildStdinWrite != nullptr) {
        CloseHandle(mChildStdinWrite);
        mChildStdinWrite = nullptr;
    }

    if (mChildStdoutRead != nullptr) {
        CloseHandle(mChildStdoutRead);
        mChildStdoutRead = nullptr;
    }

    if (mChildStderrRead != nullptr) {
        CloseHandle(mChildStderrRead);
        mChildStderrRead = nullptr;
    }

    if (mProcessHandle != nullptr) {
        CloseHandle(mProcessHandle);
        mProcessHandle = nullptr;
    }

    // Last, and after the process handle: closing the job is what would kill the child if it were
    // somehow still running, and by this point shutdown()/kill() have already ended it deliberately.
    if (mJobHandle != nullptr) {
        CloseHandle(mJobHandle);
        mJobHandle = nullptr;
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

    if (mChildStderrRead >= 0) {
        close(mChildStderrRead);
        mChildStderrRead = -1;
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
    HANDLE child_stderr_read = nullptr;
    HANDLE child_stderr_write = nullptr;

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

    // stderr is logging per the protocol, piped rather than discarded so onStderrLine (via
    // StderrPump) can hand it to a caller's own activity log; see StderrPump's docs above for why
    // this still has to be drained continuously regardless of whether anyone is listening.
    if (!CreatePipe(&child_stderr_read, &child_stderr_write, &security_attributes, 0)
        || !SetHandleInformation(child_stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        outError = "failed to create sidecar stderr pipe";
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdout_write);
        return false;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_stdin_read;
    startup_info.hStdOutput = child_stdout_write;
    startup_info.hStdError = child_stderr_write;

    PROCESS_INFORMATION process_info{};

    // CreateProcessW can write into its command-line argument, so this needs to be a real,
    // owned buffer and not a pointer into juce::String's own storage.
    const auto* wide_command_line = mCommandLine.toWideCharPointer();
    std::vector<wchar_t> command_buffer(wide_command_line, wide_command_line + mCommandLine.length() + 1);

    // See mJobHandle: the net that catches the child when this process is terminated rather than
    // closed. Best-effort -- a null job here just means the child outlives an abnormal exit, which
    // is exactly the behaviour this had before, so it is not worth failing the start over.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    // CREATE_SUSPENDED so the child is in the job before it runs a single instruction. Assigning
    // afterwards would leave a window in which it could spawn a grandchild (or exit) outside the
    // job, and a model-loading python is exactly the sort of process that spawns things early.
    const auto created = CreateProcessW(nullptr,
                                        command_buffer.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW | (job != nullptr ? CREATE_SUSPENDED : 0u),
                                        nullptr,
                                        nullptr,
                                        &startup_info,
                                        &process_info)
        != FALSE;

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stderr_write);

    if (!created) {
        outError = "failed to launch sidecar: " + mCommandLine;

        if (job != nullptr) {
            CloseHandle(job);
        }

        CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read);
        CloseHandle(child_stderr_read);
        return false;
    }

    if (job != nullptr) {
        // A failure to assign or resume leaves a suspended child that will never speak, so this is
        // the one place the job is worth dying over: kill it and report, rather than starting a
        // sidecar that silently answers nothing.
        if (!AssignProcessToJobObject(job, process_info.hProcess) || ResumeThread(process_info.hThread) == (DWORD) -1) {
            outError = "failed to place the sidecar in its job object";
            TerminateProcess(process_info.hProcess, 1);
            CloseHandle(process_info.hThread);
            CloseHandle(process_info.hProcess);
            CloseHandle(job);
            CloseHandle(child_stdin_write);
            CloseHandle(child_stdout_read);
            CloseHandle(child_stderr_read);
            return false;
        }
    }

    CloseHandle(process_info.hThread);

    mJobHandle = job;
    mProcessHandle = process_info.hProcess;
    mChildStdinWrite = child_stdin_write;
    mChildStdoutRead = child_stdout_read;
    mChildStderrRead = child_stderr_read;
#else
    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        outError = "failed to create sidecar pipes";
        return false;
    }

    const auto pid = fork();

    if (pid < 0) {
        outError = "failed to fork sidecar process";
        return false;
    }

    if (pid == 0) {
        // Child: wire the pipes onto the standard streams and hand off to a shell so the command
        // line gets the same quoting/splitting a caller typing it would expect. stderr is piped
        // rather than discarded now -- see the stderr-pipe comment on the Windows branch above.
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        execl("/bin/sh", "sh", "-c", mCommandLine.toRawUTF8(), (char*) nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    mChildStdinWrite = stdin_pipe[1];
    mChildStdoutRead = stdout_pipe[0];
    mChildStderrRead = stderr_pipe[0];
    mChildPid = pid;
#endif

    mStarted = true;

    // Draining from here (right after the process exists) rather than only once the "ready" wait
    // below succeeds, so nothing the child logs during its own startup is lost.
    mStderrPump = std::make_unique<StderrPump>(*this);
    mStderrPump->startThread();

    const auto deadline_start = juce::Time::getMillisecondCounterHiRes();

    for (;;) {
        juce::String line;

        if (!_readLine(line, deadline_start, kReadyTimeoutMs, outError)) {
            _stopStderrPump();
            _closeHandles();
            mStarted = false;
            return false;
        }

        juce::var parsed;
        juce::JSON::parse(line, parsed);

        SidecarStage stage;
        const auto kind = classifyLine(parsed, stage);

        if (kind == LineKind::Stage) {
            if (onStage) {
                onStage(stage);
            }

            continue; // a stage event before "ready" would be unusual, but costs nothing to forward
        }

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
            mProtocolVersion = static_cast<int>(parsed.getProperty("protocol", 0));

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

bool SidecarClient::download(const juce::String& inUrl,
                             const juce::File& inOutDir,
                             juce::File& outFile,
                             juce::String& outTitle,
                             juce::String& outError,
                             int inTimeoutMs)
{
    if (!mStarted || !isRunning()) {
        outError = "sidecar is not running";
        return false;
    }

    const auto id = juce::String(++mNextId);

    auto* request = new juce::DynamicObject();
    request->setProperty("id", id);
    request->setProperty("cmd", "download");
    request->setProperty("url", inUrl);
    request->setProperty("out_dir", inOutDir.getFullPathName());

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

    outFile = juce::File(response.getProperty("path", juce::var()).toString());
    outTitle = response.getProperty("title", juce::var()).toString();

    return true;
}

void SidecarClient::shutdown()
{
    {
        // See kill()'s docs: the two are idempotent against each other via this same lock, so
        // whichever runs first does the teardown and the other is a no-op.
        const juce::ScopedLock lock(mLifecycleLock);

        if (!mStarted) {
            return;
        }

        mStarted = false;
    }

    if (isRunning()) {
        auto* request = new juce::DynamicObject();
        request->setProperty("cmd", "shutdown");

        juce::String ignored_error;
        _writeLine(juce::JSON::toString(juce::var(request), true), ignored_error);
    }

    // Closing stdin is what actually ends the child per the protocol (EOF after "shutdown"), so
    // that happens before the wait below, not as part of the general handle cleanup. Under
    // mIoLock all the same: this is a handle _writeLine may be inside right now, and this close
    // is subject to exactly the reuse that member's note describes.
#if JUCE_WINDOWS
    {
        const juce::ScopedLock lock(mIoLock);

        if (mChildStdinWrite != nullptr) {
            CloseHandle(mChildStdinWrite);
            mChildStdinWrite = nullptr;
        }
    }

    // Deliberately outside the lock: this waits up to kShutdownWaitMs for the child to go, and
    // holding mIoLock across it would stall the stderr pump for the whole five seconds -- which
    // is precisely the window in which the child writes whatever it has to say about exiting.
    if (mProcessHandle != nullptr && WaitForSingleObject(mProcessHandle, kShutdownWaitMs) != WAIT_OBJECT_0) {
        TerminateProcess(mProcessHandle, 0);
    }
#else
    {
        const juce::ScopedLock lock(mIoLock);

        if (mChildStdinWrite >= 0) {
            close(mChildStdinWrite);
            mChildStdinWrite = -1;
        }
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
            ::kill(mChildPid, SIGKILL);
        }
    }
#endif

    _stopStderrPump();
    _closeHandles();
    mReadBuffer.clear();
}

void SidecarClient::kill()
{
    {
        // Idempotent against shutdown() via the same lock: see shutdown()'s own comment on this.
        const juce::ScopedLock lock(mLifecycleLock);

        if (!mStarted) {
            return;
        }

        mStarted = false;
    }

    // Set before terminating, not after: the whole point is for _readLine, on whatever thread is
    // blocked reading a response right now, to see this the moment it notices the pipe is gone.
    mKilled = true;

#if JUCE_WINDOWS
    if (mProcessHandle != nullptr) {
        TerminateProcess(mProcessHandle, 1);
    }
#else
    if (mChildPid > 0) {
        ::kill(mChildPid, SIGKILL);
    }
#endif

    _stopStderrPump();
    _closeHandles();

    // mReadBuffer is deliberately left alone here (shutdown() clears it; kill() does not): a
    // blocked transcribe()/download() call may still be appending to it on another thread for a
    // few more iterations of its read loop before it notices mKilled and returns, and clearing it
    // out from under that would be a real data race, not just an untidy handoff.
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
