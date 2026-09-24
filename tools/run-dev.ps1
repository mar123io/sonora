<#
.SYNOPSIS
    Launches the debug build with the switches this development machine needs.

.DESCRIPTION
    Chromium switches are passed on the command line: CefSettings leaves
    command_line_args_disabled false, so CEF reads the process command line and
    no code change is needed to try one.

    --disable-direct-composition is a workaround for a driver bug, NOT a
    property of the application. On this machine the AMD driver fails
    VideoProcessorGetOutputExtension, the GPU process aborts on a failed CHECK
    three times, Chromium falls back to software rendering, and the DevTools
    window then cannot get a rendering context at all.

    It lives in this script rather than in OnBeforeCommandLineProcessing on
    purpose. Baking it into the application would degrade rendering for every
    user to work around one driver, and it would quietly invalidate the
    performance numbers due in week 12 -- a startup time measured with
    compositing disabled is not the startup time anyone experiences.

    -DisableCaps sets SONORA_DISABLE_CAPS for the child process only. It is
    how the degraded interface is exercised on a current build, instead of
    against a shell from six months ago that nobody has to hand. Set for this
    launch alone: an environment variable left behind in a shell is a bug
    report waiting to be filed against the wrong thing.

.EXAMPLE
    ./tools/run-dev.ps1
    ./tools/run-dev.ps1 -Configuration Release
    ./tools/run-dev.ps1 -NoWorkarounds              # what a normal machine runs
    ./tools/run-dev.ps1 -DisableCaps diagnostics    # the degraded UI
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [switch]$NoWorkarounds,

    # Comma-separated capability names, e.g. 'diagnostics'.
    [string]$DisableCaps
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$preset = if ($Configuration -eq 'Debug') { 'win-debug' } else { 'win-release' }
$exe = Join-Path $root "build/$preset/bin/$Configuration/Sonora.exe"

if (-not (Test-Path $exe)) {
    Write-Error "Not built yet: $exe`nRun: cmake --build --preset $preset"
}

$switches = @()
if (-not $NoWorkarounds) {
    $switches += '--disable-direct-composition'
}

if ($switches) {
    Write-Host "Launching with: $($switches -join ' ')" -ForegroundColor DarkYellow
}

if ($DisableCaps) {
    Write-Host "SONORA_DISABLE_CAPS=$DisableCaps" -ForegroundColor DarkYellow
    # Scoped to this invocation: $env: in PowerShell lives in the current
    # window, so setting it without putting it back would change every later
    # run from the same terminal.
    $previous = $env:SONORA_DISABLE_CAPS
    $env:SONORA_DISABLE_CAPS = $DisableCaps
    try {
        & $exe @switches
    }
    finally {
        $env:SONORA_DISABLE_CAPS = $previous
    }
}
else {
    & $exe @switches
}
