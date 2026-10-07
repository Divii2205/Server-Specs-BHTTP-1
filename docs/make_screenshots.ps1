# make_screenshots.ps1
#
# Renders a text file (a captured terminal session) into a PNG that looks
# like a terminal window, so it can be embedded in the report.
#
# Run from the project folder:
#     powershell -ExecutionPolicy Bypass -File docs\make_screenshots.ps1

Add-Type -AssemblyName System.Drawing

function New-TerminalImage {
    param(
        [string]$Text,
        [string]$Title,
        [string]$OutFile
    )

    $lines = $Text -split "`r?`n"

    $font      = New-Object System.Drawing.Font('Consolas', 13)
    $titleFont = New-Object System.Drawing.Font('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)

    # Measure with a throwaway bitmap so we know how big the real one must be.
    $probe = New-Object System.Drawing.Bitmap 1, 1
    $pg    = [System.Drawing.Graphics]::FromImage($probe)
    $ch    = [int][math]::Ceiling($pg.MeasureString('M', $font).Height) - 3
    $cw    = $pg.MeasureString('MMMMMMMMMM', $font).Width / 10

    $maxLen = 1
    foreach ($l in $lines) { if ($l.Length -gt $maxLen) { $maxLen = $l.Length } }
    $pg.Dispose(); $probe.Dispose()

    $pad    = 16
    $bar    = 34
    $width  = [int]($maxLen * $cw + $pad * 2 + 10)
    $height = [int]($lines.Count * $ch + $pad * 2 + $bar)
    if ($width  -lt 520)  { $width  = 520 }
    if ($width  -gt 1500) { $width  = 1500 }

    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    $bgBody  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(13, 17, 23))
    $bgBar   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(28, 33, 40))
    $fgText  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(201, 209, 217))
    $fgTitle = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 170, 182))
    $fgHex   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(126, 231, 135))
    $fgSend  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(121, 192, 255))
    $fgWarn  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 166, 87))
    $fgHead  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(210, 168, 255))

    $g.FillRectangle($bgBody, 0, 0, $width, $height)
    $g.FillRectangle($bgBar,  0, 0, $width, $bar)

    # the three little window buttons
    $dots = @(
        @{ x = 14; c = [System.Drawing.Color]::FromArgb(255, 95, 86) },
        @{ x = 34; c = [System.Drawing.Color]::FromArgb(255, 189, 46) },
        @{ x = 54; c = [System.Drawing.Color]::FromArgb(39, 201, 63) }
    )
    foreach ($d in $dots) {
        $b = New-Object System.Drawing.SolidBrush $d.c
        $g.FillEllipse($b, $d.x, 12, 11, 11)
        $b.Dispose()
    }
    $g.DrawString($Title, $titleFont, $fgTitle, 78, 8)

    $y = $bar + $pad
    foreach ($line in $lines) {
        $brush = $fgText
        if     ($line -match '^\s{2}[0-9a-f]{4}\s')  { $brush = $fgHex }
        elseif ($line -match '^(C>|S>)')             { $brush = $fgSend }
        elseif ($line -match '^(C<|S<)')             { $brush = $fgHead }
        elseif ($line -match '^={5,}| TEST |unknown|400|404|\[exit code') { $brush = $fgWarn }
        $g.DrawString($line, $font, $brush, $pad, $y)
        $y += $ch
    }

    $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "wrote $OutFile"
}

# ---------------------------------------------------------------------
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$shots = Join-Path $here 'screenshots'
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Path $shots | Out-Null }

$jobs = @(
    @{ src = 'build.txt';        title = 'Building the project';                 out = '01-build.png' },
    @{ src = 'server_log.txt';   title = 'bserve - server log';                  out = '02-server.png' },
    @{ src = 'demo_out.txt';     title = 'run_demo.bat - all six tests';         out = '03-tests.png' },
    @{ src = 'hex_frames.txt';   title = 'bcurl -v - every frame, hexdumped';    out = '04-verbose.png' },
    @{ src = 'hex_unknown.txt';  title = 'bcurl -v --send-unknown - the skip rule'; out = '05-unknown.png' }
)

foreach ($j in $jobs) {
    $p = Join-Path $here $j.src
    if (Test-Path $p) {
        $t = (Get-Content $p -Raw)
        if ($null -eq $t) { $t = '' }
        New-TerminalImage -Text $t.TrimEnd() -Title $j.title -OutFile (Join-Path $shots $j.out)
    } else {
        Write-Host "skipped (missing): $($j.src)"
    }
}
