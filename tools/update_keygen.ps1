<#
.SYNOPSIS
    Makes the Ed25519 key pair that signs release manifests, and says what to do with
    each half.

.DESCRIPTION
    The whole of an installation's trust in an update is one 32-byte public key compiled
    into the binary (src/update/src/signature.cpp) and one signature over the manifest --
    see ADR 0009. This script produces the pair and nothing else: it does not install
    anything, does not change any file in the repository, and prints the private half
    nowhere.

    The private half is written to a file in the current directory and is meant to go
    straight into a repository secret and then be deleted. It does not belong in this
    tree, in a password manager's notes field, or in a terminal's scrollback.

    OpenSSL signs and libsodium verifies, and those two agree about Ed25519 --
    tests/test_update_manifest.cpp checks a real signature made by the first against the
    second, because "they both implement the standard" is the kind of thing that is true
    until it is not.

.EXAMPLE
    ./tools/update_keygen.ps1
    # ...then paste the printed array into src/update/src/signature.cpp, replacing the
    # development key, and run the gh command it prints.
#>
[CmdletBinding()]
param(
    # Outside the repository, deliberately. The first version of this defaulted to the
    # working directory, where one `git add -A` would have committed a private key -- and a
    # private key that has been in a commit is compromised from that moment, because
    # rewriting the history does not un-publish what was pushed. .gitignore refuses *.pem as
    # well, but a default that cannot go wrong beats a rule that catches it going wrong.
    [string]$Out = (Join-Path $env:TEMP 'sonora-release-key.pem')
)

$ErrorActionPreference = 'Stop'

$openssl = Get-Command openssl -ErrorAction SilentlyContinue
if (-not $openssl) {
    Write-Error @'
openssl not found. It ships with Git for Windows:

    C:\Program Files\Git\usr\bin\openssl.exe

...or install it with `winget install ShiningLight.OpenSSL.Light`.
'@
}

if (Test-Path $Out) {
    Write-Error "$Out already exists. Signing keys are not overwritten by a script."
}

& openssl genpkey -algorithm ed25519 -out $Out
if ($LASTEXITCODE -ne 0) { Write-Error 'openssl genpkey failed' }

# The public half, as the last 32 bytes of the DER SubjectPublicKeyInfo. Ed25519's
# encoding is fixed-length, so "the last 32 bytes" is exact rather than a guess.
$derPath = [System.IO.Path]::GetTempFileName()
try {
    & openssl pkey -in $Out -pubout -outform DER -out $derPath
    if ($LASTEXITCODE -ne 0) { Write-Error 'openssl pkey failed' }
    $der = [System.IO.File]::ReadAllBytes($derPath)
    if ($der.Length -lt 32) { Write-Error 'that does not look like an Ed25519 public key' }
    $raw = $der[($der.Length - 32)..($der.Length - 1)]
} finally {
    Remove-Item $derPath -Force -ErrorAction SilentlyContinue
}

$lines = @()
for ($i = 0; $i -lt 32; $i += 11) {
    $slice = $raw[$i..([Math]::Min($i + 10, 31))]
    $lines += '    ' + (($slice | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', ') + ','
}

Write-Host ''
Write-Host 'The public half. Paste this into src/update/src/signature.cpp, replacing' -ForegroundColor Cyan
Write-Host 'kDevelopmentKey entirely -- not appending to the list, replacing.' -ForegroundColor Cyan
Write-Host ''
Write-Host 'constexpr PublicKey kReleaseKey2026 = {'
$lines | ForEach-Object { Write-Host $_ }
Write-Host '};'
Write-Host ''
Write-Host ('hex, for comparing against the workflow: ' +
    (($raw | ForEach-Object { '{0:x2}' -f $_ }) -join ''))
Write-Host ''
Write-Host 'The private half is in ' -NoNewline
Write-Host $Out -ForegroundColor Yellow -NoNewline
Write-Host '. Put it in the repository secret and delete it:'
Write-Host ''
# Get-Content and a pipe, not `<`. PowerShell reserves `<` and refuses the line outright
# ("Operatore '<' riservato per utilizzi futuri"), so the first version of this script
# printed, from PowerShell, a command PowerShell cannot parse. A tool that hands you a
# command should hand you one that runs in the shell it is running in.
Write-Host "    Get-Content -Raw '$Out' | gh secret set SONORA_RELEASE_KEY"
Write-Host "    Remove-Item '$Out'"
Write-Host ''
Write-Host 'A key whose private half has been anywhere but a secret store is not a signing' -ForegroundColor Yellow
Write-Host 'key. The release job refuses to publish a manifest while the binary still' -ForegroundColor Yellow
Write-Host 'trusts the development key, which is the check that makes this instruction' -ForegroundColor Yellow
Write-Host 'something other than a note in a comment.' -ForegroundColor Yellow
