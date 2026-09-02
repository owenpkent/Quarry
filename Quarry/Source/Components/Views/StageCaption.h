//
// The short phrase the progress strip shows for a sidecar stage event.
//
// The sidecar writes one human-readable English line per stage ("received transcribe request:
// engine=kong device=cuda"), and the activity drawer shows exactly that, because the drawer has a
// full-width monospace feed to show it in. The strip does not: ProgressStripLayout gives the
// caption 169 px between the bar and Cancel, which is a few words. Handing it the sidecar's
// sentence produced "received transcribe request: en..." -- a caption whose visible half is the
// part that says nothing about which stage is running.
//
// So the strip captions off the stage *slug* instead, which the protocol defines for exactly this
// ("a short slug naming which part of the pipeline is running"; tools/sidecar/PROTOCOL.md). The
// two surfaces now say the same thing at the length each has room for, and nothing is lost: the
// sentence is still in the drawer, one keypress away.
//
// Pure and header-only so Tests/stage_caption_test.h can measure every caption against the width
// the layout actually gives it, the way sample_bar_test measures its own fixed strings.
//

#ifndef StageCaption_h
#define StageCaption_h

#include <JuceHeader.h>

namespace quarry
{

/**
 * The strip's caption for one sidecar stage slug, in the strip's own voice -- the same plain
 * present tense as the built-in captions it sits alongside ("transcribing with built-in",
 * "waiting for sidecar", "downloading").
 *
 * An unrecognised slug falls back to the sidecar's own line rather than to nothing: a sidecar
 * newer than this build can add a stage, and a truncated new caption still beats a blank one.
 * Every slug the protocol currently defines is listed here, so that fallback is for the future,
 * not for today.
 */
inline juce::String stageCaption(const juce::String& inSlug, const juce::String& inText)
{
    if (inSlug == "received")
        return "sent to sidecar";

    if (inSlug == "load-model")
        return "loading the model";

    // Both halves of a sep+ request read the same on the strip: the stem events carry a fraction
    // and the separate events do not, so the bar already tells them apart without the words.
    if (inSlug == "separate" || inSlug == "stem")
        return "separating stems";

    if (inSlug == "infer")
        return "transcribing";

    if (inSlug == "post")
        return "sorting notes";

    if (inSlug == "download")
        return "downloading";

    if (inSlug == "extract-audio")
        return "extracting audio";

    // The strip hides itself a moment later, so this is only ever seen in passing.
    if (inSlug == "done")
        return "finishing";

    return inText;
}

} // namespace quarry

#endif // StageCaption_h
