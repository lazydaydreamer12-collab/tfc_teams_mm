@echo off
setlocal
rem ---------------------------------------------------------------------------
rem make_release.bat - packs a release zip for GitHub:
rem   release\tfc_teams_mm-<version>.zip  ->  addons\tfc_teams_mm\...  (unzip in a server's tfc folder)
rem
rem Takes the Windows DLL from the Release build (bin\Release - build "Release | Win32"
rem in Visual Studio first) and the Linux .so from tfc\addons\tfc_teams_mm\.
rem ---------------------------------------------------------------------------
cd /d "%~dp0"
set NAME=tfc_teams_mm
set CFG=tfc_teams.ini

set VER=
set /p VER=Version number (e.g. 1.1.0): 
if "%VER%"=="" set VER=1.1.0

set DLL=bin\Release\%NAME%.dll
set SO=..\..\..\%NAME%\%NAME%_mm_i386.so

if not exist "%DLL%" (
  echo.
  echo No Release build found: %DLL%
  echo In Visual Studio pick "Release" and "Win32" at the top, build, then run this again.
  pause
  exit /b 1
)
if not exist "%SO%" (
  echo.
  echo No Linux build found: %SO%
  pause
  exit /b 1
)

set STAGE=release\stage
set OUT=%STAGE%\addons\%NAME%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%OUT%\configs"
mkdir "%OUT%\docs"
copy /y "%DLL%" "%OUT%\%NAME%_mm.dll" >nul
copy /y "%SO%" "%OUT%\%NAME%_mm_i386.so" >nul
copy /y "configs\%CFG%" "%OUT%\configs\%CFG%" >nul
copy /y "README.md" "%OUT%\docs\README.md" >nul
copy /y "CHANGELOG.md" "%OUT%\docs\CHANGELOG.md" >nul
copy /y "LICENSE" "%OUT%\docs\LICENSE" >nul
set PARTS=addons

set ZIP=release\%NAME%-%VER%.zip
if exist "%ZIP%" del "%ZIP%"
rem Windows' tar writes a zip with forward slashes, so it unzips right on Linux too.
tar -a -c -f "%ZIP%" -C "%STAGE%" %PARTS%
if errorlevel 1 (
  echo Making the zip failed.
  pause
  exit /b 1
)
rmdir /s /q "%STAGE%"

echo.
echo Made %ZIP%:
tar -t -f "%ZIP%"
echo.
echo Attach it to the GitHub release (tag v%VER%).
explorer release
pause
