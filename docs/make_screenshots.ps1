# make_screenshots.ps1
#
# Renders a text file (a captured terminal session) into a PNG that looks
# like a Windows Terminal / PowerShell window, so it can be embedded in the
# report. The command that produced the output is drawn at a PS prompt on
# the first line.
#
# Run from the project folder:
#     powershell -ExecutionPolicy Bypass -File docs\make_screenshots.ps1

Add-Type -AssemblyName System.Drawing

$Prompt = 'PS D:\Server Specs NA>'

function New-TerminalImage {
    param(
        [string]$Text,
        [string]$Title,
        [string]$Command,
        [string]$OutFile
    )

    # The command goes first, typed at the prompt, the way it looked on screen.
    $lines = @("$Prompt $Command") + ($Text -split "`r?`n")

    $font      = New-Object System.Drawing.Font('Consolas', 13)
    $titleFont = New-Object System.Drawing.Font('Segoe UI', 10)
    $iconFont  = New-Object System.Drawing.Font('Consolas', 7, [System.Drawing.FontStyle]::Bold)

    # Measure with a throwaway bitmap so we know how big the real one must be.
    $probe = New-Object System.Drawing.Bitmap 1, 1
    $pg    = [System.Drawing.Graphics]::FromImage($probe)
    $ch    = [int][math]::Ceiling($pg.MeasureString('M', $font).Height) - 3
    $cw    = $pg.MeasureString('MMMMMMMMMM', $font).Width / 10

    $maxLen = 1
    foreach ($l in $lines) { if ($l.Length -gt $maxLen) { $maxLen = $l.Length } }
    $pg.Dispose(); $probe.Dispose()

    $pad    = 16
    $bar    = 36
    $width  = [int]($maxLen * $cw + $pad * 2 + 10)
    $height = [int]($lines.Count * $ch + $pad * 2 + $bar)
    if ($width  -lt 640)  { $width  = 640 }
    if ($width  -gt 1500) { $width  = 1500 }

    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    # Windows Terminal "Campbell" colours
    $bgBody  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(12, 12, 12))
    $bgBar   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(32, 32, 32))
    $fgText  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(204, 204, 204))
    $fgTitle = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 230, 230))
    $fgCmd   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(249, 241, 165))
    $fgHex   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(22, 198, 12))
    $fgSend  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(97, 156, 255))
    $fgWarn  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(193, 156, 0))
    $fgHead  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(214, 112, 214))
    $psBlue  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(1, 36, 86))
    $pen     = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 230, 230)), 1

    $g.FillRectangle($bgBody, 0, 0, $width, $height)
    $g.FillRectangle($bgBar,  0, 0, $width, $bar)

    # the tab: a small PowerShell icon and the window title
    $tabW = [int]($g.MeasureString($Title, $titleFont).Width) + 56
    $g.FillRectangle($bgBody, 8, 6, $tabW, $bar - 6)
    $g.FillRectangle($psBlue, 18, 14, 14, 12)
    $g.DrawString('>', $iconFont, $fgTitle, 19, 14)
    $g.DrawString($Title, $titleFont, $fgTitle, 40, 11)

    # minimise, maximise, close - right-hand side, as on Windows
    $cy = [int]($bar / 2)
    $x  = $width - 132
    $g.DrawLine($pen, $x, $cy, $x + 10, $cy)
    $x += 46
    $g.DrawRectangle($pen, $x, $cy - 5, 10, 10)
    $x += 46
    $g.DrawLine($pen, $x, $cy - 5, $x + 10, $cy + 5)
    $g.DrawLine($pen, $x, $cy + 5, $x + 10, $cy - 5)

    $y = $bar + $pad
    $first = $true
    foreach ($line in $lines) {
        $brush = $fgText
        if     ($first)                              { $brush = $fgCmd }
        elseif ($line -match '^\s{2}[0-9a-f]{4}\s')  { $brush = $fgHex }
        elseif ($line -match '^(C>|S>)')             { $brush = $fgSend }
        elseif ($line -match '^(C<|S<)')             { $brush = $fgHead }
        elseif ($line -match '^={5,}| TEST |unknown|400|404|\[exit code') { $brush = $fgWarn }
        $g.DrawString($line, $font, $brush, $pad, $y)
        $y += $ch
        $first = $false
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
    @{ src = 'build.txt';       cmd = '.\build.bat';                                               out = '01-build.png' },
    @{ src = 'server_log.txt';  cmd = 'bin\bserve.exe www 9000';                                   out = '02-server.png' },
    @{ src = 'demo_out.txt';    cmd = '.\run_demo.bat';                                            out = '03-tests.png' },
    @{ src = 'hex_frames.txt';  cmd = 'bin\bcurl.exe -v localhost:9000/notes.txt';                 out = '04-verbose.png' },
    @{ src = 'hex_unknown.txt'; cmd = 'bin\bcurl.exe -v --send-unknown localhost:9000/notes.txt';  out = '05-unknown.png' },
    @{ src = 'hex_body.txt';    cmd = 'bin\bcurl.exe localhost:9000/notes.txt';                    out = '06-fetch.png' }
)

foreach ($j in $jobs) {
    $p = Join-Path $here $j.src
    if (Test-Path $p) {
        $t = (Get-Content $p -Raw)
        if ($null -eq $t) { $t = '' }
        New-TerminalImage -Text $t.TrimEnd() -Title 'Windows PowerShell' -Command $j.cmd -OutFile (Join-Path $shots $j.out)
    } else {
        Write-Host "skipped (missing): $($j.src)"
    }
}
