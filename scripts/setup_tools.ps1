<#
.SYNOPSIS
    Downloads and stages ffprobe, mkvmerge and mkvpropedit into tools\windows\.
    Run once after cloning, and whenever PINNED_VERSIONS changes.

.USAGE
    powershell -ExecutionPolicy Bypass -File scripts\setup_tools.ps1

    Add -Force to re-download even if the tools are already present.
#>

param([switch]$Force)

$ErrorActionPreference = "Stop"

# ── Pinned versions ────────────────────────────────────────────────────────────
#   To upgrade: bump the version + URLs, delete tools\windows\, re-run.
#   FFPROBE_URLS: gyan.dev rolling "release" first; GitHub mirror is the same
#   essentials build pinned to FFPROBE_VERSION (fallback when gyan.dev 503s).
$FFPROBE_VERSION = "8.1.2"
$FFPROBE_URLS    = @(
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
    "https://github.com/GyanD/codexffmpeg/releases/download/$FFPROBE_VERSION/ffmpeg-$FFPROBE_VERSION-essentials_build.zip"
)

$MKV_VERSION     = "100.0"
$MKV_URL         = "https://mkvtoolnix.download/windows/releases/100.0/mkvtoolnix-64-bit-100.0.7z"

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
#  Helper: download with retries and URL fallbacks (gyan.dev often 503s in CI)
# ──────────────────────────────────────────────────────────────────────────────
function Invoke-DownloadFile {
    param(
        [string[]]$Urls,
        [string]$OutFile,
        [int]$MaxRetries = 3,
        [int]$RetryDelaySec = 10
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($url in $Urls) {
        for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
            try {
                Write-Host "  Downloading from $url (attempt $attempt/$MaxRetries)..."
                Invoke-WebRequest -Uri $url -OutFile $OutFile -UseBasicParsing
                return $url
            } catch {
                $msg = "$url attempt ${attempt}: $($_.Exception.Message)"
                $errors.Add($msg) | Out-Null
                Write-Host "  Failed: $($_.Exception.Message)" -ForegroundColor Yellow
                if ($attempt -lt $MaxRetries) {
                    Write-Host "  Retrying in ${RetryDelaySec}s..." -ForegroundColor DarkYellow
                    Start-Sleep -Seconds $RetryDelaySec
                }
            }
        }
    }

    throw "All download attempts failed:`n$($errors -join [Environment]::NewLine)"
}

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

    $zipTmp     = Join-Path $env:TEMP "ffmpeg-essentials.zip"
    $extractTmp = Join-Path $env:TEMP "ffmpeg-essentials-extract"

    $FFPROBE_URL = Invoke-DownloadFile -Urls $FFPROBE_URLS -OutFile $zipTmp

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

    $MKV_USED_URL = Invoke-DownloadFile -Urls @($MKV_URL) -OutFile $7zTmp
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
if (-not $FFPROBE_URL) { $FFPROBE_URL = $FFPROBE_URLS[0] }

[ordered]@{
    ffprobe    = @{ version = $FFPROBE_VERSION; url = $FFPROBE_URL; fallbacks = $FFPROBE_URLS }
    mkvtoolnix = @{ version = $MKV_VERSION;     url = $(if ($MKV_USED_URL) { $MKV_USED_URL } else { $MKV_URL }) }
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
