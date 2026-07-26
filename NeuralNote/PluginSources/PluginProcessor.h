#pragma once

#include <atomic>
#include <JuceHeader.h>

#include "Resampler.h"
#include "ProcessorBase.h"
#include "BasicPitch.h"
#include "NoteOptions.h"
#include "MidiFileWriter.h"
#include "TimeQuantizeOptions.h"
#include "Player.h"
#include "SourceAudioManager.h"
#include "AudioInputManager.h"
#include "ParameterHelpers.h"
#include "TranscriptionManager.h"
#include "NnId.h"

class NeuralNoteMainView;
class NeuralNoteEditor;

enum State { EmptyAudioAndMidiRegions = 0, Recording, Processing, PopulatedAudioAndMidiRegions };

class NeuralNoteAudioProcessor : public PluginHelpers::ProcessorBase
{
public:
    NeuralNoteAudioProcessor();

    ~NeuralNoteAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    void processBlock(AudioBuffer<float>&, MidiBuffer&) override;

    AudioProcessorEditor* createEditor() override;

    void getStateInformation(MemoryBlock& destData) override;

    void setStateInformation(const void* data, int sizeInBytes) override;

    State getState() const { return mState.load(); }

    void setStateToRecording() { mState.store(Recording); }

    void setStateToProcessing() { mState.store(Processing); }

    void setStateToPopulatedAudioAndMidiRegions() { mState.store(PopulatedAudioAndMidiRegions); }

    void clear();

    /**
     * Start recording, from the input device selected in the audio input panel if there is one, and
     * otherwise from the audio the host sends us.
     * @return false if a selected input device could not be opened, in which case nothing started
     *         and AudioInputManager::getLastError() says why.
     */
    bool startRecording();

    void stopRecording();

    /** Peak level of the audio the host has sent us since the previous call, in [0, 1]. */
    float getAndResetHostInputPeakLevel() { return mHostInputPeakLevel.exchange(0.0f); }

    /** Only measure the host input level while something is showing it. */
    void setHostInputLevelWanted(bool inWanted) { mHostInputLevelWanted.store(inWanted); }

    SourceAudioManager* getSourceAudioManager() const;

    AudioInputManager* getAudioInputManager() const;

    Player* getPlayer() const;

    TranscriptionManager* getTranscriptionManager() const;

    std::array<RangedAudioParameter*, ParameterHelpers::TotalNumParams>& getParams();

    float getParameterValue(ParameterHelpers::ParamIdEnum inParamId) const;

    NeuralNoteMainView* getNeuralNoteMainView() const;

    AudioProcessorValueTreeState& getAPVTS();

    ValueTree& getValueTree();

    void addListenerToStateValueTree(ValueTree::Listener* inListener);

    void removeListenerFromStateValueTree(ValueTree::Listener* inListener);

private:
    static ValueTree _createDefaultValueTree();

    void _updateValueTree(const ValueTree& inNewState);

    // ValueTree for general plugin state
    ValueTree mValueTree = _createDefaultValueTree();

    // Value tree state to pass automatable parameters from UI
    AudioProcessorValueTreeState mAPVTS;

    std::array<RangedAudioParameter*, ParameterHelpers::TotalNumParams> mParams {};

    std::atomic<State> mState = EmptyAudioAndMidiRegions;

    std::atomic<float> mHostInputPeakLevel {0.0f};
    std::atomic<bool> mHostInputLevelWanted {false};

    std::unique_ptr<SourceAudioManager> mSourceAudioManager;
    std::unique_ptr<AudioInputManager> mAudioInputManager;
    std::unique_ptr<Player> mPlayer;
    std::unique_ptr<TranscriptionManager> mTranscriptionManager;
    std::unique_ptr<FileLogger> mLogger;
};
