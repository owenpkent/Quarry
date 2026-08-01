# okstudio JUCE kit (vendored)

`include/okstudio/` holds verbatim copies of four headers from the okstudio JUCE kit:
`WasapiLoopback.h` and `CaptureMath.h` for recording what the computer is playing, and
`Obsidian.h` with its `MouseOnly.h` dependency for the look and feel. They come from
https://github.com/owenpkent/okstudio-juce-kit. That repo is private, so a submodule would
leave public clones unable to build; the headers are checked in here instead. The look and
feel pair is needed on every platform, so this is not a Windows-only dependency. They are header-only and are consumed through
`${CMAKE_CURRENT_LIST_DIR}/ThirdParty/okstudio/include`, which keeps
`#include <okstudio/WasapiLoopback.h>` resolving unchanged.

`UPSTREAM.txt` records which kit commit these copies came from and when they were taken. It is
written by the sync script, not by hand.

To re-sync from a checkout of the kit:

    py tools/sync_okstudio.py              copy any changed headers and re-pin UPSTREAM.txt
    py tools/sync_okstudio.py --check      report drift and exit 1, copy nothing
    py tools/sync_okstudio.py --kit PATH   use a kit checkout somewhere else

The kit is looked for in order: `--kit`, `$OKSTUDIO_KIT_DIR`, then the sibling checkout
`../okstudio-juce-kit`. The script lists the kit commits that touched these headers since the
pin before it copies anything, so the diff is never a surprise.

CMake does the same comparison on every configure, but only when this machine has a kit
checkout at `OKSTUDIO_KIT_DIR` (the same sibling path by default). If a vendored header
differs from the kit's, it warns and names the sync command. A clone without the kit sees
nothing, which is the point: the warning fires where the drift originates.

If a re-synced header picks up new `okstudio/` includes, add them to `VENDORED` in
`tools/sync_okstudio.py`. The CMake check needs no such list: it globs this directory, so it
cannot fall behind the sync script.
