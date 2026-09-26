<#
.SYNOPSIS
    Builds Sonora in Release and packages it into an MSI.

.DESCRIPTION
    Four steps, in this order, and the order is the point:

      1. cmake --preset win-release -DSONORA_BUILD_UI=ON
         The UI is built by the build, not assumed to be lying around. A
         release is the one build nobody gets to "just rebuild and try again".

      2. cmake --build / cmake --install into a staging directory
         The install rules in src/shell/CMakeLists.txt decide what ships. The
         build output directory is not the payload: it also holds sonora_tests.exe,
         and an installer that ships its own test runner is an installer nobody
         read.

      3. wix build
         WiX v4, from the .NET tool. installer/Sonora.wxs harvests the staging
         directory wholesale.

      4. signtool, when -Sign is given
         See tools/sign.ps1 for what the certificate is and what it is not.

    The version comes from CMake, which got it from the git tag. Nothing here
    invents one.

.EXAMPLE
    ./tools/package.ps1
    ./tools/package.ps1 -Sign
    ./tools/package.ps1 -SkipBuild        # repackage what is already staged
#>
param(
    [switch]$Sign,
    [switch]$SkipBuild,
    [string]$Preset = 'win-release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build/$Preset"
$stageDir = Join-Path $buildDir 'stage'
$outDir = Join-Path $root 'build/packages'

function Invoke-Step {
    param([string]$What, [scriptblock]$Body)
    Write-Host "==> $What" -ForegroundColor Cyan
    & $Body
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$What failed with exit code $LASTEXITCODE"
    }
}

# ---------------------------------------------------------------------------
# 1 + 2: build and stage
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Invoke-Step 'Configuring (Release, building the UI)' {
        cmake --preset $Preset -DSONORA_BUILD_UI=ON
    }
    Invoke-Step 'Building' {
        cmake --build --preset $Preset
    }

    # Removed rather than overwritten: a staging directory that still holds a
    # file deleted three commits ago would put it in the MSI, and nothing here
    # would say so.
    if (Test-Path $stageDir) {
        Remove-Item -Recurse -Force $stageDir
    }
    Invoke-Step 'Staging the payload' {
        cmake --install $buildDir --config Release --prefix $stageDir
    }
}

if (-not (Test-Path (Join-Path $stageDir 'Sonora.exe'))) {
    Write-Error "No Sonora.exe in $stageDir. Run without -SkipBuild."
}

# ---------------------------------------------------------------------------
# The version, from the build that produced the payload
# ---------------------------------------------------------------------------
# Read out of CMake's cache rather than asked of git again: the number in the
# MSI has to be the number in the binaries, and asking twice is how the two
# come to disagree over a commit made between the build and the packaging.
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
$versionLine = Select-String -Path $cacheFile -Pattern '^CMAKE_PROJECT_VERSION:' -ErrorAction SilentlyContinue
if (-not $versionLine) {
    Write-Error "No CMAKE_PROJECT_VERSION in $cacheFile; configure the preset first."
}
$version = ($versionLine.Line -split '=', 2)[1].Trim()

# MSI versions are four numbers and compare on the first three only. The fourth
# is carried for information; a release always has it at 0.
$msiVersion = "$version.0"
Write-Host "Version: $msiVersion"

# ---------------------------------------------------------------------------
# 3: wix
# ---------------------------------------------------------------------------
# WiX v4, pinned, and the pin is the interesting part.
#
# `dotnet tool install --global wix` with no version installs the newest, and
# the newest is now v7 -- which refuses to build anything until you accept the
# Open Source Maintenance Fee licence, a commercial arrangement that has
# nothing to do with what this repository is. The first run of this script hit
# exactly that: a major version nobody chose, gating a build on a licence
# nobody read.
#
# Which makes it the fourth thing pinned this week, after CEF, the vcpkg
# packages and clang-format -- and the only one that had to prove the rule by
# breaking first.
$wixVersion = '4.*'
$wixInstall = @"
    dotnet tool uninstall --global wix
    dotnet tool install --global wix --version $wixVersion
"@

if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    Write-Error @"
WiX not found. It is a .NET global tool, and the version matters:
$wixInstall
If 'dotnet' itself is missing, install the .NET SDK first:

    winget install Microsoft.DotNet.SDK.8
"@
}

# Checked rather than assumed: v7 is on PATH under the same name, and what it
# produces is a licence message in the middle of a build log.
$wixVersionOutput = (& wix --version) -join ' '
if ($wixVersionOutput -notmatch '^\s*4\.') {
    Write-Error @"
This is WiX $wixVersionOutput; installer/Sonora.wxs is written for v4.
$wixInstall
"@
}
Write-Host "WiX: $wixVersionOutput"

# ---------------------------------------------------------------------------
# The payload fragment
# ---------------------------------------------------------------------------
# Generated from what was actually staged, every time, rather than a list kept
# by hand: a list of 250 files maintained by a person is a list that is wrong
# the first time CEF adds a .dll, and wrong silently -- the MSI builds, and the
# application it installs does not start.
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command py -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Error "Python 3 not found; it is needed for tools/gen_installer_files.py."
}

$payloadWxs = Join-Path $buildDir 'generated/payload.wxs'
Invoke-Step 'Generating the payload fragment' {
    & $python.Source (Join-Path $PSScriptRoot 'gen_installer_files.py') `
        --stage $stageDir --out $payloadWxs
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$msi = Join-Path $outDir "Sonora-$version-x64.msi"

Invoke-Step 'Building the MSI' {
    wix build `
        (Join-Path $root 'installer/Sonora.wxs') `
        $payloadWxs `
        -define "Version=$msiVersion" `
        -define "PayloadDir=$stageDir" `
        -arch x64 `
        -out $msi
}

# ---------------------------------------------------------------------------
# 4: signing
# ---------------------------------------------------------------------------
if ($Sign) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path $msi
}

Write-Host ""
Write-Host "MSI: $msi" -ForegroundColor Green
Get-Item $msi | Select-Object Name, Length, LastWriteTime | Format-List
