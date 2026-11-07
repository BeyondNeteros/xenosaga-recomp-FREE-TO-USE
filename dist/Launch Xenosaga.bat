@echo off
rem Xenosaga Episode I: Recompiled (work in progress)
rem Runs the PC port against your own disc image. Edit DISC if your ISO moves.
rem Display, sound and controls are set in settings.ini (made on first start).
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

rem Sound runs on the game's own sound driver and the emulated SPU2.
rem Set XS_NO_SOUND=1 to skip the sound driver if a scene hangs on it.
set "XS_NO_SOUND=0"
set "XS_DISC_IMAGE=%DISC%"
rem VU1 code blocks without a native version yet are listed in vu1_blocks.vbl,
rem so the next build can include them (tools/vu1gen.py). Small and cheap.
set "PS2X_VU1_BLOCKLOG=%~dp0vu1_blocks.vbl"
rem Every session's controller input is saved (inputs-last.txt; the one
rem before is inputs-prev.txt) so a problem can be replayed exactly.
rem The log of the session before is kept as xenosaga-prev.log.
if exist "%~dp0inputs-last.txt" move /y "%~dp0inputs-last.txt" "%~dp0inputs-prev.txt" >nul
if exist "%~dp0xenosaga.log" move /y "%~dp0xenosaga.log" "%~dp0xenosaga-prev.log" >nul
set "PS2X_INPUT_RECORD=%~dp0inputs-last.txt"

rem Debug recorders (off for normal play; they cost speed). Remove "rem" to use:
rem   VU1 capture when VU1 is the bottleneck (vu1_capture.bin):
rem set "PS2X_VU_CAPTURE=1"
rem   GS texture upload log:
rem set "PS2X_GS_TRXLOG=1"
rem   The game's own debug messages:
rem set "PS2X_GUEST_PRINTF=1"

echo Starting Xenosaga Episode I: Recompiled...
echo Log: "%~dp0xenosaga.log"
echo Keys: F11 fullscreen, F3 fps, F4 filter, F5 picture shape, F12 screenshot
"%~dp0XenosagaRecomp.exe" "%~dp0SLUS_204.69" > "%~dp0xenosaga.log" 2>&1
if errorlevel 1 goto crashed
goto quit

:crashed
echo The game exited with code %errorlevel%. See xenosaga.log
goto end

:elf_failed
echo Could not copy "%ELF_SOURCE%"
goto end

:end
pause
:quit
