@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem OpenTricky -- build from your own SSX Tricky (USA) Xbox disc image.
rem   build.bat "C:\path\to\SSX Tricky (USA).iso"
rem   (or drag the .iso onto this file)
rem Needs MSYS2 (https://www.msys2.org/) -- see docs/building.md.
setlocal
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
  echo MSYS2 not found in %MSYS2%.
  echo Install it from https://www.msys2.org/ or set MSYS2_ROOT to its folder.
  pause
  exit /b 1
)
if "%~1"=="" (
  echo usage: build.bat "C:\path\to\SSX Tricky (USA).iso"
  pause
  exit /b 2
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
cd /d "%~dp0"
"%MSYS2%\usr\bin\bash.exe" -lc "./build.sh \"$(cygpath -u '%~f1')\""
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" echo Build failed (%ERR%). See the messages above.
pause
exit /b %ERR%
