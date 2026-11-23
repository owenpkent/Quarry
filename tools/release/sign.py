#!/usr/bin/env python3
"""Authenticode signing for the Quarry Windows release.

Ported from alpha-osk (build/windows/sign.py). Signs with the OK Studio Inc. EV certificate,
selected by SHA-1 thumbprint (a subject name could match more than one certificate), with a
SHA-256 file digest and an RFC 3161 SHA-256 timestamp.

    py tools/release/sign.py path\\to\\file.exe             # sign one file
    py tools/release/sign.py path\\to\\file.exe --verify    # check the signature and the signer

The private key is on a hardware token. It must be plugged in, and the token's client software
will prompt for its PIN on each signing attempt. Run from a normal, not elevated, shell: the token
is not visible to elevated processes.

Retries cover Windows Defender briefly locking a freshly written file, and a timestamp server
that does not answer. Standard library only. Windows only.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

# SHA-1 thumbprint of the OK Studio Inc. EV code signing certificate.
CERTIFICATE_SHA1 = "fc22b5221318f3f3f6b3eb2d969d7f99091557bf"
# RFC 3161 timestamp server. A fallback if DigiCert is down: http://timestamp.sectigo.com
TIMESTAMP_SERVER = "http://timestamp.digicert.com"
MAX_RETRIES = 5
RETRY_BASE_DELAY = 2.0   # seconds, multiplied by the attempt number
SIGN_TIMEOUT = 60        # seconds per signtool attempt. Covers a PIN prompt left unanswered.


class SigningError(Exception):
    """Signing or verification failed. The message says why."""


# --------------------------------------------------------------------------------------
# signtool
# --------------------------------------------------------------------------------------

def find_signtool() -> str:
    """The newest x64 signtool.exe under the Windows SDK, else the App Certification Kit, else PATH."""
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    kits = os.path.join(program_files, "Windows Kits", "10")
    candidates = []
    bin_dir = os.path.join(kits, "bin")
    if os.path.isdir(bin_dir):
        for sdk in sorted(os.listdir(bin_dir), reverse=True):
            candidates.append(os.path.join(bin_dir, sdk, "x64", "signtool.exe"))
    candidates.append(os.path.join(kits, "App Certification Kit", "signtool.exe"))
    candidates.append(shutil.which("signtool.exe") or shutil.which("signtool") or "")
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    raise SigningError("signtool.exe not found. Install the Windows SDK "
                       "(https://developer.microsoft.com/windows/downloads/windows-sdk/) "
                       "or put signtool.exe on PATH.")


def sign_command(signtool: str, path: str) -> list:
    return [signtool, "sign", "/sha1", CERTIFICATE_SHA1, "/fd", "sha256",
            "/tr", TIMESTAMP_SERVER, "/td", "sha256", path]


def verify_command(signtool: str, path: str) -> list:
    return [signtool, "verify", "/pa", "/v", path]


# --------------------------------------------------------------------------------------
# Sign and verify
# --------------------------------------------------------------------------------------

def sign_file(path: str, signtool: str | None = None) -> None:
    """Sign one file, retrying with linear backoff. Raises SigningError after MAX_RETRIES."""
    signtool = signtool or find_signtool()
    name = os.path.basename(path)
    last = ""
    for attempt in range(1, MAX_RETRIES + 1):
        print(f"[sign] attempt {attempt}/{MAX_RETRIES}: {name}")
        try:
            done = subprocess.run(sign_command(signtool, path), timeout=SIGN_TIMEOUT)
            if done.returncode == 0:
                print(f"[sign] signed {name}")
                return
            last = f"signtool exit code {done.returncode}"
        except subprocess.TimeoutExpired:
            last = f"signtool timed out after {SIGN_TIMEOUT}s (PIN prompt unanswered?)"
        if attempt < MAX_RETRIES:
            delay = RETRY_BASE_DELAY * attempt
            print(f"[sign] {last}; retrying in {delay:.0f}s (antivirus lock or timestamp server)")
            time.sleep(delay)
    raise SigningError(f"Could not sign {path} after {MAX_RETRIES} attempts: {last}. Is the "
                       "hardware token plugged in, and was the PIN entered?")


def verify_file(path: str, signtool: str | None = None) -> bool:
    """True if `signtool verify /pa` accepts the file's signature."""
    signtool = signtool or find_signtool()
    done = subprocess.run(verify_command(signtool, path), capture_output=True, text=True,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if done.returncode == 0:
        return True
    print(f"[sign] verification failed for {os.path.basename(path)}\n{done.stdout}{done.stderr}")
    return False


def signature_problem(path: str, signtool: str | None = None):
    """Why `path` is not validly signed by the pinned certificate, or None if it is.

    Asks Windows (Get-AuthenticodeSignature) for the status and signer thumbprint, then
    signtool for `verify /pa`, so it does not rely on anything the build recorded.
    """
    if not os.path.isfile(path):
        return f"{path} does not exist"
    # The path goes in through the environment, as data. Windows PowerShell joins everything after
    # -Command into the command text, so a path passed as a further argument never reaches $args.
    script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:QUARRY_SIGNATURE_PATH; "
              "$t = if ($s.SignerCertificate) { $s.SignerCertificate.Thumbprint } else { '' }; "
              "Write-Output ('{0}|{1}' -f $s.Status, $t)")
    # The full path, so a powershell.exe earlier on PATH cannot answer for Windows.
    powershell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32",
                              "WindowsPowerShell", "v1.0", "powershell.exe")
    # Without the caller's PSModulePath. When this script is started from PowerShell 7, or from
    # anything PowerShell 7 started (a VS Code terminal, say), that variable lists PowerShell 7's
    # module folders, and Windows PowerShell 5.1 then picks up a Microsoft.PowerShell.Security it
    # cannot load. Get-AuthenticodeSignature fails, the exit code is still 0, and a validly signed
    # installer reads as "status unknown". With the variable gone, 5.1 uses its own default.
    environment = {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}
    environment["QUARRY_SIGNATURE_PATH"] = os.path.abspath(path)
    done = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-Command", script],
                          capture_output=True, text=True, env=environment,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    status, _, thumbprint = done.stdout.strip().partition("|")
    if done.returncode != 0 or status != "Valid":
        if not status:
            # Nothing came back at all: PowerShell could not run the check. Say what it said,
            # because "unknown" alone looks like a bad signature.
            said = " ".join(done.stderr.split())[:200]
            return "Authenticode status is unknown, not Valid" + (f" (PowerShell: {said})" if said else "")
        return f"Authenticode status is {status}, not Valid"
    if thumbprint.lower() != CERTIFICATE_SHA1:
        return f"signed by {thumbprint or 'an unknown certificate'}, not the pinned {CERTIFICATE_SHA1}"
    if not verify_file(path, signtool):
        return "signtool verify /pa rejected the signature"
    return None


# --------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Sign one file with the OK Studio Inc. EV certificate.")
    parser.add_argument("path", help="the file to sign")
    parser.add_argument("--verify", action="store_true",
                        help="check the signature and the pinned signer instead of signing")
    args = parser.parse_args(argv)
    try:
        if args.verify:
            problem = signature_problem(args.path)
            print(problem or "signature valid, signed by the pinned certificate")
            return 1 if problem else 0
        sign_file(args.path)
        return 0
    except SigningError as error:
        print(f"[sign] {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
