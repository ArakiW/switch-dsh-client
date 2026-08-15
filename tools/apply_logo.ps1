# Use a custom artwork as the app logo.
# Usage: powershell -File tools\apply_logo.ps1 [path-to-image]
# Outputs: icon.jpg (256x256 full-bleed JPEG for hbmenu) and
#          assets/logo.png (256x256 PNG with rounded transparent corners).
# NOTE: keep this file pure ASCII (PS 5.1 + no-BOM UTF-8 issue).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$proj = Split-Path -Parent $PSScriptRoot
$srcPath = if ($args.Count -gt 0) { $args[0] } else { 'C:\Users\USER\Desktop\icon-source.jpg' }
$src = [System.Drawing.Image]::FromFile($srcPath)

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90)
    $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    return $p
}

# icon.jpg: full-bleed 256x256
$icon = New-Object System.Drawing.Bitmap(256, 256, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g1 = [System.Drawing.Graphics]::FromImage($icon)
$g1.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g1.DrawImage($src, 0, 0, 256, 256)
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, 92)
$icon.Save((Join-Path $proj 'icon.jpg'), $codec, $ep)
$g1.Dispose()
$icon.Dispose()

# assets/logo.png: rounded transparent corners
$logo = New-Object System.Drawing.Bitmap(256, 256, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g2 = [System.Drawing.Graphics]::FromImage($logo)
$g2.Clear([System.Drawing.Color]::Transparent)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$clip = New-RoundRect 0 0 256 256 48
$g2.SetClip($clip)
$g2.DrawImage($src, 0, 0, 256, 256)
$g2.ResetClip()
$logo.Save((Join-Path $proj 'assets\logo.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$g2.Dispose()
$logo.Dispose()
$src.Dispose()

Get-Item (Join-Path $proj 'icon.jpg'), (Join-Path $proj 'assets\logo.png') | Select-Object Name, Length
