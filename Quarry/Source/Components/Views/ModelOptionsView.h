//
// Which model listens, and what it is for. The first thing on the page, because it is the
// choice everything under it depends on.
//

#ifndef ModelOptionsView_h
#define ModelOptionsView_h

#include <JuceHeader.h>

#include "EngineCatalog.h"
#include "Knob.h"
#include "LeftColumnLayout.h"
#include "PluginProcessor.h"
#include "QuarryTooltips.h"
#include "UIDefines.h"

/**
 * The MODEL panel: a picker, a line saying what the selected engine is for, a line saying
 * whether it can actually be reached, and the built-in decoder's three rotaries folded away
 * behind ADVANCED.
 *
 * This replaces the TRANSCRIPTION panel, which had the priorities exactly inverted. It gave
 * three rotaries permanent space to a decoder the user could not choose, while the choice of
 * decoder -- worth more than every one of those rotaries put together -- was an environment
 * variable read once at startup and mentioned nowhere. Worse, on a sidecar take those three
 * rotaries did nothing at all: they are BasicPitch decoder parameters, a sidecar take never
 * goes near BasicPitch, and nothing said so. Three live-looking controls wired to nothing.
 *
 * So the rotaries are gone from the top level, and they exist only while the engine that owns
 * them is the engine that is selected. A control the current engine does not use is not
 * dimmed here, it is absent: dimming asks the reader to work out why, and the answer is not
 * something they did wrong.
 */
class ModelOptionsView
    : public Component
    , public Timer
{
public:
    explicit ModelOptionsView(QuarryAudioProcessor& inProcessor);

    ~ModelOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

    /**
     * How tall this panel wants to be right now. Changes when ADVANCED opens and when the
     * selected engine gains or loses its rotaries, so the column that owns it has to ask again
     * rather than assume; see onPreferredHeightChanged.
     */
    int preferredHeight() const;

    /** Called when preferredHeight() has changed and the left column needs re-stacking. */
    std::function<void()> onPreferredHeightChanged;

private:
    /** True when the selected engine is the built-in one, whose decoder the rotaries drive. */
    bool _showsDecoderKnobs() const;

    /** The index the parameter currently holds, clamped into the catalog. */
    int _selectedEngine() const;

    /** Reads the sidecar's answer and rewrites the two text lines and the picker's enablement. */
    void _refreshAvailability();

    void _applyEngineChange();

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<ComboBox> mEnginePicker;
    std::unique_ptr<ComboBoxParameterAttachment> mEnginePickerAttachment;

    std::unique_ptr<TextButton> mAdvancedButton;

    std::unique_ptr<Knob> mNoteSensitivity;
    std::unique_ptr<Knob> mSplitSensitivity;
    std::unique_ptr<Knob> mMinNoteDuration;

    // Polled rather than listened for. AudioProcessorParameter::Listener delivers on whatever
    // thread moved the parameter, which for an automated one is the audio thread, and every
    // response here touches components. The timer is needed anyway to notice the sidecar probe
    // landing on a background thread, so one mechanism covers both.
    int mLastSeenEngine = -1;
    String mTraitLine;
    String mStatusLine;
    bool mStatusIsProblem = false;

    bool mAdvancedOpen = false;

    // What the column was last told to stack against. preferredHeight() is a function of the
    // live parameter, so it cannot be used as its own "before" value: by the time anything here
    // notices the engine moved, it already reports the new answer.
    int mLastReportedHeight = -1;
};

#endif // ModelOptionsView_h
