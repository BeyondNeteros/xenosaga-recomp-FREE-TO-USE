@echo off
setlocal
if "%~4"=="" goto usage
if /i "%~1"=="interpreter" goto interpreter
if /i "%~1"=="blocks" goto blocks
goto usage
:interpreter
set "PS2X_VU1_NATIVE=0"
goto settings
:blocks
set "PS2X_VU1_NATIVE=1"
:settings
if "%~2"=="1" goto setup
if "%~2"=="3" goto setup
goto usage
:setup
set "XS_CASE=%~3"
echo(%XS_CASE%| findstr /r /x "[A-Za-z0-9][A-Za-z0-9_-]*" >nul
if errorlevel 1 goto usage
set "XS_DISC_IMAGE=%~f4"
if not exist "%XS_DISC_IMAGE%" goto missing
if not defined XS_AB_SEED goto missing
if not defined XS_AB_STAGE goto missing
set "seed=%XS_AB_SEED%"
set "payload=%XS_AB_STAGE%"
set "settings=%CD%\ab-settings\gpu-%~2x.ini"
set "trial=%CD%\ab-runs\%XS_CASE%"
if not exist "%seed%\mc0\." goto missing
if not exist "%seed%\mc1\." goto missing
if not exist "%payload%\XenosagaRecomp.exe" goto missing
if not exist "%settings%" goto missing
set "PS2X_INPUT_SCRIPT="
set "PS2X_INPUT_RECORD="
if /i "%~5"=="manual" goto copy
if /i "%~5"=="record" goto record
if "%~5"=="" goto usage
if not exist "%~f5" goto missing
set "PS2X_INPUT_SCRIPT=%trial%\route.txt"
goto copy
:record
set "PS2X_INPUT_RECORD=%trial%\route-recorded.txt"
:copy
if exist "%trial%" goto exists
mkdir "%trial%" || exit /b 1
robocopy "%payload%" "%trial%" XenosagaRecomp.exe *.dll SLUS_204.69 SHA256SUMS "Launch Xenosaga.bat"
if errorlevel 8 exit /b 1
robocopy "%seed%\mc0" "%trial%\mc0" /E
if errorlevel 8 exit /b 1
robocopy "%seed%\mc1" "%trial%\mc1" /E
if errorlevel 8 exit /b 1
copy "%settings%" "%trial%\settings.ini" >nul || exit /b 1
if defined PS2X_INPUT_SCRIPT copy "%~f5" "%PS2X_INPUT_SCRIPT%" >nul || exit /b 1

set "XS_NO_SOUND=0"
set "XS_HEARTBEAT_SECONDS=5"
set "XS_PROFILE="
set "XS_PGO_DUMP="
set "XS_FRAME_KEEP="
set "PS2X_VU1_PROGRAMS=0"
set "PS2X_VU1_FAST="
set "PS2X_VU1_STATIC="
set "PS2X_VU1_DAZ="
set "PS2X_VU1_BULKKICK="
set "PS2X_VU1_CHECK="
set "PS2X_VU1_STATS="
set "PS2X_VU1_BLOCKLOG="
set "PS2X_VU_CAPTURE="
set "PS2X_GS_CAPTURE="
set "PS2X_VIF1_ASYNC="
set "PS2X_GS_ASYNC="
set "PS2X_GS_PACK="
set "PS2X_GIF_DIRECT="
set "PS2X_GIF_PRIORITY_SORT="
set "PS2X_GL_SHARED_PRESENT="
set "PS2X_RENDERER=gpu"
set "PS2X_UPSCALE=%~2"
> "%trial%\mode.txt" echo %~1 GPU %~2x
>> "%trial%\mode.txt" set PS2X_
pushd "%trial%" || exit /b 1
XenosagaRecomp.exe "%CD%\SLUS_204.69" > xenosaga.log 2>&1
set "trialExit=%errorlevel%"
popd
exit /b %trialExit%
:exists
echo Trial directory already exists. Choose a fresh trial name. 1>&2
exit /b 1
:missing
echo Set XS_AB_STAGE and XS_AB_SEED. Check the disc, stage, both seed slots, settings and route. 1>&2
exit /b 1
:usage
echo From the private benchmark directory use %~nx0 interpreter^|blocks 1^|3 trial-name iso-path route-path^|record^|manual 1>&2
exit /b 2
