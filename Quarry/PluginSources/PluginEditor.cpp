#include "PluginProcessor.h"
#include "PluginEditor.h"

QuarryEditor::QuarryEditor(QuarryAudioProcessor& p)
    : AudioProcessorEditor(&p)
{
    mMainView = std::make_unique<QuarryMainView>(p);

    addAndMakeVisible(*mMainView);
    setSize(1000, 640);

    getLookAndFeel().setDefaultSansSerifTypeface(UIDefines::MONTSERRAT_REGULAR());

    mMainView->setLookAndFeel(&mLnF);
}

QuarryEditor::~QuarryEditor()
{
    mMainView->setLookAndFeel(nullptr);
}

void QuarryEditor::paint(juce::Graphics& g)
{
}

void QuarryEditor::resized()
{
    mMainView->setBounds(getLocalBounds());
}
