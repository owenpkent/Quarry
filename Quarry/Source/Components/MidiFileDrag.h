//
// Created by Damien Ronssin on 11.03.23.
//

#ifndef MidiFileDrag_h
#define MidiFileDrag_h

#include <JuceHeader.h>

#include "MidiFileWriter.h"
#include "PluginProcessor.h"
#include "UIDefines.h"

class MidiFileDrag : public Component
{
public:
    explicit MidiFileDrag(QuarryAudioProcessor* processor);

    ~MidiFileDrag() override;

    void resized() override;

    void paint(Graphics& g) override;

    void mouseDown(const MouseEvent& event) override;

    void mouseEnter(const MouseEvent& event) override;

    void mouseExit(const MouseEvent& event) override;

private:
    /** The folder saved takes go to, so a dragged transcription lands beside the take it
        came from. Shares SampleFolderId with the sample bar rather than holding its own:
        one folder chosen once is the whole point of the setting.
    */
    juce::File _folder() const;

    QuarryAudioProcessor* mProcessor;

    MidiFileWriter mMidiFileWriter;
};

#endif // MidiFileDrag_h
