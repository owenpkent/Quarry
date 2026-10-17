# Auto update

How Quarry finds out about a new release, and in the Windows Standalone how it installs one.
The rules live in `Quarry/Source/Components/UpdatePolicy.*` (pure string logic, tested in
`Tests/update_policy_test.h`), the Windows I/O in `UpdateInstaller.*`, and the banner in
`UpdateCheck.*`. Releases are published to `owenpkent/Quarry` by `tools/release`.

## Who does what

| Build | Behaviour |
| --- | --- |
| Windows Standalone | Full flow: check, download, verify, silent install, relaunch. |
| Plugin (VST3 or AU, in any DAW) | Notify only. The banner says to close the DAW and run the installer, with a link to the release. |
| Any non-Windows build | Notify only, with the link. |

The check itself is the same everywhere and uses a strict comparison: a release is offered only
when its tag is strictly greater than the running version. An equal or older release, or a tag
that is not exactly `vX.Y.Z`, is ignored. (The inherited check used to offer any tag that merely
differed, which would have offered an older release.)

## The flow (Windows Standalone)

1. `GET https://api.github.com/repos/owenpkent/Quarry/releases/latest` on a worker thread (5 s
   timeout, 1 MB cap). Drafts and prereleases are ignored. Runs at launch unless the user turned
   off "Check for updates at startup" in the settings menu; "Check for updates" always works.
2. If the tag is newer and both release assets are present, the banner shows the version, the
   first line of the release notes, and **Install** and **Later**. Later hides the banner for the
   session.
3. Install downloads `QuarrySetup-X.Y.Z.exe.sha256` (cap 1 KB), then the installer (cap 300 MB,
   also rejected up front if Content-Length is larger) into a fresh `QuarryUpdate-<random>`
   folder under the user's temp directory, showing progress. Redirects are followed by hand, at
   most 5, and every hop is checked against the URL allowlist before any connection is made.
4. The installer file is opened with `GENERIC_READ` and `FILE_SHARE_READ` only and that handle is
   kept until the process exits, so nothing can write to, rename or delete it. Only then is it
   hashed (SHA-256, through that handle) and compared to the published hash. A mismatch deletes
   the file and stops.
5. The file's version resource must read exactly `X.Y.Z.0`.
6. The Restart Manager is asked which processes have
   `%CommonProgramFiles%\VST3\Quarry.vst3\Contents\x86_64-win\Quarry.vst3` loaded. If a host does,
   nothing is launched and the banner says "Ableton Live has the Quarry plugin loaded. Close it,
   then choose Install again." (the name is whatever the Restart Manager returns). Install again
   reuses the already verified, still pinned file.
7. If a take is being recorded or transcribed, the user is asked to confirm, since quitting loses
   it. Quarry has no project file, so that is the only unsaved work. Cancelling aborts the install.
8. The installer is started and the app quits through `JUCEApplicationBase::systemRequestedQuit()`,
   the same path as closing the window. The banner reads "Installing. Quarry will restart."
9. The installer waits for our process to exit, installs, and relaunches `Quarry.exe --updated`
   unelevated. The standalone shows "Updated to Quarry X.Y.Z".

If the installer finds the VST3 binary locked by a host anyway (a host opened between step 6 and
the install), it does not install and relaunches `Quarry.exe --update-blocked`. The standalone
shows "Update not installed: a DAW had the Quarry plugin loaded. Close it and choose Install
again.", fetches the release again, and adds the Install button to that message.

Errors are short strings in the banner ("Download failed", "Checksum did not match", "Installer
version did not match", "Could not start the installer") with the detail written through
`juce::Logger::writeToLog`.

## The contract with the release script and installer

- Tag `vX.Y.Z`, strictly `^v\d+\.\d+\.\d+$`. No prerelease suffix. Only stable tags are acted on.
- Assets `QuarrySetup-X.Y.Z.exe` and `QuarrySetup-X.Y.Z.exe.sha256`, matched by exact name. Any
  other asset (the SBOM, say) is ignored.
- The `.sha256` file is exactly `<64 lowercase hex>  QuarrySetup-X.Y.Z.exe` and a trailing
  newline (sha256sum format, two spaces). The reader accepts upper or lower case hex, and a `*`
  in place of the second space, and rejects anything else: another name, extra lines, extra bytes.
- The installer's FileVersion resource is `X.Y.Z.0`.
- Launch: `ShellExecuteExW`, verb `open`, parameters
  `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /COMPONENTS="mainapp" /WAITPID=<pid> /RELAUNCH`,
  with `"mainapp,plugin"` when `%CommonProgramFiles%\VST3\Quarry.vst3` exists.
- The installer relaunches `Quarry.exe --updated` unelevated, or `Quarry.exe --update-blocked`
  when it finds the plugin binary locked.

## Allowed download hosts

`github.com`, `objects.githubusercontent.com` and `release-assets.githubusercontent.com`, over
https, with no userinfo and no port other than 443, compared as an exact host (so
`github.com.evil.example` and `evilgithub.com` fail). `api.github.com` is used for the release
query only and is not a download host. The signed query string on a redirect is passed to the
network layer untouched (`makeRawUrl`), because `juce::URL` would turn a `+` in it into a space.

## Threat model

| Threat | Stopped by | Not stopped |
| --- | --- | --- |
| Corruption in transit, truncated download | Length checks and the SHA-256 | |
| Tampering with the asset on the CDN or by a proxy that cannot also change the hash file | SHA-256 against the published file, TLS | |
| Redirect or URL pointing at another host, plain http, userinfo tricks | Host allowlist on every hop, https only, 5 hop limit | |
| Oversized or endless download | 300 MB cap on Content-Length and on bytes read, 1 KB cap on the hash file | |
| Old installer renamed to look new | FileVersion must equal `X.Y.Z.0` | |
| Downgrade to an older release | Strict greater-than comparison, strict tag format | |
| File swapped between check and run | Handle held with read-only sharing, hash taken through it | |
| Release notes carrying control or direction-changing characters | Sanitizing (control characters, bidi overrides, 4000 char cap), notes shown as plain text | |
| A host holding the plugin makes the install half-succeed | Restart Manager check, installer's own check | |
| Someone who controls the GitHub release (stolen account or token) | Nothing | They can publish a malicious installer and a matching hash. |
| Someone who controls the repository's release pipeline | Nothing | Same. |

A hash published beside the binary protects against corruption and against tampering with the
asset alone. It does not protect against anyone who can edit the release, because they can
replace both files. Closing that needs a signature the client can pin; see the follow-ups.

## Why the plugin only notifies

A plugin runs inside the DAW. The installed VST3 is a file the DAW has loaded, so replacing it
needs the DAW closed, and the plugin cannot restart a DAW or know which project the user has
open. Quitting a host from inside a plugin is not something it may do. So the banner says what
to do ("Close your DAW, then run the installer") and links to the release. The Standalone, which
owns its process, can quit and be relaunched.

## Why the verb is `open`, not `runas`

The installer asks for administrator rights itself (Inno Setup, `PrivilegesRequired=admin`). If
Quarry launched it with `runas`, the installer would run as the child of an elevated launcher
and its `runasoriginaluser` relaunch would start Quarry elevated. With `open`, Setup starts
unelevated, raises its own UAC prompt and keeps an unelevated first instance, which is the one
`runasoriginaluser` uses, so the relaunch comes back unelevated. The cost is that the UAC prompt
appears after Quarry has already quit: if the user declines it, nothing is installed and Quarry
stays closed until it is started again. The "Install cancelled" message only covers a launch that
Windows itself refuses.

## Settings

"Check for updates at startup" (default on) is stored in `Quarry/QuarryUpdate.settings` in the
user's application data folder, shared by the Standalone and plugin instances.

## Not verified here

There are no releases yet, so the download, redirect handling and a real installer launch have
not been exercised against GitHub. The policy functions are covered by the unit tests; the
Windows I/O compiles and the app starts, nothing more.

## Follow-ups

- **Authenticode pinning.** Once the installer is signed, verify the signature with
  `WinVerifyTrust` and compare the signer's certificate thumbprint to a pinned value, as
  alpha-osk does. That is the check that survives a compromised release. Until then this
  design trusts whoever can publish to the repository.
- **macOS.** Notify only today. An updater there needs a signed and notarized bundle, a
  replacement strategy for a running `.app`, and the same pinning. Nothing in this change
  targets it.
- The narrow standalone window hides the banner entirely, as it did before, so an update cannot
  be started from that layout.
