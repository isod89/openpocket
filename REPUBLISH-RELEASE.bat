@echo off
title OpenPocket - publish a release again
rem A release whose tag was moved (PUBLISH-SOURCE.bat before v41 did that) becomes a DRAFT on GitHub, and the Releases list
rem then shows an older version as "Latest". This publishes it again and marks it Latest. Needs the GitHub CLI (gh), logged in,
rem as PUBLISH-GITHUB.bat does.
rem Usage: REPUBLISH-RELEASE.bat [vNN] [-Repo owner/name]      (asks for the version when none is given)
setlocal
cd /d "%~dp0"
set TAG=%~1
set REPO=
if /i "%~2"=="-Repo" set REPO=%~3
if defined TAG goto have
for /f "tokens=3" %%v in ('findstr /c:"define OP_VERSION_NUM" firmware\openpocket.c') do set TAG=v%%~v
set TAG_IN=
set /p TAG_IN="Release to publish again [%TAG%]: "
if defined TAG_IN set TAG=%TAG_IN%
:have
where gh >nul 2>&1 || ( echo The GitHub CLI ^(gh^) is not installed: https://cli.github.com & goto end )
if not defined REPO for /f "delims=" %%r in ('gh api user --jq .login') do set REPO=%%r/openpocket
echo == %REPO%: release %TAG%
gh release edit %TAG% --repo %REPO% --draft=false --latest
if errorlevel 1 ( echo FAILED: see the message above. & goto end )
echo.
echo OK: %TAG% is published and marked Latest: https://github.com/%REPO%/releases/tag/%TAG%
:end
pause
