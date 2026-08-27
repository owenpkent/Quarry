#!/usr/bin/env python3
"""Check Quarry's palette against the contrast rules in docs/ACCESSIBILITY.md.

The palette is not hardcoded here. It is parsed out of the three files that own it,
so the checker cannot drift from the product:

    Lib/Components/UIDefines.h            Quarry's surface and text tokens
    Lib/Components/QuarryLookAndFeel.h    the role tokens (destructive, disabled)
    ThirdParty/okstudio/.../Obsidian.h    the shared Obsidian palette

Every pairing below is one the product actually paints. Adding a token to one of
those headers does not create a check; a pairing has to be declared in PAIRINGS,
because only a person knows what ends up next to what.

Exit code is the number of failures, so CI fails on a regression. --verbose prints
the passing rows too, which is what you want when picking a new value.

Usage:
    python tools/contrast_check.py [--verbose]
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UIDEFINES = ROOT / "Lib" / "Components" / "UIDefines.h"
QUARRY_LNF = ROOT / "Lib" / "Components" / "QuarryLookAndFeel.h"
OBSIDIAN = ROOT / "ThirdParty" / "okstudio" / "include" / "okstudio" / "Obsidian.h"


# ---------------------------------------------------------------- colour maths

def _linear(channel):
    """sRGB channel (0..1) to linear light, per WCAG 2.2 relative luminance."""
    return channel / 12.92 if channel <= 0.03928 else ((channel + 0.055) / 1.055) ** 2.4


def luminance(hex_rgb):
    r, g, b = (int(hex_rgb[i:i + 2], 16) / 255 for i in (0, 2, 4))
    return 0.2126 * _linear(r) + 0.7152 * _linear(g) + 0.0722 * _linear(b)


def contrast(fg, bg):
    a, b = luminance(fg), luminance(bg)
    lighter, darker = max(a, b), min(a, b)
    return (lighter + 0.05) / (darker + 0.05)


# ---------------------------------------------------------------- palette parse

# static const Colour NAME(static_cast<uint8>(0xNN), ...) - the UIDefines form.
_CAST_FORM = re.compile(
    r"static\s+const\s+Colour\s+(\w+)\s*\(\s*"
    r"static_cast<uint8>\(0x([0-9a-fA-F]{2})\)\s*,\s*"
    r"static_cast<uint8>\(0x([0-9a-fA-F]{2})\)\s*,\s*"
    r"static_cast<uint8>\(0x([0-9a-fA-F]{2})\)\s*\)"
)

# static const juce::Colour NAME(static_cast<juce::uint8>(0xNN), ...) - the form used
# inside a namespace, where there is no JuceHeader.h "using namespace juce" in scope.
_QUALIFIED_CAST_FORM = re.compile(
    r"static\s+const\s+juce::Colour\s+(\w+)\s*\(\s*"
    r"static_cast<juce::uint8>\(0x([0-9a-fA-F]{2})\)\s*,\s*"
    r"static_cast<juce::uint8>\(0x([0-9a-fA-F]{2})\)\s*,\s*"
    r"static_cast<juce::uint8>\(0x([0-9a-fA-F]{2})\)\s*\)",
    re.S,
)

# static const Colour NAME(216, 74, 96) - the decimal form.
_DECIMAL_FORM = re.compile(
    r"static\s+const\s+Colour\s+(\w+)\s*\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*\)"
)

# const juce::Colour name { 0xffrrggbb }; - the Obsidian form.
_ARGB_FORM = re.compile(
    r"const\s+juce::Colour\s+(\w+)\s*\{\s*0x[0-9a-fA-F]{2}([0-9a-fA-F]{6})\s*\}"
)


def parse_palette():
    """Pull every named colour out of both headers into {name: 'rrggbb'}."""
    palette = {}

    for path, patterns in ((UIDEFINES, (_CAST_FORM, _DECIMAL_FORM)),
                           (QUARRY_LNF, (_QUALIFIED_CAST_FORM,)),
                           (OBSIDIAN, (_ARGB_FORM,))):
        if not path.exists():
            sys.exit(f"palette source missing: {path}")
        text = path.read_text(encoding="utf-8", errors="replace")

        for pattern in patterns:
            for match in pattern.finditer(text):
                name = match.group(1)
                if pattern is _QUALIFIED_CAST_FORM:
                    palette.setdefault(name, "".join(match.group(i).lower() for i in (2, 3, 4)))
                elif pattern is _ARGB_FORM:
                    palette.setdefault(name, match.group(2).lower())
                elif pattern is _DECIMAL_FORM:
                    channels = [int(match.group(i)) for i in (2, 3, 4)]
                    if any(c > 255 for c in channels):
                        continue
                    palette.setdefault(name, "".join(f"{c:02x}" for c in channels))
                else:
                    palette.setdefault(name, "".join(match.group(i).lower() for i in (2, 3, 4)))

    # The accent is per instance and never a constant, so it is not in either header as a
    # token. Cyan is the default and the dimmest of the eight shipped accents against a
    # dark ground, which makes it the worst case worth checking.
    palette.setdefault("cyanAccent", "35c4d7")
    return palette


# ---------------------------------------------------------------- the rules
#
# (foreground, background, minimum, what it is)
#
# Minimums come from docs/ACCESSIBILITY.md:
#   4.5  body text                        SC 1.4.3
#   3.0  large text, control boundaries   SC 1.4.3 / 1.4.11
#   1.5  disabled control boundary        Quarry product rule

TEXT_ON_SURFACES = [
    ("TEXT_MAIN", surface, 4.5) for surface in
    ("VOID_BG", "WELL_BG", "PANEL_BOT", "PANEL_BG", "PANEL_TOP", "CONTROL_BG")
] + [
    ("TEXT_DIM", surface, 4.5) for surface in
    ("VOID_BG", "WELL_BG", "PANEL_BOT", "PANEL_BG", "PANEL_TOP", "CONTROL_BG")
]

PAIRINGS = [
    # -- text, SC 1.4.3 ------------------------------------------------------
    *[(fg, bg, need, "text") for fg, bg, need in TEXT_ON_SURFACES],
    ("cyanAccent", "PANEL_BG", 4.5, "text"),
    ("cyanAccent", "CONTROL_BG", 4.5, "text"),
    ("cyanAccent", "VOID_BG", 4.5, "text"),

    # -- control boundaries, SC 1.4.11 --------------------------------------
    ("CONTROL_BORDER", "CONTROL_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_TOP", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_BOT", 3.0, "boundary"),
    ("CONTROL_BORDER", "VOID_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "WELL_BG", 3.0, "boundary"),

    # -- focus ring, SC 2.4.11 ----------------------------------------------
    ("cyanAccent", "CONTROL_BG", 3.0, "focus ring"),
    ("cyanAccent", "PANEL_BG", 3.0, "focus ring"),
    ("cyanAccent", "PANEL_TOP", 3.0, "focus ring"),
    ("cyanAccent", "VOID_BG", 3.0, "focus ring"),

    # -- primary button: dark text on an accent fill, never white -----------
    ("VOID_BG", "cyanAccent", 4.5, "text"),

    # -- destructive role: border is a boundary, label is text --------------
    ("DESTRUCTIVE", "CONTROL_BG", 4.5, "text"),
    ("DESTRUCTIVE", "PANEL_BG", 4.5, "text"),
    ("DESTRUCTIVE", "VOID_BG", 4.5, "text"),

    # -- disabled: exempt from AA, but must still read as a control ---------
    # Thresholds are the Quarry product rule in docs/ACCESSIBILITY.md 1.4, not WCAG.
    ("DISABLED_BORDER", "CONTROL_BG", 1.5, "disabled boundary"),
    ("DISABLED_BORDER", "PANEL_BG", 1.5, "disabled boundary"),
    ("DISABLED_TEXT", "CONTROL_BG", 2.5, "disabled text"),
    ("DISABLED_TEXT", "PANEL_BG", 2.5, "disabled text"),

    # -- list rows ----------------------------------------------------------
    # The stripe is decorative, so it has no minimum of its own; what matters is that
    # text stays legible on it, and that the accent bar carrying selection stands out
    # against every row colour it can land on.
    ("TEXT_DIM", "ROW_ALT", 4.5, "text"),
    ("TEXT_MAIN", "ROW_ALT", 4.5, "text"),
    ("cyanAccent", "ROW_ALT", 3.0, "selection bar"),
    ("cyanAccent", "PANEL_BOT", 3.0, "selection bar"),
    ("cyanAccent", "CONTROL_BG", 3.0, "selection bar"),

    # -- piano roll keys must read as keys ----------------------------------
    ("KEY_WHITE", "KEY_BLACK", 3.0, "boundary"),
]

# Tokens that must never be used as text, with the value that replaces them.
# HAIRLINE is decorative only; textFaint fails on every Quarry surface.
BANNED_AS_TEXT = {
    "HAIRLINE": "decorative rules only, never a control boundary or text",
    "textFaint": "2.42:1 on a panel; use TEXT_DIM, or give the label a darker plate",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="print passing rows too")
    args = parser.parse_args()

    palette = parse_palette()
    print(f"parsed {len(palette)} colours from UIDefines.h, "
          f"QuarryLookAndFeel.h and Obsidian.h\n")

    failures, missing, checked = [], [], 0

    for fg, bg, need, kind in PAIRINGS:
        if fg not in palette or bg not in palette:
            absent = fg if fg not in palette else bg
            if absent not in [m[0] for m in missing]:
                missing.append((absent, kind))
            continue

        checked += 1
        ratio = contrast(palette[fg], palette[bg])
        ok = ratio >= need

        if not ok:
            failures.append((fg, bg, ratio, need, kind))
        if args.verbose or not ok:
            mark = "pass" if ok else "FAIL"
            print(f"  {mark}  {ratio:5.2f}:1  (needs {need:.1f})  {kind:11s} "
                  f"{fg} #{palette[fg]} on {bg} #{palette[bg]}")

    print(f"\nchecked {checked} pairings, {len(failures)} failing")

    if missing:
        # A token the docs specify but the headers do not define is a failure, not a
        # skip. Skipping it would leave the checker green while the product ships
        # controls with no boundary at all, which is the exact bug it exists to catch.
        print("\nspecified in docs/UI.md but absent from the palette:")
        for name, kind in missing:
            print(f"  {name:16s} needed for: {kind}")

    if BANNED_AS_TEXT:
        print("\nnever use as text:")
        for name, why in BANNED_AS_TEXT.items():
            present = f"#{palette[name]}" if name in palette else "(absent)"
            print(f"  {name:16s} {present:10s} {why}")

    if failures:
        print("\nfailures:")
        for fg, bg, ratio, need, kind in failures:
            short = need - ratio
            print(f"  {fg} on {bg}: {ratio:.2f}:1, short of {need:.1f} by {short:.2f} ({kind})")
        print("\nrules: docs/ACCESSIBILITY.md   tokens: docs/UI.md")

    return len(failures) + len(missing)


if __name__ == "__main__":
    sys.exit(main())
