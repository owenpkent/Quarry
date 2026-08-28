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

SC 1.4.11 covers visual information required to identify a component **and its states**.
The distinction that matters in practice:

- A state that carries **information** - checked, selected, armed, disabled - must reach
  **3:1 against the state it replaced**, because a user who cannot see the difference
  cannot read the value.
- A state that is only **feedback** - hover, pressed - is not held to 3:1. Nothing about
  the control's meaning is lost if it is missed, and forcing 3:1 on hover in a dark theme
  means a near-white button, which trades a real design for no conformance gain. It still
  has to be plainly visible.

| State | Rule |
|---|---|
| Hover | Feedback, not information: no 3:1 requirement, but it must be obvious. Lift the fill and brighten the border. `brighter(0.12f)` alone gives 1.40:1 and is too subtle. |
| Pressed | **Cannot be achieved by darkening.** The rest surface is near the floor. Signal it with geometry: pass `false` for the catch-light in `raisedFill` so the chip seats, and invert the seat line. Contrast is not the mechanism here. |
| Selected / toggled on | Information: **must** clear 3:1 against the off state. Accent fill or accent border, checked on all eight accents; the worst, magenta, is 3.59:1 on a control. |
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

## 2. Pointer and keyboard — SC 2.1.1, 2.1.2, 2.4.3, 2.5.8

### 2.1 The mouse-only contract comes first

`ThirdParty/okstudio/include/okstudio/MouseOnly.h` is the line-wide contract, and it
outranks the rest of this section:

> Every interaction must work with a single left-click, a drag, or a scroll. No keyboard
> requirement, no double-click, no modifier keys, no fine-precision gestures.

So, as a hard rule and one that WCAG agrees with:

- **Everything must be fully operable by pointer alone.** No action may require the
  keyboard, a modifier, a double-click, or a precision gesture. This is stricter than
  WCAG, and it is the product.
- **Minimum hit target 34px** per `okstudio::ui::minHitPx`, not the 24px of SC 2.5.8.
  The kit's number is more generous; use the kit's. Extend the hit area rather than
  growing the graphic.
- Right-click may exist only as an optional accelerator, never as the only route.

### 2.2 Keyboard, where it applies

WCAG SC 2.1.1 wants the converse too: anything operable by mouse should also be operable
by keyboard. Quarry does not currently meet that, and the reason is real rather than an
oversight: a plugin that takes keyboard focus takes the spacebar away from the host and
stops the DAW transport.

The two builds therefore have different obligations:

- **Plugin.** Deferring the keyboard to the host is the correct behaviour. Use
  `okstudio::ui::makeMouseOnly()`. Do not take focus.
- **Standalone.** There is no host to defer to, so nothing else will handle the keyboard
  and SC 2.1.1 applies in full. This is an open item, tracked in
  [UI_AUDIT.md](UI_AUDIT.md) section 5, and is a product decision rather than a patch.

Where keyboard operation does apply:

- Tab order follows visual reading order: header transport, then the left column top to
  bottom, then the main region, then the footer. Use `setExplicitFocusOrder` where the
  child creation order does not match the layout.
- No keyboard trap. Escape leaves any modal, popup, or text field.
- Spacebar and Enter both activate a focused button. Arrow keys adjust a focused knob or
  slider; Shift-arrow makes a fine adjustment; Home and End go to the extremes.
- A shortcut that is a single unmodified character must not fire while a text field has
  focus.

Anything that can be focused at all must be visibly focused. See section 3. That holds
regardless of how section 2.2 is resolved, because a ring costs nothing on a control that
is never focused.

---

## 3. Focus visible — SC 2.4.7, 2.4.11, 2.4.13

**Every focusable control draws a focus indicator. No exceptions.**

The Quarry focus indicator:

- The accent at full opacity, 2px, drawn just **inside** the control's boundary.
- At least **3:1 against both** the control and the surrounding background. Checked across
  all eight accents; the worst, magenta, is 3.59:1 on a control.
- Never the only difference between focused and unfocused, where that difference is a
  colour a user may not perceive: the indicator has thickness, so it also changes shape.

**Inside, not outside, and this is the trap.** JUCE clips a component's painting to its own
bounds unless it calls `setPaintingIsUnclipped` (`juce_Component.cpp`,
`paintComponentAndChildren`). The first version of Quarry's ring expanded outwards from the
control's paint rect, which put a 2px stroke one to three pixels beyond the component, and
every pixel of it was clipped. Every control was focusable, every one drew a ring, and not
one of them was visible. Nothing about that shows up in a contrast table, which is worth
remembering about contrast tables: the ring passed 3:1 the whole time it was invisible.

If a ring must sit outside a control, the control's own paint rect has to be inset to make
room for it. Do not rely on a parent leaving space.

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

- **Minimum pointer target 34x34px**, per `okstudio::ui::minHitPx`. That exceeds the
  24px SC 2.5.8 asks for; the kit's number wins. See section 2.1.
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
