# Xenosaga Episode I: Recompiled - launcher
#
# Builds the PC version from the player's own disc image (tools/build_from_iso.py
# under MSYS2) and starts it. Ships no game code or art: a background picture is
# used only if the player puts one at launcher/background.png (or .jpg).

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Port = Split-Path -Parent $Here
$GameDir = Join-Path $Port 'game'
$WorkDir = Join-Path $Port 'work'
$StateFile = Join-Path $Port 'launcher.ini'
$BuildLog = Join-Path $WorkDir 'build.log'
$Msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$MsysPython = Join-Path $Msys 'mingw64\bin\python.exe'
$Packages = 'mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-python git'

# --- remembered state ----------------------------------------------------------
$State = @{ iso = '' }
if (Test-Path $StateFile) {
    foreach ($line in Get-Content $StateFile) {
        if ($line -match '^\s*(\w+)\s*=\s*(.*)$') { $State[$Matches[1]] = $Matches[2].Trim() }
    }
}
function Save-State {
    $State.GetEnumerator() | ForEach-Object { "$($_.Key) = $($_.Value)" } | Set-Content -Encoding utf8 $StateFile
}

function Test-Toolchain { (Test-Path $MsysPython) -and (Test-Path (Join-Path $Msys 'mingw64\bin\g++.exe')) -and (Test-Path (Join-Path $Msys 'mingw64\bin\cmake.exe')) }
function Test-GameBuilt { Test-Path (Join-Path $GameDir 'XenosagaRecomp.exe') }

# --- window --------------------------------------------------------------------
$W = 1024; $H = 576
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Xenosaga Episode I: Recompiled'
$form.ClientSize = New-Object System.Drawing.Size($W, $H)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [System.Drawing.Color]::FromArgb(12, 16, 28)

function New-Background {
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
    $art = @('background.png', 'background.jpg') | ForEach-Object { Join-Path $Here $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($art) {
        $img = [System.Drawing.Image]::FromFile($art)
        # Cover the window, keeping the picture's shape.
        $scale = [Math]::Max($W / $img.Width, $H / $img.Height)
        $dw = $img.Width * $scale; $dh = $img.Height * $scale
        $g.DrawImage($img, [single](($W - $dw) / 2), [single](($H - $dh) / 2), [single]$dw, [single]$dh)
        $img.Dispose()
    } else {
        $rect = New-Object System.Drawing.Rectangle(0, 0, $W, $H)
        $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, [System.Drawing.Color]::FromArgb(40, 70, 120), [System.Drawing.Color]::FromArgb(10, 12, 24), 30.0)
        $g.FillRectangle($grad, $rect)
    }
    # Shade the left side for the title and the bottom for the controls.
    $left = New-Object System.Drawing.Rectangle(0, 0, [int]($W * 0.62), $H)
    $shade = New-Object System.Drawing.Drawing2D.LinearGradientBrush($left, [System.Drawing.Color]::FromArgb(200, 8, 14, 32), [System.Drawing.Color]::FromArgb(0, 8, 14, 32), 0.0)
    $g.FillRectangle($shade, $left)
    $bottom = New-Object System.Drawing.Rectangle(0, ($H - 150), $W, 150)
    $shade2 = New-Object System.Drawing.Drawing2D.LinearGradientBrush($bottom, [System.Drawing.Color]::FromArgb(0, 6, 10, 22), [System.Drawing.Color]::FromArgb(235, 6, 10, 22), 90.0)
    $g.FillRectangle($shade2, $bottom)

    # Title text with a soft shadow.
    function Draw-Text($text, $font, $color, $x, $y) {
        $shadow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(150, 0, 0, 0))
        $g.DrawString($text, $font, $shadow, [single]($x + 2), [single]($y + 3))
        $g.DrawString($text, $font, (New-Object System.Drawing.SolidBrush($color)), [single]$x, [single]$y)
    }
    $white = [System.Drawing.Color]::White
    $accent = [System.Drawing.Color]::FromArgb(150, 205, 255)
    Draw-Text 'XENOSAGA' (New-Object System.Drawing.Font('Segoe UI Light', 54)) $white 44 58
    Draw-Text 'EPISODE I' (New-Object System.Drawing.Font('Segoe UI Semibold', 20)) $accent 50 150
    Draw-Text 'Der Wille zur Macht' (New-Object System.Drawing.Font('Segoe UI', 20, [System.Drawing.FontStyle]::Italic)) $white 50 184
    $g.FillRectangle((New-Object System.Drawing.SolidBrush($accent)), 52, 236, 64, 3)
    Draw-Text 'Recompiled  -  unofficial PC port' (New-Object System.Drawing.Font('Segoe UI', 12)) $accent 50 250
    $g.Dispose()
    return $bmp
}
$form.BackgroundImage = New-Background

function New-Button($text, $x, $width, $primary = $false) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 11)
    $b.Size = New-Object System.Drawing.Size($width, 42)
    $b.Location = New-Object System.Drawing.Point($x, ($H - 64))
    $b.FlatStyle = 'Flat'
    $b.FlatAppearance.BorderSize = 1
    $b.Cursor = 'Hand'
    if ($primary) {
        $b.BackColor = [System.Drawing.Color]::FromArgb(70, 140, 230); $b.ForeColor = [System.Drawing.Color]::White
        $b.FlatAppearance.BorderColor = [System.Drawing.Color]::FromArgb(150, 205, 255)
    } else {
        $b.BackColor = [System.Drawing.Color]::FromArgb(28, 36, 58); $b.ForeColor = [System.Drawing.Color]::FromArgb(220, 230, 245)
        $b.FlatAppearance.BorderColor = [System.Drawing.Color]::FromArgb(70, 90, 130)
    }
    # Dim the button while it can't be used (flat buttons keep their colours otherwise).
    $b.Tag = @($b.BackColor, $b.ForeColor)
    $b.Add_EnabledChanged({
        if ($this.Enabled) { $this.BackColor = $this.Tag[0]; $this.ForeColor = $this.Tag[1] }
        else { $this.BackColor = [System.Drawing.Color]::FromArgb(30, 34, 44); $this.ForeColor = [System.Drawing.Color]::FromArgb(110, 118, 135) }
    })
    $form.Controls.Add($b)
    return $b
}

$status = New-Object System.Windows.Forms.Label
$status.BackColor = [System.Drawing.Color]::Transparent
$status.ForeColor = [System.Drawing.Color]::White
$status.Font = New-Object System.Drawing.Font('Segoe UI', 11)
$status.Location = New-Object System.Drawing.Point(44, ($H - 128))
$status.Size = New-Object System.Drawing.Size(($W - 88), 26)
$form.Controls.Add($status)

$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(44, ($H - 98))
$progress.Size = New-Object System.Drawing.Size(($W - 88), 8)
$progress.Maximum = 60
$progress.Visible = $false
$form.Controls.Add($progress)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true; $log.ReadOnly = $true; $log.ScrollBars = 'Vertical'
$log.BackColor = [System.Drawing.Color]::FromArgb(10, 12, 20); $log.ForeColor = [System.Drawing.Color]::FromArgb(180, 200, 220)
$log.Font = New-Object System.Drawing.Font('Consolas', 9)
$log.Location = New-Object System.Drawing.Point(44, 40)
$log.Size = New-Object System.Drawing.Size(($W - 88), ($H - 190))
$log.Visible = $false
$form.Controls.Add($log)

$btnDisc = New-Button 'Choose disc image...' 44 200
$btnTools = New-Button 'Install build tools' 256 190
$btnBuild = New-Button 'Build the game' 458 170 $true
$btnPlay = New-Button 'PLAY' 458 170 $true
$btnOptimize = New-Button 'Optimize' 640 110
$btnSettings = New-Button 'Settings' 762 100
$btnDetails = New-Button 'Details' 874 106
$btnPlay.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 13)

$script:busy = $false
$script:job = $null
$script:logPos = 0
$script:game = $null

function Set-Status($text) { $status.Text = $text }

function Update-Ui {
    $hasIso = $State.iso -and (Test-Path $State.iso)
    $tools = Test-Toolchain
    $built = Test-GameBuilt
    $btnDisc.Enabled = -not $script:busy
    $btnTools.Visible = -not $tools
    $btnTools.Enabled = -not $script:busy
    $btnBuild.Visible = -not $built
    $btnBuild.Enabled = (-not $script:busy) -and $hasIso -and $tools
    $btnPlay.Visible = $built
    $btnPlay.Enabled = (-not $script:busy) -and $hasIso
    $vbl = Join-Path $GameDir 'vu1_blocks.vbl'
    $btnOptimize.Visible = $built
    $btnOptimize.Enabled = (-not $script:busy) -and (Test-Path $vbl) -and $tools
    $btnSettings.Enabled = Test-Path (Join-Path $GameDir 'settings.ini')
    if ($script:busy) { return }
    if (-not $hasIso) { Set-Status 'Choose your own Xenosaga Episode I (USA) disc image to begin.' }
    elseif (-not $tools) { Set-Status 'Disc ready. Next: install the free build tools (MSYS2), about 1.5 GB.' }
    elseif (-not $built) { Set-Status 'Ready to build the game from your disc. This takes 15-30 minutes, once.' }
    else { Set-Status ('Disc: ' + [IO.Path]::GetFileName($State.iso)) }
}

# --- ISO check (before any tools are installed) ----------------------------------
function Test-Disc($path) {
    try {
        $fs = [IO.File]::OpenRead($path)
        $buf = New-Object byte[] 2048
        $fs.Position = 16 * 2048; [void]$fs.Read($buf, 0, 2048)
        if ([Text.Encoding]::ASCII.GetString($buf, 1, 5) -ne 'CD001') { return 'This file is not a disc image.' }
        $extent = [BitConverter]::ToUInt32($buf, 156 + 2); $size = [BitConverter]::ToUInt32($buf, 156 + 10)
        $dir = New-Object byte[] $size
        $fs.Position = [int64]$extent * 2048; [void]$fs.Read($dir, 0, $size)
        $pos = 0; $found = $null
        while ($pos -lt $size) {
            $len = $dir[$pos]
            if ($len -eq 0) { $pos = ([Math]::Floor($pos / 2048) + 1) * 2048; continue }
            $name = [Text.Encoding]::ASCII.GetString($dir, $pos + 33, $dir[$pos + 32]).Split(';')[0]
            if ($name -eq 'SLUS_204.69') { $found = @([BitConverter]::ToUInt32($dir, $pos + 2), [BitConverter]::ToUInt32($dir, $pos + 10)) }
            $pos += $len
        }
        if (-not $found) { return 'This is not Xenosaga Episode I (USA): SLUS_204.69 is not on the disc.' }
        $elf = New-Object byte[] $found[1]
        $fs.Position = [int64]$found[0] * 2048; [void]$fs.Read($elf, 0, $found[1])
        $sha = [BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash($elf)).Replace('-', '').ToLower()
        if ($sha -ne 'fd206d5715a322830f7fa9285fb4a09276ac2a63') { return 'This copy does not match the USA release (SLUS-20469), or it is damaged.' }
        return $null
    } catch { return "Could not read the file: $($_.Exception.Message)" }
    finally { if ($fs) { $fs.Close() } }
}

# --- background jobs (output goes to a log file the timer follows) ----------------
function Start-Job2($title, $exe, $arguments) {
    New-Item -ItemType Directory -Force $WorkDir | Out-Null
    Set-Content -Encoding utf8 $BuildLog ''
    $script:logPos = 0
    $log.Clear()
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'cmd.exe'
    $psi.Arguments = "/c `"`"$exe`" $arguments >> `"$BuildLog`" 2>&1`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['PATH'] = "$Msys\mingw64\bin;$Msys\usr\bin;" + $env:PATH
    $psi.EnvironmentVariables['PYTHONUNBUFFERED'] = '1'
    $psi.EnvironmentVariables['MSYS2_ROOT'] = $Msys
    $script:job = [System.Diagnostics.Process]::Start($psi)
    $script:busy = $true
    $progress.Visible = $true; $progress.Value = 0
    Set-Status $title
    Update-Ui
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    if ($script:job) {
        if (Test-Path $BuildLog) {
            $fs = [IO.File]::Open($BuildLog, 'Open', 'Read', 'ReadWrite')
            if ($fs.Length -gt $script:logPos) {
                $fs.Position = $script:logPos
                $reader = New-Object IO.StreamReader($fs)
                $text = $reader.ReadToEnd()
                $script:logPos = $fs.Length
                $log.AppendText($text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
                foreach ($line in $text -split "`n") {
                    if ($line -match '^@@STEP (\d+)/(\d+) (.*)') {
                        $progress.Maximum = [int]$Matches[2] * 10; $progress.Value = ([int]$Matches[1] - 1) * 10
                        Set-Status "Step $($Matches[1]) of $($Matches[2]): $($Matches[3].Trim())"
                    } elseif ($line -match '^@@ERROR (.*)') { Set-Status ('Problem: ' + $Matches[1].Trim()) }
                    elseif ($line -match '^\[(\d+)/(\d+)\]') {
                        # compiler progress inside a step: move the bar within the step
                        $base = $progress.Value - ($progress.Value % 10)
                        $progress.Value = [Math]::Min($progress.Maximum, $base + [int][Math]::Floor(9 * [int]$Matches[1] / [Math]::Max(1, [int]$Matches[2])))
                    }
                }
            }
            $fs.Close()
        }
        if ($script:job.HasExited) {
            $code = $script:job.ExitCode
            $script:job = $null; $script:busy = $false; $progress.Visible = $false
            Update-Ui
            if ($code -eq 0) { Set-Status 'Done.'; Update-Ui }
            else {
                $last = (Get-Content $BuildLog | Where-Object { $_ -match '^@@ERROR' } | Select-Object -Last 1) -replace '^@@ERROR ', ''
                if (-not $last) { $last = 'see Details for the full log' }
                Set-Status "Something went wrong: $last"
                $log.Visible = $true
            }
        }
    }
    if ($script:game -and $script:game.HasExited) {
        $script:game = $null
        $form.Show(); $form.Activate()
        Update-Ui
    }
})
$timer.Start()

# --- buttons -------------------------------------------------------------------
$btnDisc.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Your Xenosaga Episode I (USA) disc image'
    $dlg.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'
    if ($dlg.ShowDialog($form) -ne 'OK') { return }
    Set-Status 'Checking the disc...'
    $form.Refresh()
    $problem = Test-Disc $dlg.FileName
    if ($problem) { Set-Status $problem; return }
    $State.iso = $dlg.FileName; Save-State
    Update-Ui
})

$btnTools.Add_Click({
    $msg = "To build the game, the launcher installs MSYS2, a free set of programming tools (compiler, CMake, Python, FFmpeg), into $Msys.`n`nIt downloads about 1.5 GB and uses Windows' winget. Windows may ask for permission. Continue?"
    if ([System.Windows.Forms.MessageBox]::Show($form, $msg, 'Install build tools', 'YesNo', 'Question') -ne 'Yes') { return }
    $script_ = Join-Path $WorkDir 'install_tools.cmd'
    New-Item -ItemType Directory -Force $WorkDir | Out-Null
    @(
        '@echo off',
        "if not exist `"$Msys\usr\bin\bash.exe`" winget install --id MSYS2.MSYS2 -e --silent --accept-source-agreements --accept-package-agreements",
        "if not exist `"$Msys\usr\bin\bash.exe`" (echo @@ERROR MSYS2 did not install & exit /b 1)",
        "`"$Msys\usr\bin\bash.exe`" -lc `"pacman -Syuu --noconfirm`"",
        "`"$Msys\usr\bin\bash.exe`" -lc `"pacman -Syuu --noconfirm`"",
        "`"$Msys\usr\bin\bash.exe`" -lc `"pacman -S --needed --noconfirm $Packages`" || (echo @@ERROR package install failed & exit /b 1)",
        'echo @@DONE'
    ) | Set-Content -Encoding ascii $script_
    Start-Job2 'Installing the build tools (this can take a while)...' $script_ ''
})

$btnBuild.Add_Click({
    Start-Job2 'Building the game from your disc...' $MsysPython "-u `"$Port\tools\build_from_iso.py`" --iso `"$($State.iso)`""
})

$btnOptimize.Add_Click({
    $msg = "While you play, the game notes the graphics routines it runs. Optimize builds fast versions of them into the game (a few minutes), which helps the frame rate.`n`nContinue?"
    if ([System.Windows.Forms.MessageBox]::Show($form, $msg, 'Optimize', 'YesNo', 'Question') -ne 'Yes') { return }
    Start-Job2 'Optimizing the game...' $MsysPython "-u `"$Port\tools\build_from_iso.py`" --optimize"
})

$btnPlay.Add_Click({
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'cmd.exe'
    $exe = Join-Path $GameDir 'XenosagaRecomp.exe'
    $elf = Join-Path $GameDir 'SLUS_204.69'
    # start /wait: the game is a windowed program, so cmd would not wait for it otherwise.
    $psi.Arguments = "/c start `"`" /wait /b `"$exe`" `"$elf`" > `"$GameDir\xenosaga.log`" 2>&1"
    $psi.WorkingDirectory = $GameDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['XS_DISC_IMAGE'] = $State.iso
    $psi.EnvironmentVariables['XS_NO_SOUND'] = '0'
    $psi.EnvironmentVariables['PS2X_VU1_BLOCKLOG'] = (Join-Path $GameDir 'vu1_blocks.vbl')
    $script:game = [System.Diagnostics.Process]::Start($psi)
    $form.Hide()
})

$btnSettings.Add_Click({ Start-Process notepad.exe (Join-Path $GameDir 'settings.ini') })
$btnDetails.Add_Click({ $log.Visible = -not $log.Visible })

$form.Add_FormClosing({
    if ($script:job -and -not $script:job.HasExited) {
        $r = [System.Windows.Forms.MessageBox]::Show($form, 'A build is still running. Stop it and close? (It continues from where it stopped next time.)', 'Close', 'YesNo', 'Warning')
        if ($r -ne 'Yes') { $_.Cancel = $true; return }
        & taskkill /T /F /PID $script:job.Id | Out-Null
    }
})

Update-Ui
# XS_LAUNCHER_SNAPSHOT=<file.png>: save a picture of the window and close (docs/tests).
if ($env:XS_LAUNCHER_SNAPSHOT) {
    $form.Add_Shown({
        $form.Refresh()
        $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save($env:XS_LAUNCHER_SNAPSHOT, [System.Drawing.Imaging.ImageFormat]::Png)
        $form.Close()
    })
}
[void]$form.ShowDialog()
