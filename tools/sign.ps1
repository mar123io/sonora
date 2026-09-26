<#
.SYNOPSIS
    Signs a file with a self-signed certificate, and says what that is worth.

.DESCRIPTION
    What this demonstrates is the pipeline: a certificate, signtool, a
    timestamp, and a verification step that fails the build when the signature
    does not take. Every one of those is the same in production.

    What it does NOT demonstrate is trust, and the difference matters enough to
    be written here rather than discovered:

      * A self-signed certificate is trusted by exactly one machine -- the one
        that created it and put it in its own store. On anybody else's computer
        this signature is worse than no signature: SmartScreen treats an
        unknown publisher as a reason to warn, and a signature from a publisher
        nobody can verify does not change that.

      * Smart App Control, which Windows 11 turns on by itself on clean
        installs, refuses unsigned binaries outright and is not satisfied by a
        self-signed one either: it checks that the certificate chains to a
        provider it already trusts. This is the wall week 7 hit, documented in
        README.md.

      * A real release needs a code-signing certificate from a CA -- an OV
        certificate builds SmartScreen reputation over time, an EV one starts
        with it -- and the private key lives in an HSM or a cloud signing
        service, never in the repository and never on the build machine. The
        signing step in CI would call that service; everything else here stays
        the same.

.EXAMPLE
    ./tools/sign.ps1 -Path build/packages/Sonora-0.5.0-x64.msi
#>
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Subject = 'CN=Sonora Development (self-signed, not for distribution)',
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Path)) {
    Write-Error "Nothing to sign at $Path"
}

# signtool ships with the Windows SDK and is not on PATH. The SDK installs a
# versioned directory per release, so the newest one wins.
function Resolve-SignTool {
    $onPath = Get-Command signtool -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\bin')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

$signtool = Resolve-SignTool
if (-not $signtool) {
    Write-Error @"
signtool.exe not found. It ships with the Windows SDK:

    winget install Microsoft.WindowsSDK.10.0.26100
"@
}

# Reused if it is already there: a new certificate every run would mean every
# build looks like a different publisher, which is the opposite of what a
# signature is for.
$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate) {
    Write-Host "Creating a self-signed code-signing certificate ($Subject)" -ForegroundColor Cyan
    $certificate = New-SelfSignedCertificate `
        -Subject $Subject `
        -Type CodeSigningCert `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(3)
}

Write-Host "Signing $Path" -ForegroundColor Cyan
# /fd sha256 and /td sha256: SHA-1 has not been acceptable for years, and the
# default is still not SHA-256 everywhere.
#
# The timestamp is the part people leave out. Without it the signature stops
# verifying the day the certificate expires, including on copies installed
# years earlier; with it, the signature keeps saying "this was signed while the
# certificate was valid" forever.
& $signtool sign `
    /sha1 $certificate.Thumbprint `
    /fd sha256 `
    /tr $TimestampUrl `
    /td sha256 `
    $Path
if ($LASTEXITCODE -ne 0) {
    Write-Error "signtool failed with exit code $LASTEXITCODE"
}

# Verified rather than assumed. /pa uses the same policy the operating system
# uses when it decides whether to run the thing.
& $signtool verify /pa /v $Path
if ($LASTEXITCODE -ne 0) {
    Write-Error "The signature did not verify. See the note at the top of this file."
}

Write-Host ""
Write-Host "Signed with a self-signed certificate: valid on this machine, and on no other." -ForegroundColor Yellow
