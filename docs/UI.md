# Quarry — UI

How the interface is put together: where the palette lives, what each token is for, and
which component to reach for. The accessibility rules these have to satisfy are in
[ACCESSIBILITY.md](ACCESSIBILITY.md); current conformance is in [UI_AUDIT.md](UI_AUDIT.md).

---

## Where things live

| | |
|---|---|
| `ThirdParty/okstudio/include/okstudio/Obsidian.h` | **Owns the palette.** The look and feel, the accent API, the fonts, and the drawing primitives. Shared across the OK Studio line. |
| `Lib/Components/UIDefines.h` | Quarry's aliases and the roles Obsidian does not cover. Aliases only, never redefinitions. |
| `Lib/Components/` | Reusable widgets: `Knob`, `NumericTextEditor`, meters. |
| `Quarry/Source/Components/` | Product components: the main view, piano roll, regions, playhead. |
| `Quarry/Source/Components/Views/` | Screens and panels. `SamplePageView`, `TranscriptionSummary`, the options views. |

**Obsidian is upstream.** It is vendored under `ThirdParty` and shared with Keys and the
rest of the line. Changing a value there changes every product. If Quarry needs a colour
that is Quarry's alone, it belongs in `UIDefines.h` with a comment saying why. If Quarry
needs an Obsidian value changed, that is an upstream conversation, and `UPSTREAM.txt`
records where the copy came from.

---

## The accent is per instance, not global

A DAW loads every plugin instance into one process. A global accent would repaint every
track's instance whenever any one of them changed colour.

So each editor owns one `okstudio::obsidian::LookAndFeel`, picks a colour with
`setAccent()`, and components read it back with:

```cpp
const auto accent = okstudio::obsidian::accentOf(*this);
g.setColour(accent.base);
```

`accentOf` walks the look-and-feel chain JUCE already maintains up to the editor, so a
component never needs to know who owns it. It falls back to cyan outside an
Obsidian-skinned editor, which is what makes it safe in a unit test.

**Never freeze the accent into a constant.** `UIDefines.h` deliberately has no accent
token. Eight accents ship (`accentChoices()`); every one of them must work, so anything
painted in the accent has to be legible across all eight.

Each accent is a triple: `base` for lit states, `hot` for gradient highlights, `deep` for
gradient shadows and for a surface that carries dark text.

---

## Surfaces

Darkest to lightest. A control sits **on** a surface; the pairing determines what border
it needs.

| Token | Value | For |
|---|---|---|
| `VOID_BG` | `#0e0f12` | The window ground, and the waveform bed. |
| `WELL_BG` | `#101216` | Inset grooves and value wells. Recessed, not raised. |
| `PANEL_BOT` | `#1c1f24` | Bottom of a panel gradient. |
| `PANEL_BG` | `#22252b` | Flat panel fill. |
| `PANEL_TOP` | `#262a31` | Top of a panel gradient. |
| `CONTROL_BG` | `#262a31` | A raised control chip. |

Two notes on this table, both of which are the audit's findings and are stated here so
nobody re-derives them:

1. **`PANEL_TOP` and `CONTROL_BG` are the same value.** A control on the top of a panel
   has no fill separation from it at all. Controls are therefore identified by their
   **border**, not their fill (see below).
2. **The whole ramp spans #0e0f12 to #262a31.** That is not enough range to carry
   hierarchy by fill alone. Hierarchy comes from border, from the accent, and from
   position, not from a lighter grey.

Panels are drawn with `raisedFill`, which needs a top and a bottom for its gradient. A
flat fill this close to the ground disappears.

### List rows

| Token | Value | For |
|---|---|---|
| `PANEL_BOT` | `#1c1f24` | The list ground, and every even band. |
| `ROW_ALT` | `#222429` | Every odd band. 1.06:1 against the ground. |
| `CONTROL_BORDER` | `#6e7381` | The rule down a group's gutter. |

Both lists go through `quarry::lnf::listRowBackground`. Do not paint a row background by
hand: two lists selecting differently is worse than neither.

**The band follows the structure of the list, not the row number.** This is the whole
rule, and getting it wrong is what made the sources list unreadable:

- **Sources** is grouped by application, so one band is one application. Every window of
  Chrome shares a tint and the next application flips. Pass `_sourceBandIndex(row)`.
- **Library** is flat, so one band is one row. Pass the row number.

An every-other-row stripe in the sources list cut straight through the groups and split
each application into striped and unstriped halves. A band that fights the structure is
worse than no band.

Bands are drawn square and full width. Rounded corners on each row would scallop the edges
of a multi-row band and undo the run it exists to make.

**A window belongs to an application by three signals**, because one was not enough:

1. The shared band behind the whole group.
2. A `CONTROL_BORDER` rule down the gutter, drawn full row height so consecutive rows join
   into one unbroken line (`listGroupRule`).
3. The `appGutterWidth` name column, which keeps its width on every row of the group even
   though the name is printed only on the first, so the titles stay aligned.

Before those, rows two onward of a group were orphans with an empty column where the name
would be, and nothing said which application they belonged to.

**The window title is `TEXT_MAIN`; the application name is `TEXT_DIM`.** The title is what
is being chosen between, so it is the primary text. A single-window application has no
title to show, so its name takes `TEXT_MAIN` instead: that row *is* the thing being picked.

**Selection is the accent bar, not the fill**, and that is forced rather than chosen.
`TEXT_DIM` is drawn on selected rows (the *source guessed* caption, and the gutter name),
which caps any row fill at about `CONTROL_BG` before `TEXT_DIM` drops under 4.5:1. At that
cap the selected fill measures 1.15:1 against the ground and 1.07:1 against the band,
which is nothing. So a 3px accent bar down the left edge carries it at 7.89:1, and the
fill only warms the row. That also settles SC 1.4.1: the selection is a shape as well as a
colour, so it survives greyscale and colour blindness.

### Borders

| Token | Value | For |
|---|---|---|
| `CONTROL_BORDER` | `#6e7381` | **The boundary of any interactive control.** The dimmest cool grey clearing 3:1 against every surface above. |
| `HAIRLINE` | `#2a2e35` | Decorative rules and dividers only. **Never a control boundary** — it is 1.06:1 against `CONTROL_BG` and does not delimit anything. |

This is the single most important rule in this document. In a dark theme a 3:1 *fill*
would mean a light grey button, which is wrong for this product, so:

> **A control is identified by its border, not its fill. Fill stays dark.**

---

## Text

| Token | Value | For |
|---|---|---|
| `TEXT_MAIN` | `#e9ecf0` | Values, button labels, anything the user reads to make a decision. |
| `TEXT_DIM` | `#8a919c` | Field labels, units, secondary copy. |

**There is no third tier**, and the reason is recorded at `UIDefines.h:88-95`: a fainter
tier was tried, every use of it turned out to be text, and text answers to 4.5:1. The
faintest step clearing 4.5:1 against `PANEL_TOP` is #8a919b, which is `TEXT_DIM`. So the
quiet tier and the dim tier are the same colour, and keeping two names for it only
invited the next person to pick the failing one.

If a label genuinely needs to recede further, **give it a darker plate, not a darker
grey.** Obsidian's `textFaint` #5a6068 exists upstream but measures 2.42:1 on a panel and
must not be used for text in Quarry.

Do not dim text with alpha. On a dark theme an alpha-dimmed light foreground loses
contrast faster than it looks like it should.

### Fonts

| Call | Use |
|---|---|
| `ui(h)` | Body and values. Segoe UI, falls back to the platform sans. |
| `uiSemi(h)` | Emphasis, button labels. |
| `micro(h = 10)` | Section labels. Tracked caps; **callers pass uppercase text**, the function does not uppercase for you. |
| `UIDefines::ROW_TITLE_FONT()` | 12.5pt semibold. A list row's primary text: the window title, or the name of a single-window application. |
| `UIDefines::ROW_META_FONT()` | 9.5pt regular. A list row's secondary text: the application name, timestamps, counts. |

**Hierarchy is size, weight and colour together, never colour alone.** The lists used to be
a single 10pt `LABEL_FONT` throughout, which left colour as the only lever and made every
row read the same. `ROW_TITLE_FONT` and `ROW_META_FONT` are the two ends of that scale.
Refactoring UI's framing is the useful one: emphasise by de-emphasising, so the secondary
text gets smaller, lighter and dimmer rather than the primary getting louder.

Nothing is embedded. Text is never baked into an image, so everything scales with the
resizable editor.

---

## Semantic colours

| Token | Value | For |
|---|---|---|
| accent `base` | per instance | Lit, active, selected, focused. |
| `RECORD_RED` | `#d84a60` | The record light. **Graphics only.** As text on a panel it is 3.71:1 and fails AA; use `#ff6b7f` (5.60:1) when it must be read. |
| amber | `#d9a441` | Low-confidence bars. Must always be paired with a non-colour signal. |
| `KEY_WHITE` / `KEY_BLACK` | `#c9ced6` / `#15181c` | Piano roll keys. Not an Obsidian role: a keyboard has to read as a keyboard. |

---

## Buttons

The product currently has one button style. It needs four, because it has four kinds of
action and no way to tell them apart.

| Role | Fill | Border | Text | For |
|---|---|---|---|---|
| **Primary** | accent `base` | none needed | `VOID_BG` (9.15:1) | The one action a screen is for. *Save*. **At most one per screen.** |
| **Secondary** | `CONTROL_BG` | `CONTROL_BORDER` | `TEXT_MAIN` | Ordinary actions. *show notes*, *SNAP TO IT*, *< SAMPLES*. The default. |
| **Quiet** | transparent | none | `TEXT_DIM` | Tertiary, inside a dense row. Must sit next to something bordered so it still reads as a control. |
| **Destructive** | `CONTROL_BG` | `#ff6b7f` | `#ff6b7f` | Discards user work. The trash / clear-take. |

A primary button on an accent fill takes **dark** text, not white: white on #35c4d7 is
2.09:1 and fails badly, while `VOID_BG` on it is 9.15:1.

### States

| State | How |
|---|---|
| Rest | As the table above. |
| Hover | Lift the fill **and** brighten the border to `TEXT_MAIN`. Hover is feedback, not information, so it is not held to 3:1 (see ACCESSIBILITY.md 1.3); it does have to be obvious, and `brighter(0.12f)` alone at 1.40:1 is not. |
| Pressed | **Geometry, not colour.** The rest surface is near the floor, so no darker value reaches 3:1. Pass `false` for the catch-light in `raisedFill` so the chip seats. |
| Toggled on | Accent fill or accent border, plus `glowRect`. This one **is** information and must clear 3:1 against the off state. The accent does, at 6.88:1. |
| Focused | 2px accent ring outside the boundary. Always, on every focusable control. |
| Disabled | A dedicated dim border and text token. Not `beginTransparencyLayer`, which lands at 1.09:1. |

Hover and focus are different states and must look different. A control that is both must
still show the focus ring.

---

## Icon buttons

Icon-only controls are `DrawableButton`s recoloured through `recolourIcon`. They carry
two obligations the text buttons do not:

- **A 34x34px minimum hit area** (`okstudio::ui::minHitPx`), even where the glyph is
  smaller. Extend the hit area, do not grow the graphic.
- **`setTitle()` with a real name.** There is no text to fall back on, so without it a
  screen reader announces an unlabelled button. A tooltip does not substitute.

The header row is seven of these in a row, which makes it the place where both rules
matter most.

---

## Layout

The shipped window is 1000x755 and resizable.

```
┌─────────────────────────────────────────────────────────┐
│ wordmark          [< SAMPLES]      transport icons      │  header
├─────────────────────────────────────────────────────────┤
│ DRIVER / INPUT / CHANNELS / LEVEL            hint text  │  device strip
├───────────────────────┬─────────────────────────────────┤
│ TRANSCRIPTION         │                                 │
│   knobs, pitch bend   │      waveform                   │
├───────────────────────┤                                 │
│ SCALE QUANTIZE        ├─────────────────────────────────┤
│   range, key, snap    │      drag strip                 │
├───────────────────────┼─────────────────────────────────┤
│ TIME QUANTIZE         │      summary + confidence bars  │
│   division, tempo     │                                 │
├───────────────────────┴─────────────────────────────────┤
│ SAVE TO   path              next name    [Wav][Midi][Save] │  footer
└─────────────────────────────────────────────────────────┘
```

- Left column is **settings**, main region is **what was heard**, footer is **output**.
  A control that changes what the transcriber does belongs left; a control that acts on
  the result belongs in the footer.
- Sections are panels with a `micro()` caps label and an enable toggle in the label row.
- `LEFT_SECTIONS_TOP_PAD` (24px) is the gap above each left-column section.
- Corner radii: `radius` 6px for controls, `panelRadius` 8px for panels.
- Reserve 2px around focusable controls so the focus ring is not clipped by a parent.

Everything must be reachable and operable by pointer alone: that is the line-wide
contract in `okstudio/MouseOnly.h` and it comes before anything about the keyboard.
Where the keyboard does apply (see ACCESSIBILITY.md section 2.2), tab order follows this
same reading order, set explicitly where child creation order does not match.

---

## Adding a component

1. **Reach for an existing widget first.** `Knob`, `NumericTextEditor`, the meter helpers
   in `SamplePageView.cpp`.
2. **Take colours from the tokens.** No hex literals in component code. If a role is
   missing, add it to `UIDefines.h` with a comment on why, rather than inlining a value.
3. **Read the accent with `accentOf(*this)`**, never a constant.
4. **Give every control a border, a focus ring, and a `setTitle()`.**
5. **Set `setAccessible(false)`** on anything decorative.
6. **Run `python tools/contrast_check.py`.** It runs in CI and fails the build on a
   regression.
7. **Tab through it, and desaturate a screenshot.** If you cannot tell two things apart
   in greyscale, they were relying on hue alone.
