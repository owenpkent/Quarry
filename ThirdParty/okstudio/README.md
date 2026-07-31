# okstudio JUCE kit (vendored)

`include/okstudio/WasapiLoopback.h` and `include/okstudio/CaptureMath.h` are verbatim copies
from the okstudio JUCE kit, https://github.com/owenpkent/okstudio-juce-kit. That repo is
private, so a submodule would leave public clones unable to build; the headers are checked in
here instead. They are header-only and are consumed through
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
`tools/sync_okstudio.py` and to the header list in the CMake drift check.
