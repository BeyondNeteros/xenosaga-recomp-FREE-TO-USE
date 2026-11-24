@echo off
setlocal
if "%~2"=="" goto usage
if not "%~3"=="" if not "%~3"=="blocks" if not "%~3"=="programs" goto usage
set "GATE=%~3"
if not defined GATE set "GATE=programs"
if not exist "%~dp0vu1_replay.exe" goto missing
if not exist "%~dp0vu1_validate.py" goto missing
py -3 -c "import sys" >nul 2>&1
if errorlevel 1 goto python
py -3 "%~dp0vu1_validate.py" run "%~dp0vu1_replay.exe" "%~f1" "%~f2" --gate "%GATE%"
exit /b %errorlevel%
:python
python.exe "%~dp0vu1_validate.py" run "%~dp0vu1_replay.exe" "%~f1" "%~f2" --gate "%GATE%"
exit /b %errorlevel%
:missing
echo Missing vu1_replay.exe or vu1_validate.py beside this script. 1>&2
exit /b 1
:usage
echo Usage: %~nx0 suite-directory new-results-directory [programs^|blocks] 1>&2
exit /b 2
