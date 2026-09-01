#include "PluginProcessor.h"
#include "PluginEditor.h"

QuarryEditor::QuarryEditor(QuarryAudioProcessor& p)
    : AudioProcessorEditor(&p)
{
    mMainView = std::make_unique<QuarryMainView>(p);

    addAndMakeVisible(*mMainView);
    setSize(1000, 755);

    getLookAndFeel().setDefaultSansSerifTypeface(UIDefines::MONTSERRAT_REGULAR());

    // And the same two faces to Obsidian, which is the other half of the window and did not
    // hear the line above. It asked for "Segoe UI" by name, and a request by family name goes
    // past the default typeface, so the menus, buttons and combo boxes were drawn in a Windows
    // system font while everything Quarry draws itself was drawn in Montserrat. One window, two
    // typefaces, on every machine -- and on a Mac a third, because Segoe UI is not there to
    // fall back to.
    //
    // Here rather than in QuarryMainView because it is one setting for the whole window and the
    // editor is the one thing that owns all of it, and before mMainView is given the look and
    // feel below, so nothing paints in the old face first.
    okstudio::obsidian::setUiTypefaces(UIDefines::MONTSERRAT_REGULAR(), UIDefines::MONTSERRAT_SEMIBOLD());

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
