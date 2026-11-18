@echo off
title OpenPocket - BlackBox USB rescue
REM Restores the official firmware of a BlackBox that no longer starts (black screen, or a restart loop) from its chip's
REM USB boot state (UBOOT), with no extra hardware and no driver to install. Windows' own mass-storage driver carries the
REM commands, which only an administrator may send: run this from a Command Prompt opened as administrator.
REM Read-only unless --write. The steps, the safety rules and the options are in docs\RESCUE.md.
REM   RESCUE.bat --package "C:\Users\you\Downloads\BlackBox.fwsc"            read-only check and full flash backup
REM   RESCUE.bat --write --package "C:\Users\you\Downloads\BlackBox.fwsc"    the rescue, after the read-only run passed
setlocal
cd /d "%~dp0"

"%SystemRoot%\System32\fltmc.exe" >nul 2>&1
if not errorlevel 1 goto admin
echo.
echo This needs a Command Prompt opened as administrator: Windows lets only an
echo administrator send the rescue commands to the pedal.
echo   1. Right-click the Start button and choose "Terminal (Admin)"
echo      or "Command Prompt (Admin)".
echo   2. Type this line, then Enter:
echo        cd /d "%~dp0"
echo   3. Run RESCUE.bat again there, with the same options.
echo Nothing was sent to the pedal.
echo.
pause
exit /b 1

:admin
set "PY="
where py >nul 2>&1 && set "PY=py -3"
if defined PY goto run
where python >nul 2>&1 && set "PY=python"
if defined PY goto run
echo.
echo Python 3 is not installed. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then run RESCUE.bat again. Nothing was sent to the pedal.
echo.
pause
exit /b 1

:run
%PY% tools\uboot_rescue.py %*
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
