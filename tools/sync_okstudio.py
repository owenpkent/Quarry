#!/usr/bin/env python3
"""Re-sync the vendored okstudio kit headers from a local checkout of the kit.

Quarry vendors a small number of headers from the okstudio JUCE kit so that a fresh
clone builds with nothing but git, cmake and a compiler. The kit repo is private, so
it cannot be a submodule. The cost of vendoring is drift: the copy in ThirdParty can
fall behind the kit without anyone noticing. This script is the cure.

Usage:
    py tools/sync_okstudio.py              copy any changed headers and re-pin
    py tools/sync_okstudio.py --check      report drift and exit 1, copy nothing
    py tools/sync_okstudio.py --kit PATH   use a kit checkout somewhere else

The kit is located, in order of precedence: --kit, $OKSTUDIO_KIT_DIR, then the
sibling checkout ../okstudio-juce-kit next to this repo.

Exit codes: 0 in sync (or synced), 1 drift found under --check, 2 the kit could not
be found or read.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import date

# Headers Quarry vendors from the kit. Paths are relative to <kit>/include and to
# <repo>/ThirdParty/okstudio/include, which are kept identical so that
# `#include <okstudio/WasapiLoopback.h>` resolves the same either way.
VENDORED = [
    "okstudio/WasapiLoopback.h",
    "okstudio/WasapiProcessLoopback.h",
    "okstudio/CaptureMath.h",
    "okstudio/Obsidian.h",
    "okstudio/MouseOnly.h",
    "okstudio/Icons.h",
]

KIT_REPO = "https://github.com/owenpkent/okstudio-juce-kit.git"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR_INCLUDE = os.path.join(REPO_ROOT, "ThirdParty", "okstudio", "include")
PIN_FILE = os.path.join(REPO_ROOT, "ThirdParty", "okstudio", "UPSTREAM.txt")


def copy_as_lf(src, dst):
    """Copy one header, normalising CRLF to LF.

    The kit stores text LF in its index and its .gitattributes lets a Windows checkout have
    CRLF on disk, so a straight copy from a developer's kit brings CRLF into ThirdParty, where
    every header is stored LF. Git then reports the whole file as changed. That is not cosmetic:
    the last sync turned a 73-line change to WasapiProcessLoopback.h into a 1479-line diff with
    the real edit somewhere inside it, and buried a second header whose content had not moved at
    all. The kit's own CLAUDE.md records the same trap being sprung there.

    Bytes, not text mode: these are C++ headers whose encoding is nobody's business here, and a
    decode/encode round trip would be a second way to change a file nobody asked to change.
    """
    with open(src, "rb") as f:
        data = f.read()

    with open(dst, "wb") as f:
        f.write(data.replace(b"\r\n", b"\n"))

    shutil.copystat(src, dst)


def sha256(path):
    """Hash one header for drift detection, with line endings normalised out of it.

    Same reason copy_as_lf exists, and it has to agree with it: the kit's Windows checkout can
    hold CRLF while ThirdParty holds LF, and hashing the raw bytes then calls an identical
    header drifted on every single run. That is worse than noise -- it trains a reader to skim
    past the drift report, which is the one place a genuine kit change announces itself.
    """
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def find_kit(explicit):
    for candidate in (explicit, os.environ.get("OKSTUDIO_KIT_DIR"),
                      os.path.join(os.path.dirname(REPO_ROOT), "okstudio-juce-kit")):
        if candidate and os.path.isdir(os.path.join(candidate, "include", "okstudio")):
            return os.path.abspath(candidate)
    return None


def git(kit, *args):
    try:
        out = subprocess.run(["git", "-C", kit] + list(args), capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def read_pin():
    pin = {}
    if os.path.exists(PIN_FILE):
        with open(PIN_FILE, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, _, value = line.partition("=")
                    pin[key.strip()] = value.strip()
    return pin


def write_pin(commit, dirty):
    os.makedirs(os.path.dirname(PIN_FILE), exist_ok=True)
    lines = [
        "# Upstream pin for the vendored okstudio kit headers.",
        "# Written by tools/sync_okstudio.py. Do not edit by hand.",
        "repo=" + KIT_REPO,
        "commit=" + (commit or "unknown"),
        "synced=" + date.today().isoformat(),
    ]
    if dirty:
        lines.append("# WARNING: the kit checkout had uncommitted changes when this was")
        lines.append("# written, so the commit above does not fully describe these files.")
    with open(PIN_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description="Re-sync vendored okstudio kit headers.")
    parser.add_argument("--kit", help="path to an okstudio-juce-kit checkout")
    parser.add_argument("--check", action="store_true",
                        help="report drift and exit 1 without copying anything")
    args = parser.parse_args()

    kit = find_kit(args.kit)
    if not kit:
        print("Could not find an okstudio-juce-kit checkout.", file=sys.stderr)
        print("Looked at --kit, $OKSTUDIO_KIT_DIR, and " +
              os.path.join(os.path.dirname(REPO_ROOT), "okstudio-juce-kit"), file=sys.stderr)
        print("Clone it from " + KIT_REPO + " (private) or pass --kit.", file=sys.stderr)
        return 2

    print("kit: " + kit)

    drifted, missing = [], []
    for rel in VENDORED:
        src = os.path.join(kit, "include", *rel.split("/"))
        dst = os.path.join(VENDOR_INCLUDE, *rel.split("/"))
        if not os.path.exists(src):
            missing.append(rel)
        elif not os.path.exists(dst) or sha256(src) != sha256(dst):
            drifted.append((rel, src, dst))

    if missing:
        for rel in missing:
            print("MISSING in kit: " + rel, file=sys.stderr)
        print("The kit no longer provides every vendored header. Update VENDORED in this "
              "script, and check whether Quarry still compiles.", file=sys.stderr)
        return 2

    head = git(kit, "rev-parse", "HEAD")

    if not drifted:
        pin = read_pin().get("commit")
        # The headers match but the pin can still be absent (first run) or stale (the
        # kit moved on without touching these files). Record it so --check and the
        # CMake drift warning have something truthful to report.
        if not args.check and pin != head:
            write_pin(head, bool(git(kit, "status", "--porcelain")))
            pin = head
        print("up to date (" + str(len(VENDORED)) + " headers, pinned at " +
              (pin or "unknown")[:12] + ")")
        return 0

    for rel, _, _ in drifted:
        print("DRIFT: " + rel)

    head = git(kit, "rev-parse", "HEAD")
    pinned = read_pin().get("commit")
    if head and pinned and pinned != "unknown" and pinned != head:
        log = git(kit, "log", "--oneline", pinned + ".." + head, "--",
                  *["include/" + rel for rel in VENDORED])
        if log:
            print("\nkit commits touching these headers since " + pinned[:12] + ":")
            for line in log.splitlines():
                print("  " + line)

    if args.check:
        print("\nOut of sync. Run: py tools/sync_okstudio.py", file=sys.stderr)
        return 1

    print()
    for rel, src, dst in drifted:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        copy_as_lf(src, dst)
        print("copied " + rel + " (" + str(os.path.getsize(dst)) + " bytes)")

    dirty = bool(git(kit, "status", "--porcelain"))
    if dirty:
        print("\nWARNING: the kit checkout has uncommitted changes, so the pinned commit "
              "does not fully describe what was just copied.")
    write_pin(head, dirty)

    print("\npinned at " + (head or "unknown")[:12] + ". Review and commit:")
    print("  git diff ThirdParty/okstudio")
    return 0


if __name__ == "__main__":
    sys.exit(main())
