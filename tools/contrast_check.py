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

# { "Amber", 0xffd7a635 }, - one row of Obsidian's accentChoices() table. An argb of 0
# means "use cyanAccent", which is the one accent not derived from a base.
_ACCENT_CHOICE = re.compile(
    r'\{\s*"(\w+)"\s*,\s*(?:0x[0-9a-fA-F]{2}([0-9a-fA-F]{6})|0)\s*\}'
)

# const Accent cyanAccent { juce::Colour(0xff35c4d7), juce::Colour(0xff8fe8f2), ... }
# Cyan ships its base/hot/deep exact rather than derived, so it is read, not computed.
_CYAN_ACCENT = re.compile(
    r"const\s+Accent\s+cyanAccent\s*\{\s*"
    r"juce::Colour\(0x[0-9a-fA-F]{2}([0-9a-fA-F]{6})\)\s*,\s*"
    r"juce::Colour\(0x[0-9a-fA-F]{2}([0-9a-fA-F]{6})\)",
    re.S,
)

# Accent { base, base.brighter(N), ... } - the factor Obsidian derives "hot" with.
_DERIVE_BRIGHTER = re.compile(r"return\s*\{\s*base\s*,\s*base\.brighter\(([0-9.]+)f\)")


def _brighter(rgb, amount):
    """juce::Colour::brighter, so a derived accent is the one the product paints."""
    factor = 1.0 / (1.0 + amount)
    return "".join(f"{min(255, max(0, int(255 - factor * (255 - int(rgb[i:i + 2], 16))))):02x}"
                   for i in (0, 2, 4))


def parse_accents(obsidian_text):
    """Every accent the user can pick, as {name: (base, hot)}.

    All eight, not just the default. The first version of this checked cyan alone under a
    comment calling it the dimmest of the eight, which had it backwards: cyan is the second
    brightest, and four of the other seven failed the 4.5:1 this file asks of accent text.
    A checker that tests the easiest case is a checker that passes.
    """
    cyan = _CYAN_ACCENT.search(obsidian_text)
    if not cyan:
        sys.exit("could not read cyanAccent out of Obsidian.h")

    amount = _DERIVE_BRIGHTER.search(obsidian_text)
    if not amount:
        sys.exit("could not read the derive() brighten factor out of Obsidian.h")
    amount = float(amount.group(1))

    accents = {}
    for match in _ACCENT_CHOICE.finditer(obsidian_text):
        name, argb = match.group(1), match.group(2)
        if argb is None:                       # the 0 sentinel: cyan, shipped exact
            accents[name] = (cyan.group(1).lower(), cyan.group(2).lower())
        else:
            accents[name] = (argb.lower(), _brighter(argb.lower(), amount))

    if not accents:
        sys.exit("could not read accentChoices() out of Obsidian.h")
    return accents


def parse_palette():
    """Pull every named colour out of the three headers into {name: 'rrggbb'}.

    Also returns the accent table, which is not a flat token: an accent is a base and a
    hot core, and which of the two a pairing wants is the difference between passing and
    failing. See parse_accents.
    """
    palette, collisions = {}, []

    for path, patterns in ((UIDEFINES, (_CAST_FORM, _DECIMAL_FORM)),
                           (QUARRY_LNF, (_QUALIFIED_CAST_FORM,)),
                           (OBSIDIAN, (_ARGB_FORM,))):
        if not path.exists():
            sys.exit(f"palette source missing: {path}")
        text = path.read_text(encoding="utf-8", errors="replace")

        for pattern in patterns:
            for match in pattern.finditer(text):
                name = match.group(1)
                if pattern is _ARGB_FORM:
                    value = match.group(2).lower()
                elif pattern is _DECIMAL_FORM:
                    channels = [int(match.group(i)) for i in (2, 3, 4)]
                    if any(c > 255 for c in channels):
                        continue
                    value = "".join(f"{c:02x}" for c in channels)
                else:
                    value = "".join(match.group(i).lower() for i in (2, 3, 4))

                # First header wins, which is the precedence the compiler gives them, but
                # say so. A token defined twice with two values means one of the two is
                # being checked and the other is being painted.
                if name in palette and palette[name] != value:
                    collisions.append((name, palette[name], value, path.name))
                palette.setdefault(name, value)

    accents = parse_accents(OBSIDIAN.read_text(encoding="utf-8", errors="replace"))
    return palette, accents, collisions


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

    # -- primary button: dark text on an accent fill, never white -----------
    ("VOID_BG", "@base", 4.5, "text"),

    # -- destructive role: border is a boundary, label is text --------------
    ("DESTRUCTIVE", "CONTROL_BG", 4.5, "text"),
    ("DESTRUCTIVE", "PANEL_BG", 4.5, "text"),
    ("DESTRUCTIVE", "VOID_BG", 4.5, "text"),

    # -- control boundaries, SC 1.4.11 --------------------------------------
    ("CONTROL_BORDER", "CONTROL_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_TOP", 3.0, "boundary"),
    ("CONTROL_BORDER", "PANEL_BOT", 3.0, "boundary"),
    ("CONTROL_BORDER", "VOID_BG", 3.0, "boundary"),
    ("CONTROL_BORDER", "WELL_BG", 3.0, "boundary"),

    # -- disabled: exempt from AA, but must still read as a control ---------
    # Thresholds are the Quarry product rule in docs/ACCESSIBILITY.md 1.4, not WCAG.
    ("DISABLED_BORDER", "CONTROL_BG", 1.5, "disabled boundary"),
    ("DISABLED_BORDER", "PANEL_BG", 1.5, "disabled boundary"),
    ("DISABLED_TEXT", "CONTROL_BG", 2.5, "disabled text"),
    ("DISABLED_TEXT", "PANEL_BG", 2.5, "disabled text"),

    # -- list rows ----------------------------------------------------------
    # The stripe is decorative, so it has no minimum of its own; what matters is that
    # text stays legible on it.
    ("TEXT_DIM", "ROW_ALT", 4.5, "text"),
    ("TEXT_MAIN", "ROW_ALT", 4.5, "text"),

    # -- piano roll keys must read as keys ----------------------------------
    ("KEY_WHITE", "KEY_BLACK", 3.0, "boundary"),
]

# Pairings run once per accent, with @base and @hot standing in for that accent's two
# values. Every one of the eight is checked, because the user picks which one they get.
#
# Which of the two a pairing takes is the rule this file exists to hold:
#
#   @base is a graphic. Lit fills, focus rings, selection bars, meters. It answers to
#   3:1 and clears it on every accent (the worst, Magenta, is 3.59 on a control).
#
#   @hot is text. Accent base as a label failed 4.5:1 on four of the eight accents
#   against a panel or a control, and did so in shipped code: the sample bar's success
#   status was painted in it. @hot is the same hue and clears 4.5:1 everywhere, worst
#   case 6.52. See docs/UI.md.
ACCENT_PAIRINGS = [
    # -- accent as a graphic, SC 1.4.11 -------------------------------------
    ("@base", "CONTROL_BG", 3.0, "focus ring"),
    ("@base", "PANEL_BG", 3.0, "focus ring"),
    ("@base", "PANEL_TOP", 3.0, "focus ring"),
    ("@base", "VOID_BG", 3.0, "focus ring"),
    ("@base", "ROW_ALT", 3.0, "selection bar"),
    ("@base", "PANEL_BOT", 3.0, "selection bar"),
    ("@base", "CONTROL_BG", 3.0, "selection bar"),

    # -- accent as text, SC 1.4.3 -------------------------------------------
    ("@hot", "PANEL_BG", 4.5, "accent text"),
    ("@hot", "PANEL_TOP", 4.5, "accent text"),
    ("@hot", "PANEL_BOT", 4.5, "accent text"),
    ("@hot", "CONTROL_BG", 4.5, "accent text"),
    ("@hot", "VOID_BG", 4.5, "accent text"),
    ("@hot", "WELL_BG", 4.5, "accent text"),
    ("@hot", "ROW_ALT", 4.5, "accent text"),
]

# Tokens that must never be used as text, with the value that replaces them, and the
# source globs searched for a violation. This used to be a list the script printed and
# nothing more, which read as a rule in a file where everything else is enforced.
BANNED_AS_TEXT = {
    "HAIRLINE": ("decorative rules only, never a control boundary or text", "CONTROL_BORDER"),
    "textFaint": ("2.42:1 on a panel", "TEXT_DIM, or give the label a darker plate"),
}

# g.setColour(X); ... g.drawText / drawFittedText - a banned token reaching the screen as
# text. Deliberately shallow: it catches the direct form, which is the one that gets
# written, and does not pretend to follow a colour through a variable.
_TEXT_PAINT = re.compile(
    r"setColour\s*\(\s*(?:[\w:]*::)?(\w+)\s*\)"
    r"(?P<between>(?:[^;]*;){0,3}?[^;]*?)"
    r"\b(?:drawText|drawFittedText|drawSingleLineText|drawMultiLineText)",
    re.S,
)

SOURCE_GLOBS = ("Quarry/Source/**/*.cpp", "Quarry/Source/**/*.h",
                "Lib/Components/**/*.h", "Lib/Components/**/*.cpp")


def find_banned_text_uses():
    """Every place a banned token is set and then drawn as text."""
    hits = []
    for glob in SOURCE_GLOBS:
        for path in sorted(ROOT.glob(glob)):
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in _TEXT_PAINT.finditer(text):
                token = match.group(1)
                if token in BANNED_AS_TEXT:
                    line = text.count("\n", 0, match.start()) + 1
                    hits.append((path.relative_to(ROOT).as_posix(), line, token))
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="print passing rows too")
    args = parser.parse_args()

    palette, accents, collisions = parse_palette()
    print(f"parsed {len(palette)} colours and {len(accents)} accents from UIDefines.h, "
          f"QuarryLookAndFeel.h and Obsidian.h\n")

    # Every pairing once, then every accent pairing once per accent. @base and @hot are
    # substituted per accent so a failure names the accent that fails.
    rows = [(fg, bg, need, kind) for fg, bg, need, kind in PAIRINGS]
    for name, (base, hot) in accents.items():
        for fg, bg, need, kind in ACCENT_PAIRINGS:
            rows.append((fg.replace("@base", f"{name}.base").replace("@hot", f"{name}.hot"),
                         bg, need, kind))
        palette[f"{name}.base"], palette[f"{name}.hot"] = base, hot
    rows = [(fg, bg.replace("@base", "Cyan.base"), need, kind) for fg, bg, need, kind in rows]

    failures, missing, checked = [], [], 0

    for fg, bg, need, kind in rows:
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
            print(f"  {mark}  {ratio:5.2f}:1  (needs {need:.1f})  {kind:13s} "
                  f"{fg} #{palette[fg]} on {bg} #{palette[bg]}")

    print(f"\nchecked {checked} pairings across {len(accents)} accents, {len(failures)} failing")

    banned = find_banned_text_uses()

    if collisions:
        # Two headers, one name, two values: one of them is being checked and the other
        # is being painted, and the checker cannot tell you which.
        print("\ndefined twice with different values:")
        for name, kept, dropped, where in collisions:
            print(f"  {name:16s} #{kept} kept, #{dropped} in {where} ignored")

    if missing:
        # A token the docs specify but the headers do not define is a failure, not a
        # skip. Skipping it would leave the checker green while the product ships
        # controls with no boundary at all, which is the exact bug it exists to catch.
        print("\nspecified in docs/UI.md but absent from the palette:")
        for name, kind in missing:
            print(f"  {name:16s} needed for: {kind}")

    print("\nnever use as text:")
    for name, (why, instead) in BANNED_AS_TEXT.items():
        present = f"#{palette[name]}" if name in palette else "(absent)"
        print(f"  {name:16s} {present:10s} {why}; use {instead}")

    if banned:
        print("\ndrawn as text anyway:")
        for path, line, token in banned:
            print(f"  {path}:{line}  {token} -> use {BANNED_AS_TEXT[token][1]}")

    if failures:
        print("\nfailures:")
        for fg, bg, ratio, need, kind in failures:
            short = need - ratio
            print(f"  {fg} on {bg}: {ratio:.2f}:1, short of {need:.1f} by {short:.2f} ({kind})")

    if failures or missing or banned or collisions:
        print("\nrules: docs/ACCESSIBILITY.md   tokens: docs/UI.md")

    return len(failures) + len(missing) + len(banned) + len(collisions)


if __name__ == "__main__":
    # Capped: an exit status is a byte, and enough failures to wrap it round to zero
    # would otherwise report as success.
    sys.exit(min(main(), 125))
