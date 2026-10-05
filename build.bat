@echo off
setlocal
set "VARS_BAT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VARS_BAT=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
:no_vswhere
if not defined VARS_BAT if exist "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" set "VARS_BAT=J:\vs2026\VC\Auxiliary\Build\vcvars64.bat"
if not defined VARS_BAT (
    echo [ERROR] MSVC not found.
    exit /b 1
)
if not defined VSCMD_ARG_TGT_ARCH call "%VARS_BAT%" >nul

cd /d "%~dp0"
if not exist out mkdir out
if not exist obj mkdir obj

echo Building EchoVRMusic.dll (plugin)...
rem /Brepro: the same source gives the same bytes, so the setup app can tell an installed copy is current
cl.exe /nologo /LD /MD /O2 /EHa /W3 /std:c++17 /Brepro /Ithird_party /Foobj\ /Fe"out\EchoVRMusic.dll" src\echovrmusic.cpp src\capture.cpp third_party\detours.lib ole32.lib mmdevapi.lib user32.lib advapi32.lib /link /Brepro
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

del /q out\*.exp out\*.lib 2>nul
echo.
echo Built into %~dp0out
