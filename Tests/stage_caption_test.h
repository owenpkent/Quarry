//
// The strip's captions have to fit the strip. ProgressStripLayout reserves the caption a fixed
// 169 px between the bar and Cancel, and ProgressStrip paints it with LABEL_FONT and lets JUCE
// ellipsise the overflow -- which is how "received transcribe request: engine=kong device=cuda"
// became "received transcribe request: en...". Measuring here is the same check sample_bar_test
// runs on its own fixed strings, for the same reason: the truncation is silent at runtime.
//

#ifndef QUARRY_STAGE_CAPTION_TEST_H
#define QUARRY_STAGE_CAPTION_TEST_H

#include <iostream>
#include <string>
#include <vector>

#include "Components/Views/ProgressStripLayout.h"
#include "Components/Views/StageCaption.h"
#include "UIDefines.h"

namespace stage_caption_test_utils
{
static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}
} // namespace stage_caption_test_utils

inline bool stage_caption_test()
{
    using namespace stage_caption_test_utils;
    failures = 0;

    // Every slug tools/sidecar emits, per PROTOCOL.md's "Slugs, in the order a transcribe request
    // can emit them" plus the two the download request adds.
    const std::vector<juce::String> slugs = {
        "received", "load-model", "separate", "stem", "infer", "post", "done",
        "download", "extract-audio"
    };

    const auto font = UIDefines::LABEL_FONT();
    const auto available = static_cast<float>(ProgressStripLayout::captionWidth());

    // The percentage ProgressStrip::_captionFor appends whenever the stage carries a fraction.
    // "stem" and "download" are the two that actually do, but a caption is only safe if it fits
    // with the suffix, so every one is measured wearing it.
    const juce::String widest_suffix = "  100%";

    juce::String widest;
    float widest_width = 0.0f;

    for (const auto& slug : slugs) {
        const auto caption = quarry::stageCaption(slug, "unused: every slug here is mapped");

        check(caption.isNotEmpty(), ("slug \"" + slug + "\" has a caption").toStdString());

        // A mapped slug must not fall through to the sidecar's own line; that fallback exists for
        // slugs a future sidecar adds, and a silent fall-through here would be the bug returning.
        check(caption != "unused: every slug here is mapped",
              ("slug \"" + slug + "\" is mapped, not falling back to the sidecar's text").toStdString());

        const auto width = font.getStringWidthFloat(caption + widest_suffix);

        if (width > widest_width) {
            widest_width = width;
            widest = caption;
        }
    }

    check(widest_width <= available,
          "the widest caption fits the room ProgressStripLayout leaves it");

    std::cout << "  widest caption: \"" << (widest + widest_suffix) << "\" at " << widest_width
              << " px of " << available << std::endl;

    // The fallback itself: an unknown slug still says something, so a newer sidecar is not silent.
    check(quarry::stageCaption("some-future-stage", "a sentence from a newer sidecar")
              == "a sentence from a newer sidecar",
          "an unknown slug falls back to the sidecar's own line");

    if (failures == 0) {
        std::cout << "  PASSED" << std::endl;
        return true;
    }

    std::cout << "  " << failures << " failure(s)" << std::endl;
    return false;
}

#endif // QUARRY_STAGE_CAPTION_TEST_H
