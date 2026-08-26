# Quarry — UI Audit

Audited 2026-08-26 against `Quarry_UI.png` (the shipped 1000x755 layout), the token
block in `Lib/Components/UIDefines.h`, and the look and feel in
`ThirdParty/okstudio/include/okstudio/Obsidian.h`.

Every ratio below is computed, not estimated: WCAG 2.2 relative luminance on the exact
sRGB values in the source. Reproduce with `tools/contrast_check.py`.

---

## The headline

**Text contrast is largely fine. Surface contrast is failing everywhere.**

Someone already did the text pass, and the comment at `Lib/Components/UIDefines.h:88-95`
records the reasoning: a third, fainter text tier was tried and removed because it could
not clear 4.5:1 on a raised panel. That work holds up. `TEXT_MAIN` on any ground is
8:1 or better; `TEXT_DIM` clears 4.5:1 on every surface it is actually painted on.

The failure is one axis over, in **SC 1.4.11 Non-text Contrast**, which asks for 3:1
between a UI component and its adjacent background. Not one surface pairing in the
product meets it. That is the measurable form of "everything is the same color".

---

## 1. Controls are indistinguishable from the surfaces they sit on

`CONTROL_BG` and `PANEL_TOP` are **the same value**, `#262a31`. A button drawn on the top
of a panel is not merely low contrast, it is the identical colour, and the only thing
separating it from its background is the 1px catch-light in `raisedFill`.

| Pairing | Ratio | Needs | |
|---|---|---|---|
| `CONTROL_BG` #262a31 on `PANEL_TOP` #262a31 | **1.00:1** | 3.0 | FAIL |
| `CONTROL_BG` on `PANEL_BG` #22252b | **1.07:1** | 3.0 | FAIL |
| `CONTROL_BG` on `PANEL_BOT` #1c1f24 | **1.15:1** | 3.0 | FAIL |
| `CONTROL_BG` on `VOID_BG` #0e0f12 | **1.33:1** | 3.0 | FAIL |
| `HAIRLINE` #2a2e35 on `PANEL_BG` | **1.13:1** | 3.0 | FAIL |
| `HAIRLINE` on `CONTROL_BG` | **1.06:1** | 3.0 | FAIL |
| `WELL_BG` inset on `PANEL_BG` | **1.22:1** | 3.0 | FAIL |

`HAIRLINE` is the token whose entire job is to draw a boundary, and it is 1.06:1 against
the control it borders. It is doing nothing.

This is visible in the screenshot: *show notes*, *SNAP TO IT*, *NEXT SHAKY BAR*, *Save*
and *< SAMPLES* all read as faint rectangles rather than as buttons, and the eye finds
them by their text, not their shape.

## 2. No visual hierarchy between actions

Every `TextButton` in the codebase is assigned one of exactly two backgrounds:
`CONTROL_BG` or `Colours::transparentBlack`. There is no primary, no destructive, no
selected. *Save* (commits work to disk) and *show notes* (toggles a panel) are the same
colour, the same size, and the same weight.

The one exception proves the rule: `SamplePageView.cpp` swaps the record button to
`accent` when armed, which is the only place in the product where a button's colour
carries meaning.

The trash / clear-take button in the header is a plain icon, tinted the same grey as
*play* and *settings*, with no destructive affordance at all.

## 3. Hover and pressed states are below the perception floor

`Obsidian.h:419-443` shades states from the resolved background: `brighter(0.12f)` on
hover, `darker(0.25f)` when down.

| State | Resulting surface | vs rest | |
|---|---|---|---|
| rest | #30343b | — | |
| hover | #464a50 | **1.40:1** | FAIL |
| pressed | #1e2227 | **1.28:1** | FAIL |

A 1.4:1 hover is roughly at the limit of what a person notices on a good monitor and
invisible on a dim laptop panel. Worth noting: **pressed cannot be fixed by darkening**.
The rest surface is already near the floor, so no darker value reaches 3:1. Pressed has
to be signalled by geometry (inset, seat-line inversion, which `raisedFill` already
supports via its last argument) or by going lighter instead.

## 4. There is no focus indicator, anywhere

`drawButtonBackground` has no `hasKeyboardFocus()` branch. Neither does the knob, the
combo box, or the toggle. Nothing in the product ever draws a focus ring.

This fails **SC 2.4.7 Focus Visible** outright, and **SC 2.4.11 Focus Appearance**
(WCAG 2.2), which wants a 3:1 indicator. The accent is right there and would work:
#35c4d7 measures 6.88:1 on a control and 7.33:1 on a panel.

## 5. The transport bar is removed from the keyboard

`QuarryMainView.cpp:300, 320-324` calls `setWantsKeyboardFocus(false)` on the back,
record, play/pause, center, settings and back-to-samples buttons, while the parent takes
`setWantsKeyboardFocus(true)`.

That is six of the product's most important controls made unreachable by Tab. The intent
is legible (the parent wants the spacebar for play/pause) but the cost is that a keyboard
user cannot reach record, cannot reach settings, and cannot leave the sample page. This
fails **SC 2.1.1 Keyboard**. The fix is a key handler on the parent that does not require
stripping focus from the children.

## 6. No accessible names on ~38 interactive controls

Across `Quarry/Source` and `Lib`, there are zero calls to `setTitle`, `setDescription`,
or `setHelpText`. The only accessibility API used in the entire codebase is
`setWantsKeyboardFocus`, and it is used to take focus away.

`DrawableButton`s have no text, so a screen reader announces them as an unlabelled
button. The header row - record, clear, back, play, center, settings, mute - is seven
unlabelled buttons in a row.

Tooltips exist (`QuarryTooltips`) but a JUCE tooltip is not an accessible name.

## 7. Two colours carry meaning that colour alone should not carry

- **Confidence-by-bar** (`TranscriptionSummary.cpp`) distinguishes tiers by hue only,
  cyan against amber #d9a441. That is **SC 1.4.1 Use of Color**. The amber bars in the
  screenshot are legible to most people and invisible to a deuteranope. The bars have
  room for a hatch, a height delta, or a marker.
- **`RECORD_RED`** #d84a60 is used as error text on a panel at **3.71:1**, under the 4.5
  needed. #ff6b7f clears it at 5.60:1 and keeps the hue.

## 8. Disabled controls fall out of the interface

`drawButtonBackground` wraps disabled painting in `beginTransparencyLayer(0.45f)`. On a
panel that yields a #282c32 surface at 1.09:1 against its ground, with `TEXT_DIM` on it
at **1.99:1**. Disabled controls are exempt from contrast minimums, but they still have
to be *identifiable as controls*, and at 1.09:1 they are not.

Related: `DISABLED_ALPHA = 0.78` applied to `TEXT_DIM` produces #737983, which is
**3.50:1** and fails AA. The comment above it correctly reasons that halving alpha on a
dark theme is too aggressive, but 0.78 is still not enough. Dimmed text needs a solid
token, not an alpha.

---

## What is already right

Worth keeping, because a rewrite would lose it:

- The text ramp, and the recorded reasoning for why there are two tiers and not three.
- The per-instance accent (`accentOf`) rather than a global, and the reasoning at
  `Obsidian.h:20-27` about DAWs loading many instances into one process.
- `Obsidian.h` as a single owner of the palette, with `UIDefines.h` aliasing rather than
  redefining.
- The accent itself: #35c4d7 clears 4.5:1 as text on every ground in the product.

The problem is not the palette's taste. It is that the palette has **one usable step of
separation** (dark ground, light text) and the entire middle of the interface, where
controls live, is compressed into six values spanning #0e0f12 to #2a2e35.

---

## Priority

| | Finding | Standard | Effort |
|---|---|---|---|
| P0 | No focus indicator | 2.4.7, 2.4.11 | Small, in `Obsidian.h` |
| P0 | Transport unreachable by keyboard | 2.1.1 | Small, one key handler |
| P0 | Control vs ground at 1.00-1.33:1 | 1.4.11 | Medium, add a border token |
| P1 | No accessible names | 4.1.2 | Medium, mechanical |
| P1 | Hover/pressed below perception | 1.4.11 | Small |
| P1 | No primary/destructive hierarchy | — | Medium |
| P2 | Confidence bars use hue only | 1.4.1 | Small |
| P2 | `RECORD_RED` as text at 3.71:1 | 1.4.3 | Trivial |
| P2 | Disabled at 1.09:1 | — | Small |

The standards these are measured against are in [ACCESSIBILITY.md](ACCESSIBILITY.md).
The component and token rules are in [UI.md](UI.md).
