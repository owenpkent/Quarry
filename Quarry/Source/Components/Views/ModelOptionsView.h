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
 * The MODEL panel: a picker, a line saying what the selected engine is for and when to reach
 * for it, a line saying whether it can actually be reached, and the built-in decoder's three
 * rotaries folded away behind ADVANCED.
 *
 * The picker is grouped rather than flat, and that is the point of it. Seven rows reading
 * "Built-in / Kong / Transkun / Muscriptor / Kong + separation / ..." are seven proper nouns:
 * six of them are the names their authors chose, none of them says what it is for, and the
 * only way to find out used to be to select one and read the line underneath. So each run of
 * engines sits under a heading naming the material it is for, and each row carries what that
 * engine measures in a second column, which is what separates Kong from Transkun once the
 * heading has told you they are both for piano. An engine this machine cannot reach says why
 * in that column instead of being greyed with no reason given, and the why matters: an engine
 * that is genuinely missing from a working sidecar wants pip, and one on a machine with no
 * sidecar configured at all wants docs/SIDECAR.md. "Not installed" for both would send half
 * the readers after the wrong fix.
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
    , public AudioProcessorParameter::Listener
    , public AsyncUpdater
{
public:
    explicit ModelOptionsView(QuarryAudioProcessor& inProcessor);

    ~ModelOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

    /** Arrives on whichever thread moved ENGINE, which for an automated one is the audio thread,
        so it only sets the flag AsyncUpdater already owns. See handleAsyncUpdate. */
    void parameterValueChanged(int parameterIndex, float newValue) override;

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void handleAsyncUpdate() override;

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

    /**
     * Fills the picker's menu from the catalog: a heading above each run of engines that share
     * one, and each row's second column from mEngineAvailable and mUnavailableReason.
     *
     * Written through ComboBox::getRootMenu rather than addItemList, because a heading is a
     * PopupMenu item and a second column is a PopupMenu item's shortcut text, and ComboBox
     * offers no way to set the second of those. Rebuilding is safe as long as the item *text*
     * never changes: ComboBox::getSelectedItemIndex compares the label it is showing against
     * the text of the item it thinks is selected, and the parameter attachment reads it. The
     * heading, the second column and the enablement are all free to change; the name is not.
     */
    void _buildMenu();

    /** The index the parameter currently holds, clamped into the catalog. */
    int _selectedEngine() const;

    /** Reads the sidecar's answer and rewrites the two text lines and the picker's enablement. */
    void _refreshAvailability();

    void _applyEngineChange();

    /**
     * The picker's tooltip: what the selected engine is for, what it reports, and -- when there
     * is one -- the whole of what went wrong.
     *
     * The status line under the picker is drawn into one 238 px row and clipped, not wrapped,
     * and the one thing it can say that is not written anywhere else is a message from a Python
     * process: a path, a traceback line, "timed out waiting for ready". Those do not fit, and a
     * reason that ellipsises after four characters is not a reason. The line keeps the summary;
     * this keeps the text.
     */
    void _refreshTooltip();

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<ComboBox> mEnginePicker;
    std::unique_ptr<ComboBoxParameterAttachment> mEnginePickerAttachment;

    std::unique_ptr<TextButton> mAdvancedButton;

    std::unique_ptr<Knob> mNoteSensitivity;
    std::unique_ptr<Knob> mSplitSensitivity;
    std::unique_ptr<Knob> mMinNoteDuration;

    // Both of the things this panel has to notice now push rather than being polled for. The
    // ENGINE parameter arrives through parameterValueChanged, on whatever thread moved it, and
    // is marshalled by AsyncUpdater because every response here touches components; the sidecar
    // probe landing arrives through TranscriptionManager::onSidecarStatusChanged, already on the
    // message thread. What replaced a 15 Hz timer that copied the whole status under a lock and
    // rebuilt three strings, for the life of the editor, to watch an answer that settles once.
    int mLastSeenEngine = -1;
    String mWhenLine;
    String mStatusLine;
    bool mStatusIsProblem = false;

    // Every row live until the probe answers; see _refreshAvailability. Held here rather than
    // read back off the menu because the menu is rebuilt from it, not the other way round.
    std::array<bool, EngineCatalog::NumEngines> mEngineAvailable {};

    // What a greyed row says in place of what it reports. One string for all of them, because
    // every reason a sidecar engine can be unreachable is a fact about the sidecar rather than
    // about that engine, up to and including the one that names an engine.
    String mUnavailableReason;

    bool mAdvancedOpen = false;

    // What the column was last told to stack against. preferredHeight() is a function of the
    // live parameter, so it cannot be used as its own "before" value: by the time anything here
    // notices the engine moved, it already reports the new answer.
    int mLastReportedHeight = -1;
};

#endif // ModelOptionsView_h
