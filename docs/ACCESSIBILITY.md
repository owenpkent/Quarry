# Quarry — Accessibility Standards

The bar Quarry holds itself to, and how to check a change against it before it ships.

**Target: WCAG 2.2 Level AA**, as far as the criteria apply to a native desktop audio
application. Quarry is not a web page and some criteria do not map; where one does not,
this document says so rather than quietly dropping it.

Current conformance is recorded in [UI_AUDIT.md](UI_AUDIT.md). This document is the
standard, not the status.

---

## 1. Contrast

### 1.1 Text — SC 1.4.3

| Text | Minimum against its own background |
|---|---|
| Body, labels, values, button text | **4.5:1** |
| Large text (>=18.66px regular, or >=14px bold) | **3:1** |
| Micro-caps section labels (`micro()`, 10px semibold) | **4.5:1** — 10px is not large text |
| Disabled text | No minimum, but see 1.4 |

Measure against the colour actually behind the glyph. A label on a panel is measured
against the panel, not the window ground. Where a surface is a gradient (`raisedFill`),
measure against the **lighter end**, because that is the worst case for light text.

Do not use alpha to dim text. On a dark theme, an alpha-dimmed light foreground loses
contrast faster than it looks like it should: `TEXT_DIM` at `DISABLED_ALPHA` 0.78
computes to #737983, which is 3.50:1 and fails. Add a solid token instead.

### 1.2 Controls and boundaries — SC 1.4.11

This is the criterion Quarry has historically missed, so it gets the most space.

**Every interactive control must be distinguishable from its adjacent background by at
least 3:1**, using either its fill or a border. In a dark theme a 3:1 *fill* would mean
a light grey button, which is wrong for this product. So the rule for Quarry is:

> A control is identified by its **border**, not its fill. Fill stays dark.

`CONTROL_BORDER` #6e7381 is the dimmest cool grey clearing 3:1 against every surface a
control can sit on:

| Against | Ratio |
|---|---|
| `CONTROL_BG` #262a31 | 3.04:1 |
| `PANEL_BG` #22252b | 3.24:1 |
| `VOID_BG` #0e0f12 | 4.05:1 |
| `WELL_BG` #101216 | 3.96:1 |

The border must be at least 1px at 100% scaling and must not be drawn with alpha.

Also requiring 3:1 against what is adjacent to them:

- Text input outlines, and the boundary of a combo box.
- The track and thumb of a slider, and the arc of a knob against its seat.
- Checkbox and toggle boundaries in **both** states.
- Any icon that is the only carrier of meaning (the transport glyphs, the trash).

Not covered by this criterion, and free to stay quiet: panel backgrounds, decorative
gradients, the seat lines and catch-lights in `raisedFill`, waveform fills, and inactive
grid rules.

### 1.3 States — SC 1.4.11

A state change that is the only signal of what happened must itself reach **3:1 against
the state it replaced**.

| State | Rule |
|---|---|
| Hover | >=3:1 against rest. `brighter(0.12f)` gives 1.40:1 and is not enough. |
| Pressed | **Cannot be achieved by darkening.** The rest surface is near the floor. Signal it with geometry: pass `false` for the catch-light in `raisedFill` so the chip seats, and invert the seat line. Contrast is not the mechanism here. |
| Selected / toggled on | Accent fill or accent border. #35c4d7 is 6.88:1 on a control. |
| Focused | See section 3. |
| Disabled | See 1.4. |

Hover and focus are different states and must look different. A control that is both
hovered and focused must still show the focus indicator.

### 1.4 Disabled

Disabled controls are exempt from contrast minimums under WCAG, but Quarry holds a
product rule on top of that:

> A disabled control must still read as a control. Minimum **1.5:1** for its boundary
> against its background, and its label must stay above **2.5:1**.

`beginTransparencyLayer(0.45f)` produces a 1.09:1 boundary and a 1.99:1 label, and
fails this. Prefer a dedicated disabled border and text token over a transparency layer.

### 1.5 Never signal with hue alone — SC 1.4.1

Colour may reinforce meaning. It may not be the only thing carrying it.

Anywhere hue distinguishes one thing from another, a second channel must also change:
a shape, a hatch, a height, a marker, a label, or a position. The confidence-by-bar
strip is the live example: cyan versus amber is invisible to a red-green colour
deficient user, and needs a hatch or a height delta.

This applies to error states too. Red text is not sufficient to mark an error; the copy
itself must say what went wrong.

---

## 2. Keyboard — SC 2.1.1, 2.1.2, 2.4.3

**Every control that can be operated with a mouse must be operable from the keyboard.**

- Do not call `setWantsKeyboardFocus(false)` on an interactive control. If a parent needs
  a key for a shortcut, take it in the parent's `keyPressed` and return `true`; that does
  not require removing the children from the tab order.
- Tab order follows visual reading order: header transport, then the left column top to
  bottom, then the main region, then the footer. Use `setExplicitFocusOrder` where the
  child creation order does not match the layout.
- No keyboard trap. Escape leaves any modal, popup, or text field.
- Spacebar and Enter both activate a focused button. Arrow keys adjust a focused knob or
  slider; Shift-arrow makes a fine adjustment; Home and End go to the extremes.
- A shortcut that is a single unmodified character must not fire while a text field has
  focus.

Anything reachable by Tab must be visibly focused. See section 3.

---

## 3. Focus visible — SC 2.4.7, 2.4.11, 2.4.13

**Every focusable control draws a focus indicator. No exceptions.**

The Quarry focus indicator:

- The accent at full opacity, 2px, drawn just outside the control's boundary.
- At least **3:1 against both** the control and the surrounding background. The accent
  measures 6.88:1 on a control and 7.33:1 on a panel, so it clears with margin.
- Never the only difference between focused and unfocused, where that difference is a
  colour a user may not perceive: the indicator has thickness, so it also changes shape.
- Not clipped by a parent. Reserve 2px of padding around controls in layout, or draw the
  ring on a sibling overlay.

Focus must not be obscured by a popup, tooltip, or panel that opens over it (SC 2.4.11).

`Obsidian.h` sets `TextEditor::focusedOutlineColourId` already; the rest of the controls
need the same treatment in `drawButtonBackground`, `drawRotarySlider`, `drawComboBox`,
and `drawToggleButton`.

---

## 4. Screen readers — SC 4.1.2, 1.3.1

JUCE exposes UI Automation on Windows and NSAccessibility on macOS through
`AccessibilityHandler`. Most of it is automatic once controls are named.

**Every interactive control needs an accessible name.**

| Call | Use for |
|---|---|
| `setTitle()` | The name. Required on every icon-only button, since there is no text to fall back on. |
| `setDescription()` | What the control does, when the name alone is not enough. |
| `setHelpText()` | Longer guidance, equivalent to the tooltip text. |

A JUCE tooltip is **not** an accessible name. `setTooltip` and `setTitle` are separate,
and a control with only a tooltip announces as unlabelled.

Further rules:

- Name the control by what it does, not what it looks like: "Record", not "Circle icon".
- A toggle's name does not change with its state. State is announced separately; a button
  named "Mute" that becomes "Unmute" reads as a different control.
- Group related controls with `setTitle` on the containing component so the group is
  announced once, rather than each child repeating the section name.
- Purely decorative components set `setAccessible(false)` so they do not clutter the
  tree. Backgrounds, spacers, and the wordmark qualify.
- A value that changes without user action (a level meter, a transcription progress
  count) should not announce on every frame. Announce meaningful transitions only, via
  `AccessibilityHandler::notifyAccessibilityEvent`.

---

## 5. Motion and timing — SC 2.2.2, 2.3.1, 2.3.3

- Nothing flashes more than three times per second. The record indicator blinks well
  under that; keep it there.
- Meters and waveform animation are exempt as they are the product's function, but any
  purely decorative animation should be still by default.
- No time limit on any interaction.

---

## 6. Targets and layout — SC 2.5.8, 1.4.10, 1.4.4

- **Minimum pointer target 24x24px**, per SC 2.5.8 Level AA. Where a control is drawn
  smaller, extend its hit area to 24px rather than growing the graphic.
- Quarry has a resizable editor. Layout must survive the full supported range without
  clipping text or overlapping controls.
- Text must not be baked into images. Everything is drawn with `ui()`, `uiSemi()` or
  `micro()` so it scales with the editor.

---

## 7. Checking a change

Before merging anything that touches painting or layout:

1. **Run the contrast checker.** `python tools/contrast_check.py` validates every token
   pairing declared in `Lib/Components/UIDefines.h` against the rules above and exits
   non-zero on a regression. It runs in CI.
2. **Tab through the whole window.** Every control reachable, focus visible on each,
   Escape leaves every popup, no trap.
3. **Turn on a screen reader** (Narrator on Windows, VoiceOver on macOS) and tab the
   header row. Every button announces a name.
4. **Check the greyscale.** Screenshot the window and desaturate it. Anything you can no
   longer tell apart was relying on hue alone.

The first of those is automated. The other three are five minutes and catch what a
contrast checker structurally cannot.

---

## 8. What this standard does not cover

Stated so the gaps are deliberate rather than forgotten:

- **Windows High Contrast mode.** JUCE does not adopt system high-contrast palettes and
  Quarry does not currently respond to it. A user in that mode gets the normal skin.
- **Localisation and RTL.** Quarry is English-only today; layout is not mirrored.
- **`prefers-reduced-motion`.** No desktop equivalent is wired up.
- **A light theme.** Obsidian is dark by design. Every ratio in this document is computed
  for the dark palette only; a light theme would need the whole table recomputed.

If any of those become requirements, they need their own pass, not an extrapolation from
this one.
