@echo off
rem Builds DS4Native.exe, then the installer and the portable zip into dist\.
rem Needs Inno Setup 6 (iscc on PATH) and the GitHub CLI (gh) to fetch the bundled driver installers.
setlocal
set NoDefaultCurrentDirectoryInExePath=
cd /d "%~dp0"
set VER=1.2.0
call ..\app\build.bat || exit /b 1
cd /d "%~dp0"

if not exist redist mkdir redist
if not exist redist\ViGEmBus_1.22.0_x64_x86_arm64.exe (
    gh release download v1.22.0 -R nefarius/ViGEmBus -p ViGEmBus_1.22.0_x64_x86_arm64.exe -D redist || exit /b 1
)
if not exist redist\HidHide_1.5.230_x64.exe (
    gh release download v1.5.230.0 -R nefarius/HidHide -p HidHide_1.5.230_x64.exe -D redist || exit /b 1
)

iscc /Q /DAppVer=%VER% DS4Native.iss || exit /b 1

set PORTABLE=..\dist\DS4-Native-%VER%-portable
if exist "%PORTABLE%" rmdir /s /q "%PORTABLE%"
mkdir "%PORTABLE%"
copy /y ..\app\DS4Native.exe "%PORTABLE%" >nul
copy /y ..\README.md "%PORTABLE%" >nul
copy /y ..\THIRD_PARTY_NOTICES.md "%PORTABLE%" >nul
if exist "%PORTABLE%.zip" del "%PORTABLE%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%PORTABLE%\*' -DestinationPath '%PORTABLE%.zip'" || exit /b 1
rmdir /s /q "%PORTABLE%"

echo.
echo Built dist\DS4-Native-Setup-%VER%.exe and dist\DS4-Native-%VER%-portable.zip
