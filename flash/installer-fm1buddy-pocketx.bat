@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
title fm1buddy install (Pocket X version) on ESP32-S3

echo ============================================================
echo   fm1buddy  -  POCKET X VERSION  -  ESP32-S3 installer
echo ============================================================
echo.
echo   This version adds a "Dump Pocket X" button that reads
echo   the Pocket X's original firmware through the hardware key
echo   (not over MIDI, which the stock Pocket X does not expose).
echo.
echo   Target: Freenove ESP32-S3 2.8" board FNK0104B (N16R8).
echo   Plug the ESP32-S3 into the PC over USB-C BEFORE going on.
echo.
echo   IMPORTANT: keep  pocketx-dump.patch  in the SAME folder
echo   as this .bat file.
echo.
pause
echo.

REM ---- 0. is the patch here? ----
if not exist "%~dp0pocketx-dump.patch" (
  echo   ERROR: pocketx-dump.patch not found next to the .bat.
  echo   Put both files in the same folder and run it again.
  pause & exit /b 1
)

REM ---- 1. Python ----
echo [1/6] Checking Python...
set "PY="
where py  >nul 2>&1 && set "PY=py"
if not defined PY ( where python >nul 2>&1 && set "PY=python" )
if not defined PY (
  echo   ERROR: Python not found. https://www.python.org/downloads/  ^(tick "Add to PATH"^)
  pause & exit /b 1
)
echo   OK : %PY%

REM ---- 2. Git ----
echo [2/6] Checking Git...
where git >nul 2>&1
if errorlevel 1 ( echo   ERROR: Git not found. https://git-scm.com/download/win & pause & exit /b 1 )
echo   OK : git

REM ---- 3. PlatformIO ----
echo [3/6] Installing / updating PlatformIO...
%PY% -m pip install --upgrade --quiet platformio
if errorlevel 1 ( echo   ERROR: pip failed. & pause & exit /b 1 )
echo   OK : platformio

REM ---- 4. Clean clone + pinned commit + Pocket X patch ----
echo [4/6] Getting fm1buddy ^(clean clone^)...
if exist fm1buddy (
  echo   Removing the old fm1buddy folder...
  rmdir /s /q fm1buddy
)
git clone https://github.com/konsumer/fm1buddy fm1buddy
if not exist fm1buddy\platformio.ini ( echo   ERROR: clone failed. & pause & exit /b 1 )
pushd fm1buddy
echo   Pinning the tested commit ^(3caf5ca^)...
git -c advice.detachedHead=false checkout 3caf5ca >nul 2>&1
if errorlevel 1 ( echo   ERROR: commit 3caf5ca not found. & popd & pause & exit /b 1 )
echo   Applying the Pocket X patch...
git apply --whitespace=nowarn "%~dp0pocketx-dump.patch"
if errorlevel 1 (
  echo   ERROR: the patch does not apply. & popd & pause & exit /b 1
)
echo   OK: patch applied.

REM ---- 5. Build + upload ----
echo [5/6] Building + installing on the ESP32-S3 ^(env cyd-s3^)...
echo   ^(First time: the toolchain downloads, several minutes.^)
echo   If it hangs on "Connecting....", hold BOOT, tap RESET, release BOOT.
echo.
%PY% -m platformio run -e cyd-s3 -t upload
set "RC=%errorlevel%"
popd
echo.
if not "%RC%"=="0" (
  echo ============================================================
  echo   Failed ^(code %RC%^). Check: USB-C data cable, BOOT+RESET,
  echo   COM port free ^(close Arduino / serial monitors^).
  echo ============================================================
  pause & exit /b %RC%
)

REM ---- 6. UBOOT loader for the dump ----
echo [6/6] Downloading the UBOOT loader ^(wl82loader.bin^)...
if not exist sdcard mkdir sdcard
powershell -NoProfile -Command ^
  "try { Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/c32b28e7a04f873f526ba929cb1bae9f05442fe9/data/loaderblobs/usb/wl82loader.bin' -OutFile 'sdcard\wl82loader.bin' } catch { exit 1 }"
if exist sdcard\wl82loader.bin (
  for /f %%H in ('powershell -NoProfile -Command "(Get-FileHash ''sdcard\wl82loader.bin'' -Algorithm SHA256).Hash.ToLower()"') do set "GOT=%%H"
  set "WANT=d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"
  if /i "!GOT!"=="!WANT!" ( echo   OK: sdcard\wl82loader.bin ^(SHA256 checked^) ) else ( echo   WARNING: unexpected SHA256, do not use it. )
) else ( echo   ^(No problem: fm1buddy can download it over Wi-Fi.^) )

echo.
echo ============================================================
echo   DONE. Pocket X version installed on the ESP32-S3.
echo.
echo   DUMP the Pocket X's original firmware:
echo    1. Copy  sdcard\wl82loader.bin  to the SD card, put it in the ESP32-S3.
echo    2. UNPLUG the ESP32-S3 from the PC. Power it some other way.
echo    3. Plug the Pocket X DIRECTLY into the ESP32-S3 (NO hub).
echo    4. Screen ^> Settings ^> "Recover the FM-1" ^> button "Dump Pocket X".
echo    5. Follow it: pedal off, then on when it asks.
echo    6. The dump is written to  /backups  on the SD card. That is your backup.
echo    7. On the PC:  python pocketx_verify_dump.py  YOUR_DUMP.bin
echo ============================================================
echo.
pause
endlocal
