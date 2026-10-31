@echo off
rem Xenosaga Episode I: Recompiled (work in progress)
rem Runs the PC port against your own disc image. Edit DISC if your ISO moves.
cd /d "%~dp0"

set "DISC=<your disc image>"
set "ELF_SOURCE=<game files>\SLUS_204.69"

if not exist "%DISC%" (
    echo Disc image not found:
    echo   %DISC%
    echo Edit DISC at the top of this file.
    pause
    exit /b 1
)
if not exist "SLUS_204.69" copy /y "%ELF_SOURCE%" "SLUS_204.69" >nul

rem Builds arrive split in parts (file transfer limit); rejoin a new build once.
if exist "XenosagaRecomp.exe.part0" (
    echo Assembling XenosagaRecomp.exe...
    copy /b /y "XenosagaRecomp.exe.part0"+"XenosagaRecomp.exe.part1"+"XenosagaRecomp.exe.part2" "XenosagaRecomp.exe" >nul
    if errorlevel 1 (
        echo Failed to assemble XenosagaRecomp.exe
        pause
        exit /b 1
    )
    del "XenosagaRecomp.exe.part0" "XenosagaRecomp.exe.part1" "XenosagaRecomp.exe.part2"
)

rem Sound is skipped until the sound driver connection is fixed.
set "XS_NO_SOUND=1"
set "XS_DISC_IMAGE=%DISC%"

echo Starting Xenosaga Episode I: Recompiled...
echo Log: %~dp0xenosaga.log
XenosagaRecomp.exe "%~dp0SLUS_204.69" > xenosaga.log 2>&1
echo Exited with code %errorlevel%. See xenosaga.log
