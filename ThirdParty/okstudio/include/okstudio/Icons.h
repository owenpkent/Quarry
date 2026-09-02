#pragma once

/*
    Icons for the OK Studio line, as SVG source.

    Header-only and JUCE-free on purpose. These are plain string constants, so this header costs
    a consumer nothing and can be included from the pure-logic headers that must not reach
    juce_gui_basics (see CLAUDE.md, "A consumer's test exe links only okstudio_kit"). Turning one
    into something drawable is the consumer's job and needs juce_gui_basics:

        auto d = juce::Drawable::createFromImageData (okstudio::icons::folderOpen.data(),
                                                      okstudio::icons::folderOpen.size());

    Two things about the artwork that a consumer has to know.

    The stroke is #000000, not Lucide's own "currentColor". JUCE 8's SVG parser resolves hex and
    named colours and knows nothing about currentColor, so an icon shipped as Lucide publishes it
    would come out of createFromImageData with whatever the parser fell back to. Black is a
    colour every consumer already replaces: recolour on load, against the theme, the way a line
    plugin recolours anything else it draws. Doing so is not optional -- black on a dark ground
    is 1.09:1 -- and a consumer that draws one of these unrecoloured has an invisible button.

    They are 24x24 on a 2 px stroke, which is Lucide's grid and not the 1.8 px the icons this set
    replaced were cut at. Do not restyle them to match something older: the value of taking a
    published set is that the next icon anyone adds already agrees with these, and a locally
    reweighted copy throws that away for one screen's worth of consistency.

    Adding one: take it from the Lucide release pinned below, compact the whitespace, swap
    currentColor for #000000, and give it a name saying what it means here rather than what it
    draws -- "close" rather than "x", because the next surface that needs dismissing should not
    have to know that Lucide calls it a letter. Add it to the table in KitTests too, which is
    what checks it survived the paste.

    ------------------------------------------------------------------------------------------
    Artwork: Lucide (https://lucide.dev), pinned at commit
    eb3c04d059d92903f992f0d3b7c732deca6da7a5
    fetched 2026-08-31. Unmodified but for the stroke colour and whitespace above.

    ISC License

    Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of Feather
    (MIT). All other copyright (c) for Lucide are held by Lucide Contributors 2022.

    Permission to use, copy, modify, and/or distribute this software for any purpose with or
    without fee is hereby granted, provided that the above copyright notice and this permission
    notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
    SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
    THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
    DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
    OR PERFORMANCE OF THIS SOFTWARE.
    ------------------------------------------------------------------------------------------
*/

#include <string_view>

namespace okstudio::icons
{

/** The grid every icon here is drawn on, for a consumer sizing a button around one. */
inline constexpr int viewBoxPx = 24;

/** The stroke every icon here is drawn with, for a consumer matching it in something hand-drawn. */
inline constexpr float strokeWidthPx = 2.0f;

// Files and folders.

/** A closed folder: somewhere things are kept. (Lucide "folder"). */
inline constexpr std::string_view folder =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"/></svg>)SVG";

/** An open folder: showing what is kept there, in the desktop's own window. (Lucide "folder-open"). */
inline constexpr std::string_view folderOpen =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2"/></svg>)SVG";

/** Writing to disk. (Lucide "save"). */
inline constexpr std::string_view save =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/></svg>)SVG";

/** Discarding what a surface holds. Destructive, and the only icon here that is. (Lucide "trash-2"). */
inline constexpr std::string_view trash =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 11v6"/><path d="M14 11v6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>)SVG";

/** Leaving for somewhere this window does not own. (Lucide "external-link"). */
inline constexpr std::string_view openExternal =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/></svg>)SVG";

// Transport.

/** Start, or resume. (Lucide "play"). */
inline constexpr std::string_view play =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"/></svg>)SVG";

/** Stop where you are, keeping the position. (Lucide "pause"). */
inline constexpr std::string_view pause =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="14" y="3" width="5" height="18" rx="1"/><rect x="5" y="3" width="5" height="18" rx="1"/></svg>)SVG";

/** Back to the beginning, which is not the same as stopping. (Lucide "skip-back"). */
inline constexpr std::string_view skipToStart =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.971 4.285A2 2 0 0 1 21 6v12a2 2 0 0 1-3.029 1.715l-9.997-5.998a2 2 0 0 1-.003-3.432z"/><path d="M3 20V4"/></svg>)SVG";

/** Armed and not yet recording. The ring is empty because nothing is in it yet. (Lucide "circle"). */
inline constexpr std::string_view record =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/></svg>)SVG";

/** Recording now. Paired with record, and differing in shape as well as in the
    colour a consumer paints it, so the state does not rest on hue alone. (Lucide "circle-dot"). */
inline constexpr std::string_view recording =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="1"/><circle cx="12" cy="12" r="10"/></svg>)SVG";

// View.

/** Drawing inward to a centre line: a view held to something. (Lucide "fold-horizontal"). */
inline constexpr std::string_view foldHorizontal =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12h6"/><path d="M22 12h-6"/><path d="M12 2v2"/><path d="M12 8v2"/><path d="M12 14v2"/><path d="M12 20v2"/><path d="m19 9-3 3 3 3"/><path d="m5 15 3-3-3-3"/></svg>)SVG";

/** Spreading outward from a centre line: a view free to move. Paired with
    foldHorizontal. (Lucide "unfold-horizontal"). */
inline constexpr std::string_view unfoldHorizontal =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M16 12h6"/><path d="M8 12H2"/><path d="M12 2v2"/><path d="M12 8v2"/><path d="M12 14v2"/><path d="M12 20v2"/><path d="m19 15 3-3-3-3"/><path d="m5 9-3 3 3 3"/></svg>)SVG";

/** More below, or a menu that opens downward. (Lucide "chevron-down"). */
inline constexpr std::string_view chevronDown =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6"/></svg>)SVG";

// Sound.

/** Silenced. (Lucide "volume-x"). */
inline constexpr std::string_view mute =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.7.7 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.7.7 0 0 0 11 19.298z"/><path d="m16.5 14.5 5-5"/><path d="m16.5 9.5 5 5"/></svg>)SVG";

/** Sounding. Paired with mute. (Lucide "volume-2"). */
inline constexpr std::string_view unmute =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z"/><path d="M16 9a5 5 0 0 1 0 6"/><path d="M19.364 18.364a9 9 0 0 0 0-12.728"/></svg>)SVG";

// Everything else.

/** The controls behind the ones on screen. (Lucide "settings"). */
inline constexpr std::string_view settings =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915"/><circle cx="12" cy="12" r="3"/></svg>)SVG";

/** Adding one of whatever the surface is a list of. (Lucide "plus"). */
inline constexpr std::string_view plus =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>)SVG";

/** Dismissing. Never deleting, which is what trash is for. (Lucide "x"). */
inline constexpr std::string_view close =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>)SVG";

/** Done, or chosen. (Lucide "check"). */
inline constexpr std::string_view check =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000000" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6 9 17l-5-5"/></svg>)SVG";

} // namespace okstudio::icons
