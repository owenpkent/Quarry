//
// Blocking client for the transcription sidecar protocol (version 1): newline-delimited JSON
// over a child process's stdio.
//

#ifndef SidecarClient_h
#define SidecarClient_h

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

    /** Send {"cmd":"shutdown"}, close stdin, and wait briefly for the child to exit on its own. */
    void shutdown();

    /** Whether the child process is still alive. False before start() and after shutdown(). */
    bool isRunning() const;

    /**
     * The engine names the sidecar reported in its "ready" line, sorted as it sent them. These
     * are the engines whose packages import cleanly in that interpreter, which is not the same
     * as the ones with a model loaded -- loading is lazy and happens on first use. Empty until
     * start() has succeeded. See tools/sidecar/PROTOCOL.md, "Startup".
     */
    const juce::StringArray& getAvailableEngines() const;

    /**
     * "cuda" or "cpu": the device every engine in this process is loaded on, fixed for the life
     * of the child. Empty until start() has succeeded.
     */
    const juce::String& getDevice() const;

private:
    /** Write one line (with the trailing newline) to the child's stdin. */
    bool _writeLine(const juce::String& inLine, juce::String& outError);

    /**
     * Block for one newline-terminated line from the child's stdout, or until inTimeoutMs has
     * elapsed since inDeadlineStart. Bytes read past the newline are kept in mReadBuffer for the
     * next call, since the child's writes have no reason to land on line boundaries.
     */
    bool _readLine(juce::String& outLine, double inDeadlineStart, int inTimeoutMs, juce::String& outError);

    /** Read one JSON object line matching inAwaitedId, skipping anything else (see class docs). */
    bool _readResponse(const juce::String& inAwaitedId, juce::var& outResponse, int inTimeoutMs, juce::String& outError);

    void _closeHandles();

    juce::String mCommandLine;
    std::string mReadBuffer;
    int mNextId = 0;
    bool mStarted = false;

    // Filled from the "ready" line and then left alone: the child re-reports nothing, so these
    // describe this child for as long as it lives.
    juce::StringArray mAvailableEngines;
    juce::String mDevice;

#if JUCE_WINDOWS
    HANDLE mChildStdinWrite = nullptr;
    HANDLE mChildStdoutRead = nullptr;
    HANDLE mProcessHandle = nullptr;
#else
    int mChildStdinWrite = -1;
    int mChildStdoutRead = -1;
    pid_t mChildPid = -1;
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
