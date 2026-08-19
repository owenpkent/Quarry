//
// Pure decision logic for whether the sidecar transcription tier is active, kept separate from
// TranscriptionManager so it can be unit tested without a QuarryAudioProcessor.
//

#ifndef SidecarActivation_h
#define SidecarActivation_h

#include <JuceHeader.h>

/**
 * The sidecar transcription tier is controlled by two environment variables, read once when
 * TranscriptionManager is constructed (i.e. once per process, not per take):
 *
 *  - QUARRY_SIDECAR_CMD: the full command line used to launch the sidecar, exactly as it would be
 *    typed at a shell, e.g. "py C:/path/quarry_sidecar.py serve". Absent or empty (including
 *    whitespace-only) means the sidecar tier is off and transcription runs through BasicPitch,
 *    byte-for-byte as it did before this feature existed.
 *  - QUARRY_SIDECAR_ENGINE: which sidecar engine to request ("kong", "transkun", "muscriptor", or
 *    "auto"). Optional; defaults to "auto". Read but not otherwise validated here -- an unknown
 *    engine name is the sidecar's own error to report at transcribe time.
 */
struct SidecarActivation {
    bool active = false;
    juce::String commandLine;
    juce::String engine = "auto";
};

/**
 * @param inCommandLineEnv Raw value of QUARRY_SIDECAR_CMD (empty string if unset).
 * @param inEngineEnv Raw value of QUARRY_SIDECAR_ENGINE (empty string if unset).
 */
SidecarActivation resolveSidecarActivation(const juce::String& inCommandLineEnv, const juce::String& inEngineEnv);

#endif // SidecarActivation_h
