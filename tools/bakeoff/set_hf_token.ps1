# One-time HuggingFace auth for the bake-off's gated models (MuScriptor).
#
# Run this yourself in a terminal (it prompts for the token, which should never
# be pasted into anything else):
#
#   powershell -ExecutionPolicy Bypass -File tools\bakeoff\set_hf_token.ps1
#
# Prerequisite: accept the model licence in a browser first, at
#   https://huggingface.co/MuScriptor/muscriptor-medium   (and -small / -large if wanted)
# then create a Read token at https://huggingface.co/settings/tokens.
#
# The script stores the token in the two places tooling looks:
#   1. %USERPROFILE%\.cache\huggingface\token   (what huggingface_hub reads)
#   2. the persistent user-level HF_TOKEN environment variable
# and then verifies it by asking the HuggingFace API who you are.

$secure = Read-Host -Prompt "Paste your HuggingFace token (input hidden)" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
$token = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)

if (-not $token -or -not $token.StartsWith("hf_")) {
    Write-Host "That does not look like a HuggingFace token (expected it to start with hf_). Nothing was saved." -ForegroundColor Yellow
    exit 1
}

$tokenDir = Join-Path $env:USERPROFILE ".cache\huggingface"
New-Item -ItemType Directory -Force $tokenDir | Out-Null
# huggingface_hub expects the bare token with no trailing newline; -NoNewline and
# ASCII keep the file byte-exact.
[IO.File]::WriteAllText((Join-Path $tokenDir "token"), $token, [Text.Encoding]::ASCII)

setx HF_TOKEN $token | Out-Null
$env:HF_TOKEN = $token

$py = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
$check = & $py -c "from huggingface_hub import whoami; print(whoami()['name'])" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "Token saved and verified: logged in as '$check'." -ForegroundColor Green
    Write-Host "MuScriptor downloads will work once the licence is accepted on the model page."
} else {
    Write-Host "Token saved, but verification failed:" -ForegroundColor Yellow
    Write-Host $check
    Write-Host "Check that the token is a valid Read token, then re-run this script."
}
