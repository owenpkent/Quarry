#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <okstudio/Fonts.h>

QuarryEditor::QuarryEditor(QuarryAudioProcessor& p)
    : AudioProcessorEditor(&p)
{
    mMainView = std::make_unique<QuarryMainView>(p);

    addAndMakeVisible(*mMainView);
    setSize(1000, 755);

    getLookAndFeel().setDefaultSansSerifTypeface(UIDefines::MONTSERRAT_REGULAR());

    // And the same two faces to the kit, which draws the other half of the window and did not
    // hear the line above. Obsidian asked for "Segoe UI" by name, and a request by family name
    // goes past the default typeface, so the menus, buttons and combo boxes were drawn in a
    // Windows system font while everything Quarry draws itself was drawn in Montserrat. One
    // window, two typefaces, on every machine, and a third on any Mac.
    //
    // Through fonts::useEmbedded rather than obsidian::setUiTypefaces directly, because it is
    // the call every product on the line makes and they should all make the same one. The bytes
    // are the kit's own faces, vendored under ThirdParty/okstudio/data/fonts and compiled in
    // from there; Quarry stopped carrying its own copy when Montserrat became the line's face
    // rather than this product's.
    //
    // Here rather than in QuarryMainView because it is one setting for the whole window and the
    // editor owns all of it, and before mMainView is given the look and feel below, so nothing
    // paints in the fallback first.
    okstudio::fonts::useEmbedded(BinaryData::MontserratRegular_ttf,
                                 BinaryData::MontserratRegular_ttfSize,
                                 BinaryData::MontserratSemiBold_ttf,
                                 BinaryData::MontserratSemiBold_ttfSize);

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
