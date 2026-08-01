#include "ProcessorBase.h"

namespace PluginHelpers
{
ProcessorBase::ProcessorBase()
    : juce::AudioProcessor(getDefaultProperties())
{
}

ProcessorBase::ProcessorBase(const BusesProperties& ioLayouts)
    : AudioProcessor(ioLayouts)
{
}

const juce::String ProcessorBase::getName() const
{
    return JucePlugin_Name;
}

bool ProcessorBase::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool ProcessorBase::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool ProcessorBase::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double ProcessorBase::getTailLengthSeconds() const
{
    return 0.0;
}

int ProcessorBase::getNumPrograms()
{
    return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
    // so this should be at least 1, even if you're not really implementing programs.
}

int ProcessorBase::getCurrentProgram()
{
    return 0;
}

void ProcessorBase::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String ProcessorBase::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void ProcessorBase::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void ProcessorBase::releaseResources()
{
}

juce::AudioProcessor::BusesProperties ProcessorBase::getDefaultProperties()
{
    auto properties = BusesProperties()
#if !JucePlugin_IsMidiEffect
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
        ;

#if !JucePlugin_IsMidiEffect && !JucePlugin_IsSynth
    // The standalone records through the device picked in the audio input panel and
    // never reads the host input, so it has no input bus. Declaring one made JUCE's
    // standalone wrapper assume a feedback loop, mute the input, and park a banner
    // above the window. In a plugin the host input is the recording source, so the
    // bus stays. wrapperType is not set on the processor yet at this point, but the
    // wrapper has already recorded which one loaded us.
    if (juce::PluginHostType::getPluginLoadedAs() != juce::AudioProcessor::wrapperType_Standalone)
        properties = properties.withInput("Input", juce::AudioChannelSet::stereo(), true);
#endif

    return properties;
}

juce::AudioProcessorEditor* ProcessorBase::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

bool ProcessorBase::isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const
{
    if (isMidiEffect())
        return true;
    else {
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
            && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
    }

    // This checks if the input layout matches the output layout. The standalone declares
    // no input bus at all, see getDefaultProperties, so there is no input set to match and
    // comparing the missing one against the output would reject every layout. The test is
    // on whether a bus was ever declared, not on whether this layout happens to disable it:
    // a host switching off an input bus the plugin does declare should still be rejected,
    // because recording would then silently take the output buffer for host input.
#if !JucePlugin_IsSynth
    if (getBusCount(true) > 0 && layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
}

} // namespace PluginHelpers