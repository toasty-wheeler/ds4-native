@echo off
rem Builds DS4Native.exe. Works from any prompt: finds Visual Studio's x64 tools itself.
rem build.bat [name.exe] builds under another name (e.g. while DS4Native.exe is running).
setlocal
set NoDefaultCurrentDirectoryInExePath=
cd /d "%~dp0"
where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio C++ tools not found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
set "OUT=%~1"
if "%OUT%"=="" set OUT=DS4Native.exe
set VIGEM=..\third_party\ViGEmClient
set SPOUT=..\third_party\Spout2\SPOUTSDK
if not exist obj mkdir obj
rem Spout2's sender (the stream overlay): its own step, it uses the narrow-character Windows APIs.
cl /nologo /c /EHsc /O2 /W0 /std:c++17 /I"%SPOUT%\SpoutGL" /Foobj\ "%SPOUT%\SpoutDirectX\SpoutDX\SpoutDX.cpp" ^
   "%SPOUT%\SpoutGL\SpoutDirectX.cpp" "%SPOUT%\SpoutGL\SpoutSenderNames.cpp" "%SPOUT%\SpoutGL\SpoutSharedMemory.cpp" ^
   "%SPOUT%\SpoutGL\SpoutFrameCount.cpp" "%SPOUT%\SpoutGL\SpoutCopy.cpp" "%SPOUT%\SpoutGL\SpoutUtils.cpp" || exit /b 1
cl /nologo /EHsc /O2 /W3 /std:c++17 /DUNICODE /D_UNICODE /I"%VIGEM%\include" /I"%SPOUT%\SpoutGL" /I"%SPOUT%\SpoutDirectX\SpoutDX" ^
   /Foobj\ DS4Native.cpp "%VIGEM%\src\ViGEmClient.cpp" obj\SpoutDX.obj obj\SpoutDirectX.obj obj\SpoutSenderNames.obj ^
   obj\SpoutSharedMemory.obj obj\SpoutFrameCount.obj obj\SpoutCopy.obj obj\SpoutUtils.obj ^
   /Fe:%OUT% /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED || exit /b 1
echo.
echo Built %OUT% - run it; it lives in the tray.
