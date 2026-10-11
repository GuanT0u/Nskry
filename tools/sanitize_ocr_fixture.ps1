# Lossless fixture-data sanitization: retain geometry and regression pixels.
# Run from any directory. The original stays in ignored docs/DebugSamples/.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixturePath = Join-Path $repoRoot 'tests/ocr/fixtures/explorer_dark_zh.png'
$regionsPath = Join-Path $repoRoot 'tests/ocr/fixtures/explorer_dark_zh.regions.json'
$backupPath = Join-Path $repoRoot 'docs/DebugSamples/explorer_dark_zh_original_private.png'
$regions = Get-Content -LiteralPath $regionsPath -Raw -Encoding UTF8 | ConvertFrom-Json

$stream = [System.IO.MemoryStream]::new([System.IO.File]::ReadAllBytes($fixturePath))
$original = $null
$sanitized = $null
$graphics = $null
$brush = $null
try {
    $original = [System.Drawing.Bitmap]::new($stream)
    if ($original.Width -ne 1408 -or $original.Height -ne 793) {
        throw 'Unexpected fixture geometry; review masks before sanitizing.'
    }
    # A new bitmap carries pixels only, not source image metadata.
    $sanitized = [System.Drawing.Bitmap]::new(1408, 793, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($sanitized)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.DrawImageUnscaled($original, 0, 0)
    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 32, 32, 32))
    # Sidebar (personal shortcuts/drives), breadcrumb stem, timestamps/sizes.
    $masks = @(
        [System.Drawing.Rectangle]::new(0, 170, 225, 623),
        [System.Drawing.Rectangle]::new(416, 60, 331, 40),
        [System.Drawing.Rectangle]::new(372, 204, 358, 562),
        [System.Drawing.Rectangle]::new(829, 204, 200, 562)
    )
    foreach ($mask in $masks) { $graphics.FillRectangle($brush, $mask) }
    $graphics.Dispose(); $graphics = $null
    # Reject any mask that changes a single pixel in the five acceptance ROIs.
    foreach ($region in $regions) {
        $rect = $region.rect
        for ($y = [int]$rect[1]; $y -lt [int]$rect[3]; ++$y) {
            for ($x = [int]$rect[0]; $x -lt [int]$rect[2]; ++$x) {
                if ($original.GetPixel($x, $y).ToArgb() -ne $sanitized.GetPixel($x, $y).ToArgb()) {
                    throw "Sanitization changed regression pixels: $($region.name)"
                }
            }
        }
    }
    if (-not (Test-Path -LiteralPath $backupPath)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $backupPath) -Force | Out-Null
        Copy-Item -LiteralPath $fixturePath -Destination $backupPath
    }
    $original.Dispose(); $original = $null
    $stream.Dispose()
    $sanitized.Save($fixturePath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output 'Sanitized fixture metadata and private areas; all five acceptance ROIs are pixel-identical.'
} finally {
    if ($graphics) { $graphics.Dispose() }
    if ($brush) { $brush.Dispose() }
    if ($sanitized) { $sanitized.Dispose() }
    if ($original) { $original.Dispose() }
    $stream.Dispose()
}
