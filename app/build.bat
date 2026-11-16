@echo off
rem Builds DS4Native.exe. Works from any prompt: finds Visual Studio's x64 tools itself.
setlocal
set NoDefaultCurrentDirectoryInExePath=
cd /d "%~dp0"
where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio C++ tools not found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
set VIGEM=..\third_party\ViGEmClient
if not exist obj mkdir obj
cl /nologo /EHsc /O2 /W3 /std:c++17 /DUNICODE /D_UNICODE /I"%VIGEM%\include" /Foobj\ DS4Native.cpp "%VIGEM%\src\ViGEmClient.cpp" ^
   /Fe:DS4Native.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED || exit /b 1
echo.
echo Built DS4Native.exe - run it; it lives in the tray.
