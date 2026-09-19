<#
.SYNOPSIS
    Formats every C++ source file in the repository with clang-format.

.DESCRIPTION
    Run this before committing. CI runs the same check with -Check and fails
    the build on any difference, so a formatted tree is never a review comment.

.EXAMPLE
    ./tools/format.ps1
    ./tools/format.ps1 -Check
#>
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
if (-not $clangFormat) {
    Write-Error "clang-format not found. It ships with Visual Studio (C++ workload) or 'winget install LLVM.LLVM'."
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
    & clang-format --dry-run --Werror @($files.FullName)
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Formatting differences found. Run ./tools/format.ps1 to fix them."
    }
    Write-Host "$($files.Count) files are correctly formatted."
} else {
    & clang-format -i @($files.FullName)
    Write-Host "Formatted $($files.Count) files."
}
