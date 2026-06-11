<#
.SYNOPSIS
    Converts app_icon.svg -> app_icon.ico.
    Strategy: one Inkscape call at 256 px, then System.Drawing scales to all
    required sizes in-process.  No ImageMagick, no repeated Inkscape spawning.

.USAGE
    .\scripts\export_icon.ps1
    .\scripts\export_icon.ps1 -Inkscape "C:\Program Files\Inkscape\bin\inkscape.exe"
#>

param(
    [string]$SvgPath  = "$PSScriptRoot\..\src\resources\icons\app_icon.svg",
    [string]$OutIco   = "$PSScriptRoot\..\src\resources\icons\app_icon.ico",
    [string]$Inkscape = "inkscape"
)

$SvgPath = (Resolve-Path $SvgPath).Path
$OutIco  = [System.IO.Path]::GetFullPath($OutIco)
$sizes   = @(16, 24, 32, 48, 64, 128, 256)

# ── 1. Export SVG at 256 px ────────────────────────────────────────────────────
$src256 = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($SvgPath), "icon_src256_tmp.png")
Write-Host "Rendering SVG at 256 px via Inkscape..."
$inkArgs = @($SvgPath, "--export-type=png", "--export-filename=$src256", "--export-width=256", "--export-height=256")
& $Inkscape @inkArgs

if (-not (Test-Path $src256)) {
    Write-Error "Inkscape failed. Make sure '$Inkscape' is correct and Inkscape 1.x is installed."
    exit 1
}
Write-Host "  OK -> $src256"

# ── 2. Scale to all target sizes using System.Drawing ─────────────────────────
Add-Type -AssemblyName System.Drawing

$srcBitmap = [System.Drawing.Image]::FromFile($src256)

$pngDatas = [System.Collections.Generic.List[byte[]]]::new()

foreach ($sz in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.DrawImage($srcBitmap, 0, 0, $sz, $sz)
    $g.Dispose()

    $pms = New-Object System.IO.MemoryStream
    $bmp.Save($pms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngDatas.Add($pms.ToArray())
    $pms.Close()
    $bmp.Dispose()
    Write-Host "  Scaled ${sz}x${sz}"
}

$srcBitmap.Dispose()
Remove-Item $src256 -ErrorAction SilentlyContinue

# ── 3. Write ICO (PNG blobs inside — supported Vista+) ────────────────────────
Write-Host "Building ICO..."

$count      = $sizes.Count
$dataOffset = 6 + $count * 16

$oms = New-Object System.IO.MemoryStream
$bw  = New-Object System.IO.BinaryWriter($oms)

# ICONDIR
$bw.Write([uint16]0)        # reserved
$bw.Write([uint16]1)        # type = ICO
$bw.Write([uint16]$count)

# Directory entries
$offset = [uint32]$dataOffset
for ($i = 0; $i -lt $count; $i++) {
    $sz   = $sizes[$i]
    $data = $pngDatas[$i]
    $bw.Write([byte]$(if ($sz -ge 256) { 0 } else { $sz }))  # 0 means 256
    $bw.Write([byte]$(if ($sz -ge 256) { 0 } else { $sz }))
    $bw.Write([byte]0)       # colorCount
    $bw.Write([byte]0)       # reserved
    $bw.Write([uint16]1)     # planes
    $bw.Write([uint16]32)    # bitCount
    $bw.Write([uint32]$data.Length)
    $bw.Write($offset)
    $offset += [uint32]$data.Length
}

# Image data
foreach ($data in $pngDatas) {
    $bw.Write($data, 0, $data.Length)
}

$bw.Flush()
[System.IO.File]::WriteAllBytes($OutIco, $oms.ToArray())
$bw.Close()
$oms.Close()

$bytes = (Get-Item $OutIco).Length
Write-Host "Done -> $OutIco  ($bytes bytes)"
