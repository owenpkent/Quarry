#pragma once

#include "PluginProcessor.h"
#include "QuarryMainView.h"
#include "QuarryLNF.h"

class QuarryEditor : public juce::AudioProcessorEditor
{
public:
    explicit QuarryEditor(QuarryAudioProcessor&);

    ~QuarryEditor();

    void paint(juce::Graphics&) override;

    void resized() override;

    QuarryMainView* getMainView() const { return mMainView.get(); }

private:
    std::unique_ptr<QuarryMainView> mMainView;

    QuarryLNF mQuarryLnF;
};
