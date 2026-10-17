#!/usr/bin/env python3
"""Build, check and publish a Windows release of Quarry.

Manual and local, like the rest of the build: nothing here runs in CI. The version has one
source, project(QuarryPlugin VERSION x.y.z) in CMakeLists.txt; the installer, the file names, the
tag and the notes all follow from it.

    py tools/release/release.py                      # build + test + installer + artifacts
    py tools/release/release.py build --skip-build   # reuse build-release/, redo installer + artifacts
    py tools/release/release.py build --verify-only  # only run the gates on what is already built
    py tools/release/release.py publish              # print the gh command
    py tools/release/release.py publish --yes        # run it

Artifacts land in release/ (git-ignored):

    QuarrySetup-X.Y.Z.exe
    QuarrySetup-X.Y.Z.exe.sha256          sha256sum format, so `sha256sum -c` reads it
    Quarry-X.Y.Z-sbom.cyclonedx.json      the git submodules plus the prebuilt onnxruntime
    release-notes-X.Y.Z.md                the CHANGELOG section, for the GitHub release body

There is no lockfile: the installer ships Quarry.exe and the VST3 and nothing written in Python
(the sidecar under tools/sidecar is a developer tool and is not in Installers/Windows/quarry.iss).
The names and the flags the installer takes are a contract with the in-app updater
(docs/AUTO_UPDATE.md); do not rename either without changing both.

Windows is not code signed yet, so the .sha256 file is the integrity story and SmartScreen will warn.

Windows only. Standard library only, so it runs on a bare Python.
"""

import argparse
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from ctypes import wintypes

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BUILD_DIR = os.path.join(ROOT, "build-release")
RELEASE_DIR = os.path.join(ROOT, "release")
ARTEFACTS = os.path.join(BUILD_DIR, "Quarry_artefacts", "Release")
STANDALONE_EXE = os.path.join(ARTEFACTS, "Standalone", "Quarry.exe")
VST3_BINARY = os.path.join(ARTEFACTS, "VST3", "Quarry.vst3", "Contents", "x86_64-win", "Quarry.vst3")
UNIT_TESTS_EXE = os.path.join(BUILD_DIR, "Tests", "UnitTests_artefacts", "Release", "UnitTests.exe")
ISS = os.path.join(ROOT, "Installers", "Windows", "quarry.iss")
REPO = "owenpkent/Quarry"
ISCC_DEFAULTS = (
    r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    r"C:\Program Files\Inno Setup 6\ISCC.exe",
)

# --------------------------------------------------------------------------------------
# Console
# --------------------------------------------------------------------------------------

def _enable_ansi() -> bool:
    if not sys.stdout.isatty():
        return False
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = wintypes.DWORD()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except (AttributeError, OSError):
        return False


_ANSI = _enable_ansi()
GREEN = "\033[32m" if _ANSI else ""
YELLOW = "\033[33m" if _ANSI else ""
GREY = "\033[90m" if _ANSI else ""
RESET = "\033[0m" if _ANSI else ""


class ReleaseError(Exception):
    """A gate failed or a step could not run. The message says what to do about it."""


# --------------------------------------------------------------------------------------
# Pure logic (tested in test_release.py)
# --------------------------------------------------------------------------------------

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_cmake_version(cmake_text: str) -> str:
    """The X.Y.Z of project(QuarryPlugin VERSION X.Y.Z ...), the one source of truth."""
    match = re.search(r"^\s*project\s*\(\s*\w+\s+VERSION\s+([0-9][0-9.]*)", cmake_text,
                      re.IGNORECASE | re.MULTILINE)
    if not match:
        raise ReleaseError("Could not find project(... VERSION x.y.z) in CMakeLists.txt.")
    version = match.group(1)
    if not VERSION_RE.match(version):
        raise ReleaseError(f"The CMake version {version!r} is not X.Y.Z; the updater compares "
                           "three numeric parts.")
    return version


def changelog_section(changelog_text: str, version: str):
    """The body of the `## [X.Y.Z]` section (Keep a Changelog), or None if there is none.

    The heading may carry a date (`## [1.2.0] - 2026-10-16`). The body runs to the next `## `
    heading or the link reference block at the end, with surrounding blank lines trimmed.
    """
    text = changelog_text.replace("\r\n", "\n")
    heading = re.compile(r"^## \[" + re.escape(version) + r"\](?:[ \t]+-.*)?[ \t]*$", re.MULTILINE)
    start = heading.search(text)
    if not start:
        return None
    rest = text[start.end():]
    stop = re.search(r"^## |^\[[^\]]+\]:[ \t]", rest, re.MULTILINE)
    body = rest[:stop.start()] if stop else rest
    body = body.strip("\n")
    return body if body.strip() else None


def installer_name(version: str) -> str:
    return f"QuarrySetup-{version}.exe"


def sbom_name(version: str) -> str:
    return f"Quarry-{version}-sbom.cyclonedx.json"


def notes_name(version: str) -> str:
    return f"release-notes-{version}.md"


def sha256_line(digest: str, filename: str) -> str:
    """One sha256sum line: 64 lowercase hex, two spaces, the name, LF."""
    digest = digest.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError(f"not a SHA-256 digest: {digest!r}")
    return f"{digest}  {filename}\n"


def version_tuple_string(parts: tuple) -> str:
    """(1, 1, 0, 0) -> '1.1.0.0'."""
    return ".".join(str(part) for part in parts)


def github_purl(url: str, commit: str):
    """pkg:github/<owner>/<repo>@<commit> for a github.com remote, else None."""
    match = re.match(r"^(?:https://github\.com/|git@github\.com:)([^/]+)/([^/]+?)(?:\.git)?/?$", url.strip())
    if not match:
        return None
    return f"pkg:github/{match.group(1).lower()}/{match.group(2).lower()}@{commit}"


def parse_submodule_status(status_text: str) -> list:
    """`git submodule status` lines -> [(path, commit, described)]. A leading + or - marks a
    checkout that differs from, or is missing against, the recorded commit; it is dropped here."""
    modules = []
    for line in status_text.splitlines():
        match = re.match(r"^[ +\-U]?([0-9a-f]{40})\s+(\S+)(?:\s+\((.*)\))?\s*$", line)
        if match:
            modules.append((match.group(2), match.group(1), match.group(3) or ""))
    return modules


def build_sbom(version: str, submodules: list, onnxruntime: dict, timestamp: str,
               serial: str) -> dict:
    """A CycloneDX 1.6 document.

    submodules: [{"name", "path", "commit", "url", "describe"}]
    onnxruntime: {"version", "url", "sha256"}
    """
    components = []
    for module in sorted(submodules, key=lambda m: m["path"]):
        component = {
            "type": "library",
            "bom-ref": f"submodule:{module['path']}",
            "name": module["name"],
            "version": module["commit"],
        }
        purl = github_purl(module.get("url", ""), module["commit"])
        if purl:
            component["purl"] = purl
        if module.get("url"):
            component["externalReferences"] = [{"type": "vcs", "url": module["url"]}]
        if module.get("describe"):
            component["properties"] = [{"name": "quarry:git-describe", "value": module["describe"]}]
        components.append(component)

    components.append({
        "type": "library",
        "bom-ref": "prebuilt:onnxruntime",
        "name": "onnxruntime",
        "version": onnxruntime["version"],
        "hashes": [{"alg": "SHA-256", "content": onnxruntime["sha256"]}],
        "externalReferences": [{"type": "distribution", "url": onnxruntime["url"]}],
        "description": "Prebuilt static library and feature model, fetched by run.py and "
                       "verified against the pinned SHA-256.",
    })

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "timestamp": timestamp,
            "component": {
                "type": "application",
                "bom-ref": "quarry",
                "name": "Quarry",
                "version": version,
                "supplier": {"name": "OK Studio"},
                "purl": f"pkg:github/{REPO.lower()}@v{version}",
            },
        },
        "components": components,
    }


def gh_create_command(version: str, files: list) -> list:
    """The exact `gh release create` argv. Never draft, never prerelease."""
    tag = f"v{version}"
    return (["gh", "release", "create", tag] + list(files) +
            ["--repo", REPO, "--title", tag, "--notes-file",
             os.path.join(RELEASE_DIR, notes_name(version)), "--verify-tag"])


# --------------------------------------------------------------------------------------
# Environment
# --------------------------------------------------------------------------------------

def run_cmd(args):
    print(f"{GREY}> {' '.join(args)}{RESET}")
    done = subprocess.run(args, cwd=ROOT)
    if done.returncode != 0:
        raise ReleaseError(f"{os.path.basename(args[0])} failed with exit code {done.returncode}.")


def git(*args, check=True) -> str:
    done = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if check and done.returncode != 0:
        raise ReleaseError(f"git {' '.join(args)} failed: {done.stderr.strip()}")
    return done.stdout


def read_version() -> str:
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as handle:
        return parse_cmake_version(handle.read())


def find_iscc():
    override = os.environ.get("QUARRY_ISCC", "").strip()
    candidates = ([override] if override else []) + [shutil.which("ISCC") or ""] + list(ISCC_DEFAULTS)
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def file_sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_file_version(path: str) -> str:
    """The numeric FileVersion of an exe or dll, as 'a.b.c.d', from its version resource."""
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                        ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL

    size = api.GetFileVersionInfoSizeW(path, None)
    if not size:
        raise ReleaseError(f"{path} has no version resource.")
    buffer = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(path, 0, size, buffer):
        raise ReleaseError(f"Could not read the version resource of {path}.")
    pointer = ctypes.c_void_p()
    length = wintypes.UINT()
    if (not api.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length))
            or pointer.value is None):
        raise ReleaseError(f"{path} has no fixed file version.")
    # VS_FIXEDFILEINFO: signature, struct version, then FileVersionMS and FileVersionLS.
    fields = (wintypes.DWORD * 4).from_address(pointer.value)
    ms, ls = fields[2], fields[3]
    return version_tuple_string((ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF))


# --------------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------------

def tag_exists_locally(tag: str) -> bool:
    return bool(git("tag", "--list", tag).strip())


def tag_on_origin(tag: str):
    """The commit the tag points to on origin, '' if there is no such tag, None if offline."""
    done = subprocess.run(["git", "ls-remote", "--tags", "origin", f"refs/tags/{tag}",
                           f"refs/tags/{tag}^{{}}"], cwd=ROOT, capture_output=True, text=True)
    if done.returncode != 0:
        return None
    commit = ""
    for line in done.stdout.splitlines():
        sha, ref = line.split("\t", 1)
        if ref.endswith("^{}"):
            return sha
        commit = sha
    return commit


def check_tree_clean(allow_dirty: bool) -> None:
    dirty = git("status", "--porcelain").rstrip()
    if dirty:
        if allow_dirty:
            print(f"{YELLOW}Working tree is not clean (allowed by --allow-dirty).{RESET}")
        else:
            raise ReleaseError("The working tree is not clean, so the build would not match a "
                               "commit. Commit or stash, or pass --allow-dirty:\n" + dirty)


def check_changelog(version: str) -> str:
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as handle:
        section = changelog_section(handle.read(), version)
    if section is None:
        raise ReleaseError(f"CHANGELOG.md has no non-empty `## [{version}]` section. Rename "
                           "[Unreleased] to it (and add a fresh [Unreleased]) before releasing.")
    return section


def check_tag_is_free(version: str) -> None:
    tag = f"v{version}"
    if tag_exists_locally(tag):
        raise ReleaseError(f"The tag {tag} already exists locally; bump the version.")
    remote = tag_on_origin(tag)
    if remote is None:
        print(f"{YELLOW}Could not reach origin to check for {tag}; check it yourself before tagging.{RESET}")
    elif remote:
        raise ReleaseError(f"The tag {tag} already exists on origin; bump the version.")


def check_binary_versions(version: str) -> None:
    for label, path in (("Quarry.exe", STANDALONE_EXE), ("Quarry.vst3", VST3_BINARY)):
        if not os.path.isfile(path):
            raise ReleaseError(f"{label} not found at {path}; build first.")
        found = read_file_version(path)
        # JUCE stamps the project version as X.Y.Z.0.
        if found.rsplit(".", 1)[0] != version:
            raise ReleaseError(f"{label} reports version {found}, expected {version}. "
                               "Reconfigure build-release (CMake caches the version).")
        print(f"{GREEN}{label} version {found}{RESET}")


def check_installer(version: str) -> str:
    path = os.path.join(RELEASE_DIR, installer_name(version))
    if not os.path.isfile(path):
        raise ReleaseError(f"{path} not found; run the build without --no-installer.")
    found = read_file_version(path)
    if found != f"{version}.0":
        raise ReleaseError(f"The installer reports version {found}, expected {version}.0.")
    print(f"{GREEN}{installer_name(version)} version {found}{RESET}")
    return path


def run_gates(version: str, allow_dirty: bool, with_installer: bool = True) -> None:
    check_tree_clean(allow_dirty)
    check_binary_versions(version)
    if with_installer:
        check_installer(version)


# --------------------------------------------------------------------------------------
# Steps
# --------------------------------------------------------------------------------------

def build_binaries(lto: bool) -> None:
    sys.path.insert(0, ROOT)
    import run  # the same fetches a developer build makes, so the pin lives in one place
    if not run.ensure_submodules() or not run.ensure_onnxruntime():
        raise ReleaseError("Could not prepare the submodules and onnxruntime.")
    # LTO is off by default for the reason run.py and ci.yml give: the prebuilt onnxruntime.lib is
    # compiled with /GL by one MSVC version and fails to link with C1047 under any other.
    # --lto is for a machine that has that compiler.
    run_cmd(["cmake", "-S", ".", "-B", BUILD_DIR, f"-DLTO={'ON' if lto else 'OFF'}",
             "-DBUILD_UNIT_TESTS=ON"])
    run_cmd(["cmake", "--build", BUILD_DIR, "--config", "Release", "--target", "UnitTests",
             "Quarry_Standalone", "Quarry_VST3", "--", "-m"])
    run_cmd([UNIT_TESTS_EXE])


def compile_installer(version: str) -> None:
    iscc = find_iscc()
    if not iscc:
        raise ReleaseError("ISCC.exe not found. Install Inno Setup 6 or set QUARRY_ISCC.")
    os.makedirs(RELEASE_DIR, exist_ok=True)
    stale = os.path.join(RELEASE_DIR, installer_name(version))
    if os.path.exists(stale):
        os.remove(stale)
    # quarry.iss prefixes ReleaseDir with ..\..\ (it lives two levels down), so pass it relative.
    release_dir_rel = os.path.relpath(ARTEFACTS, ROOT).replace("\\", "/")
    run_cmd([iscc, ISS, f"/DAppVersion={version}", f"/DReleaseDir={release_dir_rel}",
             f"/O{RELEASE_DIR}", "/Qp"])


def git_submodules() -> list:
    urls, paths = {}, {}
    config = git("config", "--file", ".gitmodules", "--get-regexp", r"^submodule\..*\.(path|url)$",
                 check=False)
    for line in config.splitlines():
        key, value = line.split(" ", 1)
        name, field = key[len("submodule."):].rsplit(".", 1)
        if field == "path":
            paths[value] = name
        else:
            urls[name] = value
    modules = []
    for path, commit, describe in parse_submodule_status(git("submodule", "status")):
        name = paths.get(path, path)
        modules.append({"name": os.path.basename(path), "path": path, "commit": commit,
                        "url": urls.get(name, ""), "describe": describe})
    return modules


def write_artifacts(version: str, section: str, with_installer: bool) -> list:
    os.makedirs(RELEASE_DIR, exist_ok=True)
    written = []

    if with_installer:
        installer = os.path.join(RELEASE_DIR, installer_name(version))
        sha_path = installer + ".sha256"
        with open(sha_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(sha256_line(file_sha256(installer), installer_name(version)))
        written += [installer, sha_path]

    sys.path.insert(0, ROOT)
    import run
    sbom = build_sbom(
        version, git_submodules(),
        {"version": run.ONNX_VERSION, "url": run.ONNX_UPSTREAM_URL, "sha256": run.ONNX_SHA256},
        datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        str(uuid.uuid4()))
    sbom_path = os.path.join(RELEASE_DIR, sbom_name(version))
    with open(sbom_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(sbom, handle, indent=2)
        handle.write("\n")
    written.append(sbom_path)

    notes_path = os.path.join(RELEASE_DIR, notes_name(version))
    with open(notes_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(section.strip("\n") + "\n")
    written.append(notes_path)
    return written


def cmd_build(args) -> int:
    version = read_version()
    print(f"Quarry {version}")

    if args.verify_only:
        run_gates(version, args.allow_dirty, with_installer=not args.no_installer)
        print(f"{GREEN}All gates passed.{RESET}")
        return 0

    check_tree_clean(args.allow_dirty)
    section = check_changelog(version)
    check_tag_is_free(version)

    if not args.skip_build:
        build_binaries(args.lto)
    check_binary_versions(version)

    if not args.no_installer:
        compile_installer(version)
    written = write_artifacts(version, section, with_installer=not args.no_installer)
    run_gates(version, args.allow_dirty, with_installer=not args.no_installer)

    print(f"{GREEN}Release {version} is in {RELEASE_DIR}:{RESET}")
    for path in written:
        print(f"  {os.path.basename(path)}")
    print(f"{GREY}Not code signed: the .sha256 is the integrity check, and SmartScreen will warn.{RESET}")
    return 0


def cmd_publish(args) -> int:
    version = read_version()
    tag = f"v{version}"
    check_changelog(version)
    run_gates(version, args.allow_dirty)

    files = [os.path.join(RELEASE_DIR, installer_name(version)),
             os.path.join(RELEASE_DIR, installer_name(version) + ".sha256"),
             os.path.join(RELEASE_DIR, sbom_name(version))]
    for path in files + [os.path.join(RELEASE_DIR, notes_name(version))]:
        if not os.path.isfile(path):
            raise ReleaseError(f"{path} is missing; run `release.py build`.")

    remote = tag_on_origin(tag)
    if remote is None:
        raise ReleaseError(f"Could not reach origin to confirm {tag} is pushed.")
    if not remote:
        raise ReleaseError(f"The tag {tag} is not on origin. `git tag {tag}` then "
                           f"`git push origin {tag}` first.")
    if tag_exists_locally(tag):
        local = git("rev-list", "-n", "1", tag).strip()
        if local != remote:
            raise ReleaseError(f"{tag} points at {local[:10]} locally and {remote[:10]} on origin.")

    command = gh_create_command(version, files)
    print(" ".join(f'"{part}"' if " " in part else part for part in command))
    if not args.yes:
        print(f"{GREY}Dry run. Add --yes to run it.{RESET}")
        return 0
    return subprocess.run(command, cwd=ROOT).returncode


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build, check and publish a Windows release of Quarry.")
    sub = parser.add_subparsers(dest="cmd")

    build = sub.add_parser("build", help="build, test, make the installer and the artifacts (default)")
    build.add_argument("--skip-build", action="store_true", help="reuse build-release/")
    build.add_argument("--no-installer", action="store_true", help="skip ISCC and the installer gates")
    build.add_argument("--verify-only", action="store_true", help="only run the gates on what exists")
    build.add_argument("--allow-dirty", action="store_true", help="allow uncommitted changes")
    build.add_argument("--lto", action="store_true",
                       help="configure with LTO on (needs the MSVC the prebuilt onnxruntime was built with)")

    publish = sub.add_parser("publish", help="print, or with --yes run, the gh release create command")
    publish.add_argument("--yes", action="store_true", help="actually create the release")
    publish.add_argument("--allow-dirty", action="store_true", help="allow uncommitted changes")
    return parser


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in ("build", "publish", "-h", "--help"):
        argv.insert(0, "build")
    args = make_parser().parse_args(argv)
    try:
        return cmd_publish(args) if args.cmd == "publish" else cmd_build(args)
    except ReleaseError as error:
        print(f"{YELLOW}{error}{RESET}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
