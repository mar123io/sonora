<#
.SYNOPSIS
    Turns a minidump into a stack trace, using the PDBs from the build it came
    from.

.DESCRIPTION
    A minidump carries module names, addresses and the build id of each module.
    It does not carry symbols, and nothing reconstructs them afterwards: the
    names live in the PDB that the linker produced for that exact build, and a
    PDB from a rebuild of the same source is not the same file. That is why
    every release attaches Sonora.pdb, sonora_helper.pdb and
    sonora-updater.pdb, and why this script's -Symbols argument is not optional.
    See ADR 0014.

    What this does is drive cdb, which ships with the Windows SDK's Debugging
    Tools. It does not vendor a symbolisation stack. Breakpad's dump_syms and
    minidump_stackwalk would avoid the dependency, and building them is a week
    of somebody's time for a script that would still only run on Windows.

    The symbol path is the directory given plus Microsoft's public server, in
    that order. The second half is what makes the frames inside ntdll, kernel32
    and the C runtime readable; without it a stack that leaves Sonora's own code
    ends in hexadecimal, which is usually the half that says what happened.
    Nothing in libcef.dll will ever resolve: Chromium's symbols are 3 GB per
    build and CEF does not publish them for its binary distributions, so a frame
    inside CEF is a module name and an offset and that is the ceiling.

    If there is a .json next to the dump -- tools/crash_receiver.py writes one --
    its crash keys are printed first. Which version, which build, what the
    updater was doing, whether audio started: the five things the stack does not
    say and that usually decide what to look at.

.EXAMPLE
    # The usual case: whatever just crashed, against the build that produced it.
    ./tools/symbolise.ps1 -Symbols build/win-debug/bin/Debug

.EXAMPLE
    ./tools/symbolise.ps1 -Dump build/crashes/20260927-125131-602313.dmp `
                          -Symbols build/win-release/bin/Release

.EXAMPLE
    # A release's PDBs, downloaded from the tag they belong to:
    gh release download v0.7.0 --pattern '*.pdb' --dir build/symbols/0.7.0
    ./tools/symbolise.ps1 -Dump crash.dmp -Symbols build/symbols/0.7.0
#>
param(
    # Optional, and it defaults to the newest .dmp in -CrashDir. The name of a dump is a
    # timestamp to the microsecond: nobody types one, everybody copies it, and the one
    # anybody wants is the one that just arrived.
    [string]$Dump,
    [Parameter(Mandatory = $true)][string]$Symbols,
    [string]$CrashDir = 'build/crashes',
    # Every thread rather than the faulting one. A deadlock is the case where the
    # interesting stack is not the one that crashed, and it is also the case
    # where -All is the difference between a diagnosis and a guess.
    [switch]$All,
    # The commands cdb runs. Overridable because the day somebody needs `dt` or
    # `!locks` is the day they should not have to edit this file.
    [string]$Commands
)

$ErrorActionPreference = 'Stop'

# Get-Item inside a try, and not Test-Path, because Test-Path *throws* when the path contains
# a character Windows does not allow -- an angle bracket left in from a copied command line,
# say -- so the friendly message below never got printed and what came out instead was a .NET
# ArgumentException with a stack trace. A tool for a bad day should not have a bad day.
#
# Through PowerShell and not [System.IO.File]::Exists, which resolves a relative path against
# the process's working directory rather than the one the prompt is showing you.
function Resolve-InputPath {
    param([string]$Path, [ValidateSet('File', 'Directory')][string]$Kind, [string]$Description)
    try {
        $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    } catch {
        Write-Error "No $Description at $Path"
        return
    }
    if ($Kind -eq 'File' -and $item.PSIsContainer) {
        Write-Error "$Path is a folder; -Dump wants the .dmp itself"
        return
    }
    if ($Kind -eq 'Directory' -and -not $item.PSIsContainer) {
        Write-Error "$Path is a file; -Symbols wants the folder the PDBs are in"
        return
    }
    return $item.FullName
}

if (-not $Dump) {
    # Newest by write time, and it says which one it picked: a script that silently chooses a
    # file is a script that will one day symbolise yesterday's crash while somebody stares at
    # the stack wondering why their fix did nothing.
    $newest = Get-ChildItem -LiteralPath $CrashDir -Filter '*.dmp' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest) {
        Write-Error @"
No .dmp in $CrashDir, and no -Dump given.

Either pass one, or produce one: start tools/crash_receiver.py, then run a DevTools build
with --simulate-crash=browser. See the crash section of README.md.
"@
    }
    $Dump = $newest.FullName
    Write-Host "dump: newest in $CrashDir -- $($newest.Name), $($newest.LastWriteTime)" `
        -ForegroundColor Cyan
}

$dumpPath = Resolve-InputPath -Path $Dump -Kind File -Description 'dump'
$symbolPath = Resolve-InputPath -Path $Symbols -Kind Directory -Description 'symbol directory'

# cdb.exe is in the Debugging Tools for Windows, which is a feature of the
# Windows SDK and is not installed by default with Visual Studio's C++
# workload. It is also not on PATH. Same shape as Resolve-SignTool in
# tools/sign.ps1, and the same reason: the SDK installs a versioned directory
# per release, so the newest one wins.
function Resolve-Cdb {
    $onPath = Get-Command cdb -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\Debuggers')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Recurse -Filter cdb.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

$cdb = Resolve-Cdb
if (-not $cdb) {
    Write-Error @"
cdb.exe not found. It is part of the Debugging Tools for Windows:

    winget install Microsoft.WindowsSDK.10.0.26100

...and if the SDK is already installed, the Debugging Tools are a separate
feature of it: Settings > Apps > Windows Software Development Kit > Modify, and
tick "Debugging Tools for Windows".
"@
}

# The annotations first, because they decide whether the stack is worth reading.
# Written by tools/crash_receiver.py beside the dump; a dump copied off a user's
# machine by hand will not have one, and that is not an error.
$annotations = [System.IO.Path]::ChangeExtension($dumpPath, '.json')
if (Test-Path -PathType Leaf $annotations) {
    Write-Host "Crash keys from $(Split-Path -Leaf $annotations):" -ForegroundColor Cyan
    $keys = Get-Content $annotations -Raw | ConvertFrom-Json
    foreach ($property in $keys.PSObject.Properties | Sort-Object Name) {
        # The sonora_ ones are this project's five; the rest are Chromium's own
        # and there are dozens, so they are shown dimmer rather than hidden.
        $colour = if ($property.Name -like 'sonora_*') { 'White' } else { 'DarkGray' }
        Write-Host ("    {0,-28} {1}" -f $property.Name, $property.Value) -ForegroundColor $colour
    }
    Write-Host ''

    # The one mismatch worth catching before reading a trace: symbols from a
    # different build than the dump. cdb will happily load a PDB whose build id
    # does not match and then print names that belong to other functions, which
    # is worse than printing addresses.
    $version = $keys.'sonora_build'
    if ($version) {
        Write-Host "This dump is from build $version -- the PDBs must be that build's." `
            -ForegroundColor DarkYellow
        Write-Host ''
    }
}

if (-not $Commands) {
    # !analyze -v: Windows' own triage. It names the exception, the faulting
    # module and the probable cause, and for an access violation it is usually
    # the whole answer.
    # .ecxr: switches to the context record the exception was raised in. Without
    # it the "current" stack is the one the dump writer was on, which is
    # Crashpad's and is never interesting.
    # kv: the stack with frame pointers and arguments.
    # lm v m Sonora: which PDB was loaded for the main module, which is the line
    # that says whether the names in the stack above can be trusted.
    #
    # NOT .symfix and NOT .sympath, and that is the whole reason this comment is
    # long. A semicolon separates cdb commands *except* after the ones whose
    # argument is the remainder of the line -- .sympath is one of those. The first
    # version of this script began with ".symfix; .sympath+ <dir>; .reload /f;
    # !analyze -v; ..." and what cdb did with it was set the symbol path to
    #
    #     srv*;<dir>; .reload /f; !analyze -v; .ecxr; kv; lm; q
    #
    # ...and then sit at the prompt, having run no analysis at all. It looked like
    # a debugger that could not find symbols. It was a quoting bug.
    #
    # The path does not need setting here anyway: -y below carries it, and so does
    # _NT_SYMBOL_PATH. One place, and it is not inside a command string.
    $stack = if ($All) { '~*kv' } else { 'kv' }
    $Commands = ".reload /f; !analyze -v; .ecxr; $stack; lm v m Sonora; q"
}

Write-Host "cdb: $cdb" -ForegroundColor DarkGray
Write-Host "dump: $dumpPath" -ForegroundColor DarkGray
Write-Host "symbols: $symbolPath (plus Microsoft's public server)" -ForegroundColor DarkGray
Write-Host ''

# -z the dump, -y the symbol path, -c the commands, -lines for source lines when
# the PDB has them and the source is where it was built from.
#
# SRV* with a local cache and not a bare http URL: without the cache every run
# downloads ntdll's symbols again, and the second run of this script is the one
# during which somebody loses patience with it.
$cache = Join-Path $env:LOCALAPPDATA 'Sonora\symbols'
$env:_NT_SYMBOL_PATH = "$symbolPath;SRV*$cache*https://msdl.microsoft.com/download/symbols"

& $cdb -z $dumpPath -y $env:_NT_SYMBOL_PATH -lines -c $Commands
if ($LASTEXITCODE -ne 0) {
    # Not an error on its own: cdb exits with the last command's status, and `q`
    # after !analyze on a dump that analysed fine still sometimes returns
    # non-zero. Said plainly rather than swallowed.
    Write-Host "cdb exited with $LASTEXITCODE (often harmless after q)" -ForegroundColor DarkGray
}
