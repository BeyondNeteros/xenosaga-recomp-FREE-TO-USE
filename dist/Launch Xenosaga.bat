@echo off
rem Xenosaga Episode I: Recompiled (work in progress)
rem Runs the PC port against your own disc image. Edit DISC if your ISO moves.
rem Note: paths here contain parentheses, so they are only used outside ( ) blocks.
setlocal
cd /d "%~dp0"

set "DISC=<your disc image>"
set "ELF_SOURCE=<game files>\SLUS_204.69"

if exist "%DISC%" goto disc_ok
echo Disc image not found:
echo   "%DISC%"
echo Edit DISC at the top of this file.
goto end
:disc_ok

if exist "SLUS_204.69" goto elf_ok
copy /y "%ELF_SOURCE%" "SLUS_204.69" >nul
if errorlevel 1 goto elf_failed
:elf_ok

rem Builds arrive split in parts (file transfer limit); rejoin a new build once.
if not exist "XenosagaRecomp.exe.part0" goto exe_ready
echo Assembling XenosagaRecomp.exe...
copy /b /y "XenosagaRecomp.exe.part0"+"XenosagaRecomp.exe.part1"+"XenosagaRecomp.exe.part2" "XenosagaRecomp.exe" >nul
if errorlevel 1 goto assemble_failed
del "XenosagaRecomp.exe.part0" "XenosagaRecomp.exe.part1" "XenosagaRecomp.exe.part2"
:exe_ready

rem Sound is skipped until the sound driver connection is fixed.
set "XS_NO_SOUND=1"
set "XS_DISC_IMAGE=%DISC%"
rem Debug: when VU1 is the bottleneck, record 300 VU1 calls to vu1_capture.bin.
set "PS2X_VU_CAPTURE=1"

echo Starting Xenosaga Episode I: Recompiled...
echo Log: "%~dp0xenosaga.log"
"%~dp0XenosagaRecomp.exe" "%~dp0SLUS_204.69" > "%~dp0xenosaga.log" 2>&1
echo Exited with code %errorlevel%. See xenosaga.log
goto end

:elf_failed
echo Could not copy "%ELF_SOURCE%"
goto end

:assemble_failed
echo Failed to assemble XenosagaRecomp.exe
goto end

:end
pause
