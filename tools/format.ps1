<#
.SYNOPSIS
    Formats every C++ source file in the repository with clang-format.

.DESCRIPTION
    Run this before committing. CI runs the same check with -Check and fails
    the build on any difference, so a formatted tree is never a review comment.

    clang-format is taken from PATH when it is there, and otherwise from the
    Visual Studio installation: the C++ workload ships LLVM but does not put it
    on PATH, so the tool is almost always present on a machine that can build
    this repository and almost never reachable by name.

.EXAMPLE
    ./tools/format.ps1
    ./tools/format.ps1 -Check
#>
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

# The formatter's version, exactly, and the same number as
# CLANG_FORMAT_VERSION in .github/workflows/ci.yml -- which installs it from
# pip rather than taking whatever the distribution ships, precisely so that
# there is a number to match here.
#
# Only the major is compared: LLVM's formatting changes between majors and not
# within one, so demanding 20.1.7 exactly would reject a perfectly good 20.1.8
# and teach people to ignore the warning.
$expectedVersion = '20.1.7'
$expectedMajor = 20

function Resolve-ClangFormat {
    $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # Not on PATH is the normal case on Windows, not the exception: the Visual
    # Studio C++ workload ships LLVM without adding it, and the standalone LLVM
    # installer only adds it if you tick a box during setup. So this looks in
    # the places installers actually put it.
    $candidates = @()

    # vswhere is the one path Microsoft guarantees; everything else about a
    # Visual Studio installation is discovered through it.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($root in (& $vswhere -products * -latest -prerelease -property installationPath)) {
            $candidates += (Join-Path $root 'VC\Tools\Llvm\x64\bin\clang-format.exe')
            $candidates += (Join-Path $root 'VC\Tools\Llvm\bin\clang-format.exe')
        }
    }

    # A standalone LLVM: machine-wide, 32-bit-on-64, per-user, and the shim
    # directory winget keeps for packages it installed.
    $candidates += (Join-Path $env:ProgramFiles 'LLVM\bin\clang-format.exe')
    $candidates += (Join-Path ${env:ProgramFiles(x86)} 'LLVM\bin\clang-format.exe')
    $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\LLVM\bin\clang-format.exe')
    $candidates += (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\clang-format.exe')

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }

    return $null
}

$clangFormat = Resolve-ClangFormat
if (-not $clangFormat) {
    Write-Error @"
clang-format not found: not on PATH, not in a Visual Studio installation, and
not in any of the usual LLVM install directories.
Install it with 'winget install LLVM.LLVM', or add the C++ workload's
'C++ Clang Compiler for Windows' component in the Visual Studio Installer.
"@
}

$version = (& $clangFormat --version) -join ' '
if (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) {
    Write-Host "clang-format: $clangFormat"
}
if ($version -match 'version (\d+)\.') {
    if ([int]$Matches[1] -ne $expectedMajor) {
        Write-Warning "Using $version, CI uses clang-format $expectedVersion. Formatting differences between the two are this version's, not yours."
    }
}

$root = Split-Path -Parent $PSScriptRoot
$patterns = '*.cpp', '*.h', '*.mm'
$files = Get-ChildItem -Path (Join-Path $root 'src'), (Join-Path $root 'tests') `
    -Recurse -Include $patterns -File

if (-not $files) {
    Write-Host 'No source files found.'
    exit 0
}

if ($Check) {
    & $clangFormat --dry-run --Werror @($files.FullName)
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Formatting differences found. Run ./tools/format.ps1 to fix them."
    }
    Write-Host "$($files.Count) files are correctly formatted."
} else {
    & $clangFormat -i @($files.FullName)
    Write-Host "Formatted $($files.Count) files."
}
