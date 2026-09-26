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

# CI's format job installs the distribution's clang-format on ubuntu-latest,
# which is 18. Different majors disagree about real cases -- a check that
# passes here and fails there is worse than no check, so a mismatch is said out
# loud rather than discovered in a pull request.
$expectedMajor = 18

function Resolve-ClangFormat {
    $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # vswhere is the one path Microsoft guarantees; everything else about a
    # Visual Studio installation is discovered through it.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $roots = & $vswhere -products * -latest -prerelease -property installationPath
        foreach ($root in $roots) {
            $candidates = @(
                'VC\Tools\Llvm\x64\bin\clang-format.exe',
                'VC\Tools\Llvm\bin\clang-format.exe'
            )
            foreach ($relative in $candidates) {
                $candidate = Join-Path $root $relative
                if (Test-Path $candidate) { return $candidate }
            }
        }
    }

    return $null
}

$clangFormat = Resolve-ClangFormat
if (-not $clangFormat) {
    Write-Error @"
clang-format not found, on PATH or in a Visual Studio installation.
Install it with 'winget install LLVM.LLVM', or add the C++ workload's
'C++ Clang Compiler for Windows' component in the Visual Studio Installer.
"@
}

$version = (& $clangFormat --version) -join ' '
if ($version -match 'version (\d+)\.') {
    if ([int]$Matches[1] -ne $expectedMajor) {
        Write-Warning "Using $version, CI uses clang-format $expectedMajor. Formatting differences between the two are this version's, not yours."
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
