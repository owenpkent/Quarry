#!/usr/bin/env python3
"""The fast prototyping loop: build the standalone and run it.

Double-click this file (or right-click -> Open) to build and launch Quarry's
standalone app. No VST3, no installer, no DAW rescan. The standalone can record from
this computer's own audio hardware (the microphone button in the toolbar), so most
changes can be tried without a DAW in the picture at all.

    py run.py                # build + launch the standalone
    py run.py --no-build     # just relaunch what is already built

On a fresh clone this also fetches what the build needs: the JUCE and RTNeural
submodules, and the prebuilt onnxruntime library plus the feature model that
build.bat would otherwise download. That tarball is 600 MB and comes from one person's
2023 GitHub release, so set OKSTUDIO_ONNXRUNTIME_CACHE to a directory outside every build
tree and it is fetched once for every checkout you will ever make; OKSTUDIO_ONNXRUNTIME_URL
points the fetch at a mirror if that release ever goes away.

If the build tree was generated somewhere else, because this repo was renamed or moved, it is
deleted and rebuilt. CMake bakes absolute paths into the cache and into every project file, so
such a tree cannot be reconfigured in place, and the failure otherwise surfaces from inside
MSBuild rather than anywhere that suggests the cause.

Note it configures with -DLTO=OFF. The checked-in onnxruntime.lib is compiled with
/GL by a specific MSVC version, so with link-time optimisation on, linking fails
outright (C1047) unless your compiler happens to match that one. build.bat still asks
for LTO, which is why it cannot build this repo here.

Windows only, like the product. Standard library only, so it runs on a bare Python.
"""

import argparse
import ctypes
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import time
import urllib.request

# Everything past this point is Win32: ctypes.wintypes, the WinDLL handles, the process
# enumeration. They raise on import elsewhere, long before main() can say anything useful,
# so the platform check has to sit above the first Windows-only line rather than in
# __main__.
if sys.platform != "win32":
    print("run.py is Windows-only. On macOS or Linux use ./build.sh.")
    sys.exit(1)

from ctypes import wintypes  # noqa: E402 - Windows-only, must follow the check above

ROOT = os.path.dirname(os.path.abspath(__file__))

# Our own messages interleave with cmake's, which writes straight to the console handle.
# Without this, Python's block buffering lands "Closing running Quarry..." *after*
# the build output it happened before, which reads like the script did things out of order.
try:
    sys.stdout.reconfigure(line_buffering=True)
except (AttributeError, OSError):
    pass

# The onnxruntime build the upstream NeuralNote authors publish, as used by build.bat. It is one
# individual's GitHub release, published once in March 2023, and every transcribing build in this
# line depends on it, so both where it is fetched from and where it is kept are overridable:
#
#   OKSTUDIO_ONNXRUNTIME_URL    fetch the tarball from a mirror instead of that release
#   OKSTUDIO_ONNXRUNTIME_CACHE  a directory outside every build tree to keep the tarball in, so a
#                               clean checkout unpacks it with no network at all
#
# The Windows tarball is 600 MB and unpacks to a 2.9 GB static library. The newer
# v1.14.1-neuralnote.2 release carries a macOS asset only, so .1 is the only Windows source
# there has ever been.
ONNX_VERSION = "v1.14.1-neuralnote.1"
ONNX_DIRNAME = f"onnxruntime-{ONNX_VERSION}-windows-x86_64"
ONNX_UPSTREAM_URL = (f"https://github.com/tiborvass/libonnxruntime-neuralnote/releases/download/"
                     f"{ONNX_VERSION}/{ONNX_DIRNAME}.tar.gz")
ONNX_URL = os.environ.get("OKSTUDIO_ONNXRUNTIME_URL", "").strip() or ONNX_UPSTREAM_URL
ONNX_CACHE_DIR = os.environ.get("OKSTUDIO_ONNXRUNTIME_CACHE", "").strip()
# SHA-256 of the upstream Windows tarball, 628,941,249 bytes, taken 2026-08-17. Checked on every
# download so a mirror is worth no more trust than the bytes it serves.
ONNX_SHA256 = "0fe0568469956181fafffdfb416916c10919114b0c8ce2badddd0a9a313ebab8"

# --------------------------------------------------------------------------------------
# Console
# --------------------------------------------------------------------------------------

def _enable_ansi() -> bool:
    """Turn on VT sequences so the colours below mean something. False if we can't."""
    if not sys.stdout.isatty():
        return False
    try:
        kernel32_local = ctypes.WinDLL("kernel32", use_last_error=True)
        handle = kernel32_local.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = wintypes.DWORD()
        if not kernel32_local.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32_local.SetConsoleMode(handle, mode.value | 0x0004))
    except OSError:
        return False


_ANSI = _enable_ansi()
GREY = "\033[90m" if _ANSI else ""
GREEN = "\033[92m" if _ANSI else ""
YELLOW = "\033[93m" if _ANSI else ""
RESET = "\033[0m" if _ANSI else ""


def console_is_ours() -> bool:
    """True when this console exists only for us, i.e. the script was double-clicked.

    Matters because that console vanishes the instant the script returns, taking any
    error message with it. Run from a terminal the count is at least two (the shell and
    us) and we must not pause. GetConsoleProcessList is the only reliable way to tell.
    """
    try:
        kernel32_local = ctypes.WinDLL("kernel32", use_last_error=True)
        buf = (wintypes.DWORD * 8)()
        count = kernel32_local.GetConsoleProcessList(buf, len(buf))
        return count == 1
    except OSError:
        return False


def hold_window_open() -> None:
    """Keep a double-clicked console up so the failure above can actually be read."""
    if not console_is_ours():
        return
    print()
    print(f"{YELLOW}Read the error above, then close this window (or press Enter).{RESET}")
    try:
        input()
    except (EOFError, KeyboardInterrupt):
        pass


# --------------------------------------------------------------------------------------
# Finding a cmake that will actually start
# --------------------------------------------------------------------------------------

def _cmake_candidates():
    """Every cmake worth trying, best first.

    The one on PATH is *not* best. This machine installs cmake through pip, and pip's
    entry point is a small unsigned launcher at Scripts\\cmake.exe that re-execs the real
    binary. Smart App Control (enforced here) blocks unsigned executables it does not
    recognise, so CreateProcess on the launcher fails outright - while the signed binary
    it wraps, in site-packages/cmake/data/bin, starts perfectly. Prefer anything signed
    and leave the launcher as the last resort.
    """
    override = os.environ.get("CMAKE")
    if override:
        yield override

    # Visual Studio ships a Microsoft-signed cmake. Not present on every install, and the
    # directory is the VS *major version*, never the marketing year.
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        pattern = os.path.join(base, "Microsoft Visual Studio", "*", "*", "Common7", "IDE",
                               "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        yield from sorted(glob.glob(pattern), reverse=True)

    # pip's real payload, behind the launcher.
    for key in ("purelib", "platlib"):
        path = sysconfig.get_paths().get(key)
        if path:
            yield os.path.join(path, "cmake", "data", "bin", "cmake.exe")

    # A normal MSI install.
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        yield os.path.join(base, "CMake", "bin", "cmake.exe")

    found = shutil.which("cmake")
    if found:
        yield found


_cmake_cached = None


def find_cmake():
    """The first cmake that actually launches, or None. Probed once per run."""
    global _cmake_cached
    if _cmake_cached is not None:
        return _cmake_cached

    on_path = shutil.which("cmake")
    seen = set()
    blocked = []
    for cand in _cmake_candidates():
        cand = os.path.normpath(cand)
        key = cand.lower()
        if key in seen or not os.path.exists(cand):
            continue
        seen.add(key)
        try:
            subprocess.run([cand, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
        except OSError as exc:
            blocked.append((cand, exc))
            continue
        except subprocess.CalledProcessError:
            continue
        if on_path and os.path.normcase(os.path.normpath(on_path)) != os.path.normcase(cand):
            print(f"{GREY}cmake on PATH ({on_path}) is not the one being used; using {cand}{RESET}")
        _cmake_cached = cand
        return cand

    _report_no_cmake(blocked)
    return None


def _report_no_cmake(blocked) -> None:
    print(f"{YELLOW}No usable cmake found.{RESET}")
    for path, exc in blocked:
        print(f"{YELLOW}  blocked: {path}{RESET}")
        print(f"{YELLOW}           {exc}{RESET}")
    if blocked:
        print(f"{YELLOW}  Smart App Control blocks unsigned executables. Either install CMake "
              f"from cmake.org (signed), or set CMAKE to a cmake.exe that runs.{RESET}")
    else:
        print(f"{YELLOW}  Install CMake, or set the CMAKE environment variable to its path.{RESET}")


def run_cmake(args) -> int:
    """Run cmake with `args`, turning a failure to even start into a readable message.

    subprocess raises OSError out of CreateProcess when the exe is blocked, and the raw
    traceback that produces is exactly the thing this script exists to avoid: it gets
    double-clicked, and the console is the only place an error can be read.
    """
    exe = find_cmake()
    if exe is None:
        return 1
    try:
        return subprocess.run([exe] + list(args)).returncode
    except OSError as exc:
        print(f"{YELLOW}Could not run cmake ({exe}):{RESET}")
        print(f"{YELLOW}  {exc}{RESET}")
        print(f"{YELLOW}  Set the CMAKE environment variable to a cmake.exe that runs.{RESET}")
        return 1


# --------------------------------------------------------------------------------------
# Dependencies a fresh clone does not have
# --------------------------------------------------------------------------------------

def ensure_submodules() -> bool:
    """JUCE and RTNeural live in submodules; without them CMake fails on line one."""
    if os.path.exists(os.path.join(ROOT, "ThirdParty", "JUCE", "CMakeLists.txt")) and \
       os.path.exists(os.path.join(ROOT, "ThirdParty", "RTNeural", "CMakeLists.txt")):
        return True

    git = shutil.which("git")
    if git is None:
        print(f"{YELLOW}ThirdParty/JUCE and ThirdParty/RTNeural are empty and git is not on "
              f"PATH, so they cannot be fetched.{RESET}")
        return False

    print(f"{GREY}Fetching the JUCE and RTNeural submodules (a few minutes, once)...{RESET}")
    result = subprocess.run([git, "submodule", "update", "--init", "--recursive", "--depth", "1"],
                            cwd=ROOT)
    if result.returncode != 0:
        print(f"{YELLOW}Fetching the submodules failed. See the git output above.{RESET}")
        return False
    return True


def file_sha256(path: str) -> str:
    """Digest a file a chunk at a time; this one is 600 MB and will not fit comfortably."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def onnxruntime_archive() -> tuple:
    """Where the tarball should live, and whether that place outlives this checkout.

    With OKSTUDIO_ONNXRUNTIME_CACHE set it is kept there and reused; without one it lands
    beside the source and is deleted once unpacked, as build.bat does.
    """
    if ONNX_CACHE_DIR:
        try:
            os.makedirs(ONNX_CACHE_DIR, exist_ok=True)
            return os.path.join(ONNX_CACHE_DIR, f"{ONNX_DIRNAME}.tar.gz"), True
        except OSError as exc:
            print(f"{YELLOW}Ignoring OKSTUDIO_ONNXRUNTIME_CACHE ({ONNX_CACHE_DIR}):{RESET}")
            print(f"{YELLOW}  {exc}{RESET}")
    return os.path.join(ROOT, f"{ONNX_DIRNAME}.tar.gz"), False


def ensure_onnxruntime() -> bool:
    """Download the prebuilt onnxruntime and feature model, as build.bat does."""
    lib = os.path.join(ROOT, "ThirdParty", "onnxruntime", "lib", "onnxruntime.lib")
    model = os.path.join(ROOT, "Lib", "ModelData", "features_model.ort")
    if os.path.exists(lib) and os.path.exists(model):
        return True

    archive, cached = onnxruntime_archive()

    if os.path.exists(archive):
        print(f"{GREY}Unpacking onnxruntime {ONNX_VERSION} from {archive}...{RESET}")
    else:
        print(f"{GREY}Downloading onnxruntime {ONNX_VERSION} (600 MB, once)...{RESET}")
        # Download to a sibling and rename, so a run killed mid-transfer cannot leave a
        # half-file that every later run then treats as the cached copy.
        part = f"{archive}.part"
        try:
            urllib.request.urlretrieve(ONNX_URL, part)
            got = file_sha256(part)
            if got != ONNX_SHA256:
                os.remove(part)
                print(f"{YELLOW}The onnxruntime download is not the expected file:{RESET}")
                print(f"{YELLOW}  expected sha256 {ONNX_SHA256}{RESET}")
                print(f"{YELLOW}  got             {got}{RESET}")
                print(f"{YELLOW}  from {ONNX_URL}{RESET}")
                return False
            os.replace(part, archive)
        except OSError as exc:
            if os.path.exists(part):
                os.remove(part)
            print(f"{YELLOW}Could not download onnxruntime:{RESET}")
            print(f"{YELLOW}  {exc}{RESET}")
            print(f"{YELLOW}  {ONNX_URL}{RESET}")
            if ONNX_URL == ONNX_UPSTREAM_URL:
                print(f"{YELLOW}  That release belongs to one person's account. Point "
                      f"OKSTUDIO_ONNXRUNTIME_URL at a mirror if it has gone.{RESET}")
            return False

    try:
        extracted = os.path.join(ROOT, "ThirdParty", ONNX_DIRNAME)
        target = os.path.join(ROOT, "ThirdParty", "onnxruntime")
        shutil.rmtree(extracted, ignore_errors=True)
        shutil.rmtree(target, ignore_errors=True)

        with tarfile.open(archive) as tar:
            tar.extractall(os.path.join(ROOT, "ThirdParty"))

        os.rename(extracted, target)

        # The feature model ships inside the archive and belongs with the other model data.
        shipped_model = os.path.join(target, "model.with_runtime_opt.ort")
        if os.path.exists(shipped_model):
            os.replace(shipped_model, model)
    except (OSError, tarfile.TarError) as exc:
        # A cached archive that will not unpack is worse than none, because it would be
        # reused forever. Drop it so the next run fetches again.
        if os.path.exists(archive):
            os.remove(archive)
        print(f"{YELLOW}The onnxruntime archive did not unpack:{RESET}")
        print(f"{YELLOW}  {exc}{RESET}")
        return False
    finally:
        if not cached and os.path.exists(archive):
            os.remove(archive)

    if not (os.path.exists(lib) and os.path.exists(model)):
        print(f"{YELLOW}The onnxruntime archive unpacked but is missing "
              f"onnxruntime.lib or features_model.ort.{RESET}")
        return False
    return True


def build_system_exists(build_dir: str) -> bool:
    """True once configure got far enough to actually generate something buildable.

    CMakeCache.txt is not that signal: cmake writes it early, so a configure that dies
    after project() leaves a cache behind and no solution at all. Treating the cache as
    "already configured" hands that half-configured tree to MSBuild on every later run,
    which then fails with an error about a missing project, permanently.
    """
    for pattern in ("*.sln", "*.slnx", "build.ninja", "Makefile"):
        if glob.glob(os.path.join(build_dir, pattern)):
            return True
    return False


def lto_is_on(cache_path: str) -> bool:
    """True if an existing build tree was configured with LTO, which cannot link here."""
    try:
        with open(cache_path, "r", encoding="utf-8", errors="replace") as cache:
            return any(line.startswith("LTO:BOOL=ON") for line in cache)
    except OSError:
        return False


def foreign_source_dir(cache_path: str) -> str:
    """The source directory a build tree was generated for, when that is not this one.

    Empty when the tree belongs to this checkout, or when there is no readable cache.

    CMake bakes absolute paths into the cache and into every project file it generates, so
    renaming or moving this repo leaves a tree that is still on disk but points entirely at
    a directory that is not. Such a tree cannot be reconfigured in place - cmake will not
    reuse a cache belonging to another directory - and because a generated solution is
    still sitting there, build_system_exists() above says the tree is ready and the build
    proceeds. The failure then surfaces from inside MSBuild's regenerate step, far from
    anything that suggests the real cause. The caller deletes the tree instead.
    """
    try:
        with open(cache_path, "r", encoding="utf-8", errors="replace") as cache:
            for line in cache:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    home = line.partition("=")[2].strip()
                    if os.path.normcase(os.path.normpath(home)) == \
                       os.path.normcase(os.path.normpath(ROOT)):
                        return ""
                    return home
    except OSError:
        pass
    return ""


# --------------------------------------------------------------------------------------
# Win32: find the running app and ask it to close politely
# --------------------------------------------------------------------------------------

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
WM_CLOSE = 0x0010

kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)
]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def running_pids(exe_name: str) -> list:
    """PIDs of every running process whose image is exactly `exe_name`."""
    size = 1024
    while True:
        arr = (wintypes.DWORD * size)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcesses(ctypes.byref(arr), ctypes.sizeof(arr), ctypes.byref(needed)):
            return []
        if needed.value < ctypes.sizeof(arr):
            break
        size *= 2  # the list filled the buffer exactly; it may have been truncated

    found = []
    for i in range(needed.value // ctypes.sizeof(wintypes.DWORD)):
        pid = arr[i]
        if pid == 0:
            continue
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            continue  # system processes we may not open; never ours
        try:
            buf = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(buf))
            if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(length)):
                if os.path.basename(buf.value).lower() == exe_name.lower():
                    found.append(pid)
        finally:
            kernel32.CloseHandle(handle)
    return found


def post_close_windows(pids: list) -> int:
    """WM_CLOSE every visible top-level window belonging to `pids`.

    A polite close matters: it lets JUCE write its settings on the way out, which is
    where the standalone remembers the state it reopens with.
    """
    closed = 0
    wanted = set(pids)

    def callback(hwnd, _lparam):
        nonlocal closed
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in wanted and user32.IsWindowVisible(hwnd):
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            closed += 1
        return True

    proc = WNDENUMPROC(callback)  # must outlive the EnumWindows call
    user32.EnumWindows(proc, 0)
    return closed


def wait_for_exit(exe_name: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if not running_pids(exe_name):
            return True
        time.sleep(0.15)
    return not running_pids(exe_name)


def close_running(exe_name: str, product: str) -> None:
    pids = running_pids(exe_name)
    if not pids:
        return

    print(f"{GREY}Closing running {product}...{RESET}")

    # Two attempts: an app still opening is not pumping messages yet and will sit on the
    # first WM_CLOSE. Once settled it exits in well under a second, so this costs nothing
    # in the normal case.
    for _ in range(2):
        pids = running_pids(exe_name)
        if not pids:
            return
        post_close_windows(pids)
        if wait_for_exit(exe_name, 6.0):
            return

    if running_pids(exe_name):
        print(f"{YELLOW}  ...it ignored the close, forcing it (settings may not persist).{RESET}")
        for pid in running_pids(exe_name):
            handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
            if handle:
                try:
                    kernel32.TerminateProcess(handle, 1)
                finally:
                    kernel32.CloseHandle(handle)
        wait_for_exit(exe_name, 5.0)


# --------------------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build and launch the Quarry standalone.")
    parser.add_argument("--no-build", action="store_true",
                        help="skip the build and just relaunch what is already there")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    os.chdir(ROOT)

    exe_name = "Quarry.exe"
    product = "Quarry"
    exe = os.path.join(ROOT, "build", "Quarry_artefacts", args.config, "Standalone", exe_name)

    close_running(exe_name, product)

    if not args.no_build:
        if not ensure_submodules():
            return 1
        if not ensure_onnxruntime():
            return 1

        # Only configure on a cold build tree, or on one configured with LTO on, which
        # cannot link against the prebuilt onnxruntime here. The VS generator re-runs
        # CMake by itself when CMakeLists.txt changes, so configuring every launch is
        # dead time.
        build_dir = os.path.join(ROOT, "build")
        cache = os.path.join(build_dir, "CMakeCache.txt")

        # Everything under build/ is derived, and a tree built for the old path is stale
        # whole, so start it over rather than trying to salvage any of it.
        stale_root = foreign_source_dir(cache)
        if stale_root:
            print(f"{GREY}The build tree was generated for {stale_root}, which is not this "
                  f"directory; rebuilding it from scratch...{RESET}")
            shutil.rmtree(build_dir, ignore_errors=True)
            if os.path.exists(cache):
                print(f"{YELLOW}Could not delete {build_dir}; something is holding a file "
                      f"open there (Visual Studio?).{RESET}")
                print(f"{YELLOW}  Close it, delete that folder, and run this again.{RESET}")
                return 1

        if not build_system_exists(build_dir) or lto_is_on(cache):
            if run_cmake(["-S", ".", "-B", "build", "-DLTO=OFF"]) != 0:
                print(f"{YELLOW}Configuring the build tree failed, so nothing was built.{RESET}")
                print(f"{YELLOW}  Fix the cause above and run this again; it will retry the "
                      f"configure. If it keeps failing, delete {build_dir} first.{RESET}")
                return 1

        started = time.monotonic()
        if run_cmake(["--build", "build", "--config", args.config,
                      "--target", "Quarry_Standalone"]) != 0:
            return 1
        print(f"{GREEN}Built the standalone in {time.monotonic() - started:.1f}s{RESET}")

    if not os.path.exists(exe):
        print(f"{YELLOW}{exe_name} not found at {exe} - run without --no-build first.{RESET}")
        return 1

    # Smart App Control is enforced on this machine and dev builds are unsigned, so it
    # blocks the first launch of a freshly linked exe while its reputation check runs,
    # then lets the same file through once that finishes. Absorb the transient here, and
    # say what is happening so the pause is legible rather than a hang.
    launch_timeout = 240.0
    deadline = time.monotonic() + launch_timeout
    launched = False
    announced = False
    while True:
        try:
            # Detached, so the app outlives this console instead of dying with it.
            subprocess.Popen([exe], cwd=ROOT, close_fds=True,
                             creationflags=subprocess.DETACHED_PROCESS
                             | subprocess.CREATE_NEW_PROCESS_GROUP)
            launched = True
            break
        except OSError:
            if time.monotonic() >= deadline:
                break
            if not announced:
                announced = True
                print(f"{GREY}Smart App Control is checking the new build; waiting for it "
                      f"(up to {launch_timeout:.0f}s)...{RESET}")
            time.sleep(2.0)
    if launched and announced:
        print(f"{GREEN}Cleared.{RESET}")

    if not launched:
        print(f"{YELLOW}Could not launch {exe_name} - Smart App Control blocked it for "
              f"{launch_timeout:.0f}s.{RESET}")
        print(f"{YELLOW}  Retry with: py run.py --no-build{RESET}")
        return 1

    print(f"{GREEN}Launched {product} ({args.config}){RESET}")
    print(f"{GREY}  The microphone button in the toolbar opens the AUDIO INPUT panel: "
          f"pick an input, then record.{RESET}")
    return 0


if __name__ == "__main__":
    try:
        code = main()
    except Exception:  # noqa: BLE001 - a double-clicked window must show the traceback
        import traceback
        traceback.print_exc()
        hold_window_open()
        sys.exit(1)
    if code != 0:
        hold_window_open()
    sys.exit(code)
