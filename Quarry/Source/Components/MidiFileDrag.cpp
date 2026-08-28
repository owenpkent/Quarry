//
// Created by Damien Ronssin on 11.03.23.
//

#include "MidiFileDrag.h"

#include "TakeNaming.h"

#include <okstudio/Obsidian.h>

MidiFileDrag::MidiFileDrag(QuarryAudioProcessor* processor)
    : mProcessor(processor)
{
}

MidiFileDrag::~MidiFileDrag() = default;

File MidiFileDrag::_folder() const
{
    return TakeNaming::folder(mProcessor->getValueTree());
}

void MidiFileDrag::resized()
{
}

void MidiFileDrag::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(
        g, getLocalBounds().toFloat().reduced(0.5f), 4.0f, PANEL_TOP, PANEL_BOT);

    g.setColour(TEXT_MAIN);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("DRAG THE MIDI FILE FROM HERE", getLocalBounds(), juce::Justification::centred);
}

void MidiFileDrag::mouseDown(const MouseEvent& event)
{
    const auto folder = _folder();

    if (!folder.isDirectory()) {
        auto result = folder.createDirectory();

        if (result.failed()) {
            NativeMessageBox::showMessageBoxAsync(
                juce::MessageBoxIconType::NoIcon, "Error", "Could not create " + folder.getFullPathName());
            return;
        }
    }

    // The dragged file stays in the folder afterwards, so a name already in use belongs to an
    // earlier export and must not be written over. Legalised the same way the sample bar
    // legalises it, so the two agree on what a dropped file is called.
    const String dropped = mProcessor->getSourceAudioManager()->getDroppedFilename();
    const String base = File::createLegalFileName(
        dropped.isEmpty() ? String("QuarryTranscription") : dropped + "_QuarryTranscription");

    // Reuse this session's file when the same take goes out again.
    //
    // The file has to exist before the drag starts, and performExternalDragDropOfFiles only
    // reports that a drag began: nothing here can tell a drop into a track from a click that
    // moved no further, or from a drag let go over the desktop. Writing a fresh numbered file
    // each time turned every one of those into a copy, so aiming at a track four times left
    // four identical takes in the user's music folder and walked the counter towards the
    // point where freeStem gives up. Rewritten rather than merely re-dragged, since the
    // transcription can be re-tuned between two drags of it.
    auto out_file = mLastExport;

    if (mLastBase != base || mLastExport.getParentDirectory() != folder || ! mLastExport.existsAsFile()) {
        const String stem = TakeNaming::freeStem(folder, base, {".mid"});

        if (stem.isEmpty()) {
            NativeMessageBox::showMessageBoxAsync(
                juce::MessageBoxIconType::NoIcon, "Error", "No free name left in " + folder.getFileName());
            return;
        }

        out_file = folder.getChildFile(stem + ".mid");
    }

    double export_bpm = mProcessor->getValueTree().getProperty(NnId::ExportTempoId, 120.0);

    auto success_midi_file_creation = mMidiFileWriter.writeMidiFile(
        mProcessor->getTranscriptionManager()->getNoteEventVector(),
        out_file,
        mProcessor->getTranscriptionManager()->getTimeQuantizeOptions().getTimeQuantizeInfo(),
        export_bpm,
        static_cast<PitchBendModes>(mProcessor->getParameterValue(ParameterHelpers::PitchBendModeId)),
        mProcessor->getTranscriptionManager()->getPedalEvents());

    if (!success_midi_file_creation) {
        NativeMessageBox::showMessageBoxAsync(
            juce::MessageBoxIconType::NoIcon, "Error", "Could not create the midi file.");
        return;
    }

    mLastExport = out_file;
    mLastBase = base;

    StringArray out_files = {out_file.getFullPathName()};

    DragAndDropContainer::performExternalDragDropOfFiles(out_files, false, this);
}

void MidiFileDrag::mouseEnter(const MouseEvent& event)
{
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void MidiFileDrag::mouseExit(const MouseEvent& event)
{
    setMouseCursor(juce::MouseCursor::ParentCursor);
}
