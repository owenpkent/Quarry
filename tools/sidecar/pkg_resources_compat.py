"""A stand-in for `pkg_resources`, which setuptools 82 removed.

Two pinned engine packages still import it, for one function each:

- librosa 0.9.2, at import: `from pkg_resources import resource_filename` (librosa/util/files.py).
  kong (piano_transcription_inference) imports librosa, so without this kong does not import.
- transkun 2.0.1, inside transkun.transcribe:main: `pkg_resources.resource_filename(...)` for its
  default weight paths, evaluated even when --weight and --conf are passed.

Staying on a setuptools that still ships pkg_resources is not an option: every release below 82
carries advisories that are only fixed in 83 and 84. So this module supplies the one function
both callers use, and nothing else. It never replaces a real pkg_resources.

Use it in-process:

    import pkg_resources_compat
    pkg_resources_compat.install()   # before importing librosa or an engine package

or as a launcher for a module that is normally run with `python -m`, which is how transkun is
driven (a fresh interpreter does not inherit an in-process stand-in):

    python pkg_resources_compat.py transkun.transcribe in.wav out.mid --device cuda

Delete this file, and its callers' use of it, once neither package imports pkg_resources.
"""

import importlib.util
import pathlib
import runpy
import sys
import types

# This file's own path, for callers that run it as the launcher described above.
LAUNCHER = str(pathlib.Path(__file__).resolve())


def resource_filename(package_or_requirement, resource_name):
    """pkg_resources.resource_filename for the only case the two callers need: a module or
    package name that is installed as plain files. The resource is found next to the module's
    file (for a package, next to its __init__.py), which is where pkg_resources looks too."""
    module = sys.modules.get(package_or_requirement)
    origin = getattr(module, "__file__", None)

    if origin is None:
        spec = importlib.util.find_spec(package_or_requirement)
        origin = spec.origin if spec is not None else None

    if origin is None:
        raise ModuleNotFoundError(f"pkg_resources stand-in: cannot locate {package_or_requirement!r}")

    return str(pathlib.Path(origin).parent / resource_name)


def install() -> bool:
    """Puts the stand-in in sys.modules if there is no real pkg_resources. Returns True if the
    stand-in is now in use, False if the real module is there and was left alone."""
    existing = sys.modules.get("pkg_resources")
    if existing is not None:
        return getattr(existing, "__quarry_stand_in__", False)

    if importlib.util.find_spec("pkg_resources") is not None:
        return False

    stand_in = types.ModuleType("pkg_resources")
    stand_in.__doc__ = "Quarry's stand-in for pkg_resources; see tools/sidecar/pkg_resources_compat.py."
    setattr(stand_in, "__quarry_stand_in__", True)
    setattr(stand_in, "resource_filename", resource_filename)
    sys.modules["pkg_resources"] = stand_in
    return True


def main(argv) -> int:
    if len(argv) < 2:
        print("usage: pkg_resources_compat.py <module> [args...]", file=sys.stderr)
        return 2

    install()
    # Python put this script's directory first on sys.path. The target has no business importing
    # from it (engines.py and friends live here), so take it off before handing over.
    if sys.path and sys.path[0] and pathlib.Path(sys.path[0]).resolve() == pathlib.Path(__file__).resolve().parent:
        del sys.path[0]
    sys.argv = [argv[1], *argv[2:]]
    runpy.run_module(argv[1], run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
