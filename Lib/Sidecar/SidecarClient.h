//
// Blocking client for the transcription sidecar protocol (tools/sidecar/PROTOCOL.md, version 2):
// newline-delimited JSON over a child process's stdio. A version-1 sidecar still works with this
// client -- it just never sends a "stage" event, which this client treats the same as any other
// line it does not recognise (see classifyLine).
//

#ifndef SidecarClient_h
#define SidecarClient_h

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "Notes.h"
#include "SidecarTypes.h"

#if JUCE_WINDOWS
// Without NOMINMAX, windows.h #defines min/max and silently breaks every std::min/std::max call
// in any translation unit that includes this header afterwards.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

/**
 * Talks to an external transcription process (the Python sidecar, or anything else that speaks
 * the same protocol) over its own stdin/stdout: one request per line in, one response per line
 * out, both newline-delimited JSON.
 *
 * This is not built on juce::ChildProcess. That class only wires up a child's stdout/stderr to a
 * readable pipe -- on Windows its ActiveProcess leaves hStdInput unset, and there is no method on
 * any platform to write to the child's stdin -- so a class built "over" it cannot send the child
 * anything, and this protocol is a request/response exchange over stdin as much as stdout. This
 * talks to the OS process APIs directly instead, and leans on JUCE (juce::String, juce::JSON,
 * juce::File, juce::Time) for everything else. Shape and lifecycle (start/kill) still follow
 * juce::ChildProcess so this drops in where that would have gone.
 *
 * One request is ever in flight at a time; this is a bench/offline tool's client, not a
 * multiplexing one.
 */
class SidecarClient
{
public:
    /**
     * @param inCommandLine Full command line used to launch the sidecar, e.g.
     *  "py C:/path/sidecar.py serve". Parsed by the OS's own command-line rules (CreateProcess on
     *  Windows, /bin/sh -c elsewhere), exactly as if typed at a shell.
     */
    explicit SidecarClient(const juce::String& inCommandLine);

    /**
     * Shuts down the child if it is still running. juce::ChildProcess deliberately leaves the
     * child alive across its own destructor; this does not, because the sidecar can be holding a
     * loaded model's worth of memory (and on some engines a GPU context), and a caller that
     * forgets to call shutdown() should not leak that.
     */
    ~SidecarClient();

    SidecarClient(const SidecarClient&) = delete;
    SidecarClient& operator=(const SidecarClient&) = delete;

    /**
     * Launch the sidecar and block until its "ready" line arrives. Model loads are slow, so the
     * wait is generous (about 120 s) rather than tied to inTimeoutMs on transcribe().
     * @param outError Set on failure.
     * @return true once the sidecar has reported protocol readiness.
     */
    bool start(juce::String& outError);

    /**
     * Send one transcribe request and block for its matching response.
     * @param inWav Absolute path to the audio file to transcribe.
     * @param inEngine Engine name ("kong", "transkun", "muscriptor", "auto").
     * @param outNotes Cleared and filled with the response's notes, sorted by onset (the sidecar
     *  is required to sort them; this does not re-sort).
     * @param outPedal Cleared and filled with the response's pedal events.
     * @param outError Set to the sidecar's own error message on a well-formed failure response,
     *  or to a description of a transport failure (timeout, dead child, ...).
     * @param inTimeoutMs How long to wait for the response line before giving up.
     * @return true on a well-formed {"ok":true, ...} response.
     */
    bool transcribe(const juce::File& inWav,
                    const juce::String& inEngine,
                    std::vector<SidecarNote>& outNotes,
                    std::vector<SidecarPedalEvent>& outPedal,
                    juce::String& outError,
                    int inTimeoutMs = 600000);

    /**
     * Send one download request and block for its matching response (see PROTOCOL.md's
     * "download request"). Mirrors transcribe()'s request/response handling.
     * @param inUrl The URL to fetch audio from.
     * @param inOutDir Folder the sidecar should write the wav into (created if it does not exist).
     * @param outFile Set to the written wav's path on success.
     * @param outTitle Set to the source's title, as the sidecar reports it, on success.
     * @param outError Set to the sidecar's own error message on a well-formed failure response,
     *  or to a description of a transport failure (timeout, dead child, ...).
     * @param inTimeoutMs How long to wait for the response line before giving up. Long by default:
     *  a download is bytes over the network plus an ffmpeg pass, not a model inference call, but
     *  neither is it bounded the way a local file read would be.
     * @return true on a well-formed {"ok":true, ...} response.
     */
    bool download(const juce::String& inUrl,
                  const juce::File& inOutDir,
                  juce::File& outFile,
                  juce::String& outTitle,
                  juce::String& outError,
                  int inTimeoutMs = 600000);

    /** Send {"cmd":"shutdown"}, close stdin, and wait briefly for the child to exit on its own.
     *  Idempotent against itself and against kill() -- whichever of the two runs first does the
     *  work; the other finds the child already gone and returns immediately. */
    void shutdown();

    /**
     * Terminates the child immediately (TerminateProcess / SIGKILL) rather than asking it to exit,
     * and closes this client's handles. Idempotent against itself and against shutdown(), same as
     * shutdown() itself.
     *
     * Safe to call from another thread while a transcribe()/download() call is blocked waiting on
     * a response: killing the child closes the far end of the stdout pipe, which the blocked
     * call's read loop notices on its own (PeekNamedPipe reports a broken pipe on Windows, poll()
     * reports POLLHUP on POSIX) and returns from promptly, with outError set to
     * "sidecar terminated" rather than the generic "sidecar process exited unexpectedly" a crash
     * would produce. Not wired to anything yet (no cancel button, no manager-level timeout); it
     * exists as the primitive a future one will call.
     */
    void kill();

    /** Whether the child process is still alive. False before start() and after shutdown()/kill(). */
    bool isRunning() const;

    /**
     * The engine names the sidecar reported in its "ready" line, sorted as it sent them. These
     * are the engines whose packages import cleanly in that interpreter, which is not the same
     * as the ones with a model loaded -- loading is lazy and happens on first use. Empty until
     * start() has succeeded. See tools/sidecar/PROTOCOL.md, "Startup".
     */
    const juce::StringArray& getAvailableEngines() const;

    /**
     * Whether the ready line carried an "engines" array at all, which is not the same question
     * as whether that array had anything in it.
     *
     * The field is what the protocol says a sidecar sends, but an older serve process, or any
     * other implementation of the protocol, can leave it out -- and an absent field parses to
     * exactly the same empty StringArray as a sidecar that genuinely has no engine installed.
     * Treating the two alike refuses every transcribe request before it is sent, so a sidecar
     * that worked yesterday reports seven engines as "not installed" today. False here means
     * the sidecar did not say, and the only honest thing to do with a request is send it.
     */
    bool hasEngineList() const;

    /**
     * "cuda" or "cpu": the device every engine in this process is loaded on, fixed for the life
     * of the child. Empty until start() has succeeded.
     */
    const juce::String& getDevice() const;

    /** The "protocol" number from the "ready" line (see PROTOCOL.md's "Startup"). 0 until
     *  start() has succeeded, or if a pre-version-2 "ready" line ever omitted the field. */
    int getProtocolVersion() const;

    /**
     * Stage-event and raw-stderr sinks. Settable any time (including before start(), the usual
     * case); either may be left null, in which case that kind of line is simply not delivered
     * anywhere. Neither is called under any lock this class holds, so the receiver is responsible
     * for its own thread safety -- onStage and onStderrLine are not called on the same thread as
     * each other (see each field's own doc), and a caller touching shared state from either one
     * needs to guard it itself.
     */
    std::function<void(const SidecarStage&)> onStage;      // called on the thread blocked in transcribe()/download()/start()
    std::function<void(const juce::String&)> onStderrLine; // called on the stderr pump thread

    /** What kind of line a parsed stdout line is: a stage event to forward and keep waiting past,
     *  or anything else (the response a caller is waiting for -- transcribe/download's
     *  {"id":...,"ok":...}, start()'s {"event":"ready",...} -- or a malformed/unrecognised line,
     *  which the caller's own "is this what I'm waiting for" check already skips over the same
     *  way it would skip an unrelated well-formed line). Static and free of instance state so it
     *  can be unit-tested directly; see Tests/sidecar_client_test.h. */
    enum class LineKind { Stage, Other };

    /** Classifies inParsed (already run through juce::JSON::parse). When the result is Stage,
     *  outStage is filled in from the event's "stage"/"text"/"t"/"fraction" fields (fraction left
     *  at its struct default, -1.0, when the field is absent); outStage is left untouched
     *  otherwise. inParsed being anything other than a JSON object (parse failure, a bare array,
     *  stray non-protocol output) classifies as Other, the same as a well-formed line that is not
     *  a stage event. */
    static LineKind classifyLine(const juce::var& inParsed, SidecarStage& outStage);

private:
    class StderrPump;

    /** Write one line (with the trailing newline) to the child's stdin. */
    bool _writeLine(const juce::String& inLine, juce::String& outError);

    /**
     * Block for one newline-terminated line from the child's stdout, or until inTimeoutMs has
     * elapsed since inDeadlineStart. Bytes read past the newline are kept in mReadBuffer for the
     * next call, since the child's writes have no reason to land on line boundaries.
     *
     * On a broken pipe or a dead child, outError is "sidecar terminated" if mKilled is set (this
     * is kill()'s doing) or a description of the unexpected failure otherwise -- see kill()'s docs.
     */
    bool _readLine(juce::String& outLine, double inDeadlineStart, int inTimeoutMs, juce::String& outError);

    /**
     * Reads lines until one classifies as LineKind::Other and matches inAwaitedId (see
     * classifyLine); every LineKind::Stage line along the way is forwarded to onStage (if set)
     * and skipped without resetting inDeadlineStart. Used by transcribe() and download(); start()
     * has its own version of this loop since it is waiting for "ready", not an id match.
     */
    bool _readResponse(const juce::String& inAwaitedId, juce::var& outResponse, int inTimeoutMs, juce::String& outError);

    /** Stops and destroys the stderr pump thread, if one is running. Called before _closeHandles()
     *  everywhere a shutdown path closes the stderr pipe, so the pump is never left reading from a
     *  handle another thread just closed out from under it. */
    void _stopStderrPump();

    void _closeHandles();

    juce::String mCommandLine;
    std::string mReadBuffer;
    int mNextId = 0;
    std::atomic<bool> mStarted { false };

    // Set by kill() before it tears anything down, so _readLine can tell "the child died because
    // kill() was called" apart from "the child died on its own" and report each with a different
    // outError. Never cleared -- a killed client is done; start() is not meant to be called again
    // on it (nothing currently stops that, but nothing resets mKilled if it happens either).
    std::atomic<bool> mKilled { false };

    // Guards the mStarted check-and-clear at the top of shutdown() and kill(), so the two are
    // idempotent against each other: whichever call gets here first does the teardown, the other
    // sees mStarted already false and returns having done nothing.
    juce::CriticalSection mLifecycleLock;

    // Filled from the "ready" line and then left alone: the child re-reports nothing, so these
    // describe this child for as long as it lives.
    juce::StringArray mAvailableEngines;
    bool mEngineListReported = false;
    juce::String mDevice;
    int mProtocolVersion = 0;

    // Drains the child's stderr from just after the process launches until shutdown()/kill(), so
    // the pipe can never fill (see the pump's own docs in SidecarClient.cpp) and so its lines can
    // be handed to onStderrLine as they arrive rather than only after the child exits.
    std::unique_ptr<StderrPump> mStderrPump;

    // HANDLE (Windows, a void*) and int (POSIX, a file descriptor / pid) are trivially copyable,
    // so wrapping them in std::atomic costs nothing and closes the one real cross-thread hazard
    // kill() introduces: a plain (non-atomic) write to one of these racing a plain read of the
    // same variable on the thread blocked inside _readLine is undefined behaviour at the language
    // level even though the underlying OS handle/fd itself tolerates being closed out from under
    // a concurrent read (that just fails the read, which _readLine already treats as an error).
#if JUCE_WINDOWS
    std::atomic<HANDLE> mChildStdinWrite { nullptr };
    std::atomic<HANDLE> mChildStdoutRead { nullptr };
    std::atomic<HANDLE> mChildStderrRead { nullptr };
    std::atomic<HANDLE> mProcessHandle { nullptr };

    // The child's kill-on-close job. Its only job (so to speak) is the abnormal exit: shutdown()
    // and kill() already end the child on every path this class controls, but neither runs if the
    // host process is terminated rather than closed -- a crash, a force-quit, the debugger's stop
    // button. Windows closes this handle with the rest of them on the way down, and because it is
    // the last handle to the job, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE takes the sidecar with it.
    // Without this the child survives its parent, still holding whatever VRAM its model loaded.
    //
    // Null when the job could not be created; that degrades to the old behaviour rather than
    // refusing to start, since a sidecar with no safety net still beats no sidecar.
    std::atomic<HANDLE> mJobHandle { nullptr };
#else
    std::atomic<int> mChildStdinWrite { -1 };
    std::atomic<int> mChildStdoutRead { -1 };
    std::atomic<int> mChildStderrRead { -1 };
    std::atomic<pid_t> mChildPid { -1 };
#endif
};

/**
 * Map sidecar notes onto Notes::Event, the currency the rest of the engine speaks, so downstream
 * code (the bench, eventually the plugin) can treat a sidecar take exactly like a BasicPitch one.
 *
 * velocity/127 goes into Event::velocity, the field NoteVelocity fills in for the BasicPitch path
 * (0..1 loudness measured from audio -- not Event::amplitude, which is the model's own confidence
 * and a different thing entirely). A null sidecar velocity (-1) maps to that field's declared
 * default of 0.0, the same value an event carries before NoteVelocity has run: "not measured", not
 * "silent". amplitude and onsetConfidence are model-confidence fields the sidecar has no
 * equivalent for, so they are left at their struct defaults (0.0) rather than invented; bends is
 * left empty for the same reason; startFrame/endFrame have no meaning off basic-pitch's frame
 * grid and are left at 0.
 */
std::vector<Notes::Event> toNotesEvents(const std::vector<SidecarNote>& inNotes);

#endif // SidecarClient_h
