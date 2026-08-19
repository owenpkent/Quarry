//
// Pure decision logic for whether the sidecar transcription tier is active.
//

#include "SidecarActivation.h"

SidecarActivation resolveSidecarActivation(const juce::String& inCommandLineEnv, const juce::String& inEngineEnv)
{
    SidecarActivation result;

    result.commandLine = inCommandLineEnv.trim();
    result.active = result.commandLine.isNotEmpty();

    const auto engine = inEngineEnv.trim();
    result.engine = engine.isNotEmpty() ? engine : juce::String("auto");

    return result;
}
