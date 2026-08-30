//
// Which model listens, what it is for, and when to reach for it.
//

#include "ModelOptionsView.h"

#include <okstudio/Obsidian.h>
#include "QuarryMainView.h"

namespace
{
// The panel's own vertical grid, measured from the top of the component. Everything above
// LEFT_SECTIONS_TOP_PAD is the title row, which sits outside the raised fill.
constexpr int kPickerY = 38;
constexpr int kPickerH = 20;
constexpr int kWhenY = 64;
constexpr int kStatusY = 80;
constexpr int kTextH = 14;
constexpr int kAdvancedY = 100;
constexpr int kAdvancedH = 18;
constexpr int kKnobY = 124;
constexpr int kKnobH = 89;
constexpr int kBottomPad = 10;

constexpr int kLeftMargin = 18;
constexpr int kRowWidth = LeftColumnLayout::MODEL_ROW_WIDTH;

// The column stacks against these three numbers and cannot see this grid, so moving a control
// in here either updates LeftColumnLayout or stops compiling.
static_assert(kStatusY + kTextH + kBottomPad == LeftColumnLayout::MODEL_SIDECAR_ENGINE,
              "MODEL's height with no rotaries no longer matches what the column stacks against");
static_assert(kAdvancedY + kAdvancedH + kBottomPad == LeftColumnLayout::MODEL_ADVANCED_CLOSED,
              "MODEL's height with ADVANCED closed no longer matches what the column stacks against");
static_assert(kKnobY + kKnobH + kBottomPad == LeftColumnLayout::MODEL_ADVANCED_OPEN,
              "MODEL's height with ADVANCED open no longer matches what the column stacks against");
} // namespace

ModelOptionsView::ModelOptionsView(QuarryAudioProcessor& inProcessor)
    : mProcessor(inProcessor)
{
    mEngineAvailable.fill(true);

    mEnginePicker = std::make_unique<ComboBox>("EnginePicker");
    mEnginePicker->setEditableText(false);
    mEnginePicker->setJustificationType(Justification::centredLeft);
    mEnginePicker->setTitle("Engine");

    // Before the attachment, which selects by position and so needs the items to exist.
    _buildMenu();

    mEnginePickerAttachment = std::make_unique<ComboBoxParameterAttachment>(
        *mProcessor.getParams()[ParameterHelpers::EngineId], *mEnginePicker);
    addAndMakeVisible(mEnginePicker.get());

    mAdvancedButton = std::make_unique<TextButton>("AdvancedButton");
    mAdvancedButton->setButtonText("ADVANCED");
    mAdvancedButton->setClickingTogglesState(true);
    mAdvancedButton->setTooltip(QuarryTooltips::mo_advanced);
    mAdvancedButton->setTitle("Advanced decoder settings");
    mAdvancedButton->onClick = [this] {
        mAdvancedOpen = mAdvancedButton->getToggleState();
        _applyEngineChange();
    };
    addAndMakeVisible(mAdvancedButton.get());

    mNoteSensitivity =
        std::make_unique<Knob>(*mProcessor.getParams()[ParameterHelpers::NoteSensitivityId], "NOTE SENS", false);
    mNoteSensitivity->setTooltip(QuarryTooltips::to_note_sensitivity);
    addChildComponent(mNoteSensitivity.get());

    mSplitSensitivity =
        std::make_unique<Knob>(*mProcessor.getParams()[ParameterHelpers::SplitSensitivityId], "SPLIT SENS", false);
    mSplitSensitivity->setTooltip(QuarryTooltips::to_split_sensitivity);
    addChildComponent(mSplitSensitivity.get());

    mMinNoteDuration = std::make_unique<Knob>(
        *mProcessor.getParams()[ParameterHelpers::MinimumNoteDurationId], "MIN DUR", false, " ms");
    mMinNoteDuration->setTooltip(QuarryTooltips::to_min_note_duration);
    addChildComponent(mMinNoteDuration.get());

    // Asked once, as soon as the page exists, rather than on the first take. When no sidecar is
    // configured this answers instantly and costs nothing; when one is, the model load it starts
    // is a cost that take was going to pay anyway, and paying it while someone is still reading
    // the page is better than in the middle of their first transcription.
    if (auto* manager = mProcessor.getTranscriptionManager()) {
        manager->requestSidecarProbe();
    }

    _applyEngineChange();

    startTimerHz(15);
}

ModelOptionsView::~ModelOptionsView()
{ stopTimer(); }

int ModelOptionsView::preferredHeight() const
{
    if (!_showsDecoderKnobs()) {
        return LeftColumnLayout::MODEL_SIDECAR_ENGINE;
    }

    return mAdvancedOpen ? LeftColumnLayout::MODEL_ADVANCED_OPEN : LeftColumnLayout::MODEL_ADVANCED_CLOSED;
}

bool ModelOptionsView::_showsDecoderKnobs() const
{ return !EngineCatalog::isSidecar(_selectedEngine()); }

int ModelOptionsView::_selectedEngine() const
{
    return jlimit(
        0, EngineCatalog::NumEngines - 1, static_cast<int>(mProcessor.getParameterValue(ParameterHelpers::EngineId)));
}

void ModelOptionsView::_buildMenu()
{
    auto* menu = mEnginePicker->getRootMenu();

    // PopupMenu::clear, not ComboBox::clear. The second one deselects, which on a rebuild
    // triggered by a probe landing would silently move the parameter.
    menu->clear();

    String open_group;

    for (int i = 0; i < EngineCatalog::NumEngines; ++i) {
        const auto group = EngineCatalog::groupOf(i);

        if (group != open_group) {
            open_group = group;
            menu->addSectionHeader(group);
        }

        PopupMenu::Item item(EngineCatalog::get(i).displayName);
        item.itemID = i + 1;
        item.isEnabled = mEngineAvailable[static_cast<size_t>(i)];

        // The right-hand column. JUCE calls this the shortcut description and draws it dim and
        // right-aligned, which is the shape wanted here and is already sized for by the look
        // and feel, so it costs no custom item component.
        item.shortcutKeyDescription = item.isEnabled ? EngineCatalog::reportsLine(i) : mUnavailableReason;

        menu->addItem(std::move(item));
    }
}

void ModelOptionsView::resized()
{
    mEnginePicker->setBounds(kLeftMargin, kPickerY, kRowWidth, kPickerH);
    mAdvancedButton->setBounds(kLeftMargin, kAdvancedY, 116, kAdvancedH);

    mNoteSensitivity->setBounds(kLeftMargin, kKnobY, 66, kKnobH);
    mSplitSensitivity->setBounds(106, kKnobY, 66, kKnobH);
    mMinNoteDuration->setBounds(193, kKnobY, 66, kKnobH);
}

void ModelOptionsView::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(g,
                                   Rectangle<float>(0.0f,
                                                    static_cast<float>(LEFT_SECTIONS_TOP_PAD),
                                                    static_cast<float>(getWidth()),
                                                    static_cast<float>(getHeight() - LEFT_SECTIONS_TOP_PAD)),
                                   5.0f,
                                   PANEL_TOP,
                                   PANEL_BOT);

    const float alpha = isEnabled() ? 1.0f : DISABLED_ALPHA;

    // Aligned with SCALE QUANTIZE and TIME QUANTIZE below, whose titles are indented by the
    // enable toggle in their label row. This panel has no toggle, because there is no state of
    // this app in which no model runs, and the empty slot says so more clearly than a switch
    // that is always on would.
    g.setColour(TEXT_MAIN.withAlpha(alpha));
    g.setFont(UIDefines::TITLE_FONT());
    g.drawText("MODEL", Rectangle<int>(24, 0, 250, 17), Justification::centredLeft);

    g.setFont(UIDefines::LABEL_FONT());

    g.setColour(TEXT_DIM.withAlpha(alpha));
    g.drawText(mWhenLine, Rectangle<int>(kLeftMargin + 1, kWhenY, kRowWidth, kTextH), Justification::centredLeft);

    // A problem is brighter, not redder. Everything this line can say is a fact about the
    // machine rather than a mistake someone made, and TEXT_MAIN over TEXT_DIM is a step in
    // attention that already clears contrast on this panel, where a new red would have to earn
    // it from scratch.
    g.setColour((mStatusIsProblem ? TEXT_MAIN : TEXT_DIM).withAlpha(alpha));
    g.drawText(mStatusLine, Rectangle<int>(kLeftMargin + 1, kStatusY, kRowWidth, kTextH), Justification::centredLeft);
}

void ModelOptionsView::timerCallback()
{
    if (mLastSeenEngine != _selectedEngine()) {
        _applyEngineChange();
        return;
    }

    _refreshAvailability();
}

void ModelOptionsView::_applyEngineChange()
{
    mLastSeenEngine = _selectedEngine();

    const bool shows_knobs = _showsDecoderKnobs();

    mAdvancedButton->setVisible(shows_knobs);
    mAdvancedButton->setToggleState(mAdvancedOpen, dontSendNotification);

    // Remembered, not reset, when the engine moves away from the built-in one and back: opening
    // a disclosure is a statement about how much detail you want, not about one engine.
    const bool knobs_visible = shows_knobs && mAdvancedOpen;

    mNoteSensitivity->setVisible(knobs_visible);
    mSplitSensitivity->setVisible(knobs_visible);
    mMinNoteDuration->setVisible(knobs_visible);

    // The tooltip is the only place with room for all three facts at once, so it carries what
    // the panel had to choose between: the material, the reporting, and the general rule.
    const int selected = _selectedEngine();
    mEnginePicker->setTooltip(String(EngineCatalog::get(selected).displayName) + " -- "
                              + EngineCatalog::whenLine(selected) + "\nReports "
                              + EngineCatalog::reportsLine(selected) + ".\n\n"
                              + QuarryTooltips::mo_engine);

    _refreshAvailability();

    const int height = preferredHeight();

    if (height != mLastReportedHeight) {
        mLastReportedHeight = height;

        if (onPreferredHeightChanged != nullptr) {
            onPreferredHeightChanged();
        }
    }
}

void ModelOptionsView::_refreshAvailability()
{
    auto* manager = mProcessor.getTranscriptionManager();

    if (manager == nullptr) {
        return;
    }

    const auto status = manager->getSidecarStatus();
    const int selected = _selectedEngine();

    // Why a greyed row is greyed, which is not the same question as whether it is. With no
    // sidecar configured, nothing is missing that installing anything would supply, and telling
    // someone an engine is not installed sends them after a package when what they need is a
    // Python interpreter and one environment variable.
    String reason = "not installed";

    if (!status.configured) {
        reason = "needs the sidecar";
    } else if (status.error.isNotEmpty()) {
        reason = "sidecar unreachable";
    }

    bool availability_moved = reason != mUnavailableReason;
    mUnavailableReason = reason;

    for (int i = 0; i < EngineCatalog::NumEngines; ++i) {
        // Until the probe has answered, nothing here has grounds to call an engine missing, so
        // every row stays live. After it, the ready line is the whole truth.
        const bool available =
            !EngineCatalog::isSidecar(i) || !status.probed || status.engines.contains(EngineCatalog::get(i).wireName);

        if (mEngineAvailable[static_cast<size_t>(i)] != available) {
            mEngineAvailable[static_cast<size_t>(i)] = available;
            availability_moved = true;
        }
    }

    // Rebuilt rather than poked, because the row's second column changes with its enablement
    // and ComboBox exposes no setter for that half. Only when something actually moved: this
    // runs off a 15 Hz timer and the answer is settled after the first probe.
    if (availability_moved) {
        _buildMenu();
    }

    String status_line;
    bool is_problem = false;

    if (!status.configured) {
        status_line = "No sidecar configured, built-in only";
        is_problem = EngineCatalog::isSidecar(selected);
    } else if (!status.probed) {
        status_line = "Asking the sidecar what it has";
    } else if (status.error.isNotEmpty()) {
        status_line = "Sidecar unreachable: " + status.error;
        is_problem = true;
    } else if (EngineCatalog::isSidecar(selected) && !status.engines.contains(EngineCatalog::get(selected).wireName)) {
        status_line = String(EngineCatalog::get(selected).displayName) + " is not installed";
        is_problem = true;
    } else {
        const auto count = status.engines.size();
        status_line = "Sidecar ready on " + status.device.toUpperCase() + ", " + String(count)
                      + (count == 1 ? " engine" : " engines");
    }

    const auto when_line = EngineCatalog::whenLine(selected);

    if (when_line != mWhenLine || status_line != mStatusLine || is_problem != mStatusIsProblem) {
        mWhenLine = when_line;
        mStatusLine = status_line;
        mStatusIsProblem = is_problem;
        repaint();
    }
}
