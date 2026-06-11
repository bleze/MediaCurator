<#
.SYNOPSIS
    Downloads and stages ffprobe and mkvmerge into tools\windows\.
    Run once after cloning, and whenever PINNED_VERSIONS changes.

.USAGE
    powershell -ExecutionPolicy Bypass -File scripts\setup_tools.ps1

    Add -Force to re-download even if the tools are already present.
#>

param([switch]$Force)

$ErrorActionPreference = "Stop"

# ── Pinned versions ────────────────────────────────────────────────────────────
#   To upgrade: bump the version + URL, delete tools\windows\, re-run.
$FFPROBE_VERSION = "7.1"
$FFPROBE_URL     = "https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-7.1-essentials_build.zip"

$MKV_VERSION     = "88.0"
$MKV_URL         = "https://mkvtoolnix.download/windows/releases/88.0/mkvtoolnix-64-bit-88.0.7z"

# ── Paths ──────────────────────────────────────────────────────────────────────
$Root       = (Resolve-Path "$PSScriptRoot\..").Path
$ToolsDir   = Join-Path $Root "tools\windows"
$FfprobeExe = Join-Path $ToolsDir "ffprobe.exe"
$MkvDir     = Join-Path $ToolsDir "mkvtoolnix"
$MkvExe     = Join-Path $MkvDir   "mkvmerge.exe"

New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null

Write-Host "MediaCurator - tool setup" -ForegroundColor Cyan
Write-Host "  Target: $ToolsDir"
Write-Host ""

# ──────────────────────────────────────────────────────────────────────────────
#  Helper: locate 7-Zip, installing via winget if absent
# ──────────────────────────────────────────────────────────────────────────────
function Get-7Zip {
    $candidates = @(
        "C:\Program Files\7-Zip\7z.exe",
        "C:\Program Files (x86)\7-Zip\7z.exe"
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) { return $found }

    Write-Host "  7-Zip not found - installing via winget..." -ForegroundColor Yellow
    winget install --id 7zip.7zip --silent --accept-source-agreements --accept-package-agreements
    if (Test-Path $candidates[0]) { return $candidates[0] }
    throw "7-Zip install failed. Install 7-Zip manually and re-run."
}

# ──────────────────────────────────────────────────────────────────────────────
#  1. ffprobe (gyan.dev LGPL essentials build, static .exe — no extra DLLs)
# ──────────────────────────────────────────────────────────────────────────────
if ((-not (Test-Path $FfprobeExe)) -or $Force) {
    Write-Host "[ffprobe $FFPROBE_VERSION]" -ForegroundColor White
    Write-Host "  Downloading from gyan.dev..."

    $zipTmp     = Join-Path $env:TEMP "ffmpeg-essentials.zip"
    $extractTmp = Join-Path $env:TEMP "ffmpeg-essentials-extract"

    Invoke-WebRequest -Uri $FFPROBE_URL -OutFile $zipTmp -UseBasicParsing

    if (Test-Path $extractTmp) { Remove-Item $extractTmp -Recurse -Force }
    Expand-Archive -Path $zipTmp -DestinationPath $extractTmp -Force

    $src = Get-ChildItem -Path $extractTmp -Filter "ffprobe.exe" -Recurse |
           Select-Object -First 1
    if (-not $src) { throw "ffprobe.exe not found inside downloaded archive." }

    Copy-Item $src.FullName $FfprobeExe -Force
    Remove-Item $zipTmp     -Force -ErrorAction SilentlyContinue
    Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "  OK" -ForegroundColor Green
} else {
    Write-Host "[ffprobe] already present  (use -Force to re-download)" -ForegroundColor DarkGray
}

# ──────────────────────────────────────────────────────────────────────────────
#  2. MKVToolNix — mkvmerge + all its runtime DLLs, isolated in mkvtoolnix\
#     mkvmerge is a Qt app; keeping its DLLs in a dedicated subdirectory
#     prevents any version clash with MediaCurator's own Qt runtime.
# ──────────────────────────────────────────────────────────────────────────────
if ((-not (Test-Path $MkvExe)) -or $Force) {
    Write-Host "[mkvtoolnix $MKV_VERSION]" -ForegroundColor White
    Write-Host "  Downloading from mkvtoolnix.download..."

    $7z         = Get-7Zip
    $7zTmp      = Join-Path $env:TEMP "mkvtoolnix.7z"
    $extractTmp = Join-Path $env:TEMP "mkvtoolnix-extract"

    Invoke-WebRequest -Uri $MKV_URL -OutFile $7zTmp -UseBasicParsing
    Write-Host "  Extracting..."

    if (Test-Path $extractTmp) { Remove-Item $extractTmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $extractTmp | Out-Null
    & $7z x $7zTmp "-o$extractTmp" -y | Out-Null

    # Archive contains a single top-level folder (e.g. mkvtoolnix-88.0\)
    $mkvRoot = Get-ChildItem $extractTmp -Directory | Select-Object -First 1
    if (-not $mkvRoot) { throw "Cannot find root folder inside MKVToolNix archive." }

    if (Test-Path $MkvDir) { Remove-Item $MkvDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $MkvDir | Out-Null

    # Copy everything except the GUI app and locale files (~30 MB saved).
    # mkvmerge.exe is a console tool; it works without them.
    Get-ChildItem $mkvRoot.FullName | Where-Object {
        $_.Name -ne "mkvtoolnix-gui.exe" -and $_.Name -ne "locale"
    } | ForEach-Object {
        Copy-Item $_.FullName -Destination $MkvDir -Recurse -Force
    }

    Remove-Item $7zTmp      -Force -ErrorAction SilentlyContinue
    Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "  OK" -ForegroundColor Green
} else {
    Write-Host "[mkvtoolnix] already present  (use -Force to re-download)" -ForegroundColor DarkGray
}

# ──────────────────────────────────────────────────────────────────────────────
#  3. Version manifest — consumed by CI and CPack to stamp the installer
# ──────────────────────────────────────────────────────────────────────────────
[ordered]@{
    ffprobe    = @{ version = $FFPROBE_VERSION; url = $FFPROBE_URL }
    mkvtoolnix = @{ version = $MKV_VERSION;     url = $MKV_URL     }
} | ConvertTo-Json -Depth 3 |
    Set-Content (Join-Path $ToolsDir "versions.json") -Encoding UTF8

# ──────────────────────────────────────────────────────────────────────────────
#  4. Final status
# ──────────────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Status:" -ForegroundColor Cyan
foreach ($check in @($FfprobeExe, $MkvExe)) {
    $label = $check.Replace($Root + "\", "")
    if (Test-Path $check) {
        Write-Host "  [OK]     $label" -ForegroundColor Green
    } else {
        Write-Host "  [MISSING] $label" -ForegroundColor Red
    }
}
Write-Host ""
Write-Host "To upgrade tools, bump the version variables at the top of this script," -ForegroundColor DarkGray
Write-Host "then re-run with -Force." -ForegroundColor DarkGray
