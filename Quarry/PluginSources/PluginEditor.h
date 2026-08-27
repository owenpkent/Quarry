#pragma once

#include "PluginProcessor.h"
#include "QuarryMainView.h"

#include "QuarryLookAndFeel.h"

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

    // Obsidian plus the boundary and focus ring it does not draw. See
    // Lib/Components/QuarryLookAndFeel.h for why this is a subclass and not an edit.
    quarry::lnf::LookAndFeel mLnF;
};
