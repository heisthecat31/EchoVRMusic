@echo off
rem Dev build: out\dev\EchoVRMusic.dll with EVM_DEV, which adds in-game speaker placement
rem (key 1 adds a speaker at your head, key 2 removes the last one, written to
rem plugins\EchoVRMusic\maps\<level>.txt). For laying out speakers only: never release this build.
rem Installs it into Echo when Echo is closed.
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
if not exist out\dev mkdir out\dev
if not exist obj\dev mkdir obj\dev

echo Building the DEV EchoVRMusic.dll (speaker placement on)...
cl.exe /nologo /LD /MD /O2 /EHa /W3 /std:c++17 /DEVM_DEV /Ithird_party /Foobj\dev\ /Fe"out\dev\EchoVRMusic.dll" src\echovrmusic.cpp src\capture.cpp third_party\detours.lib ole32.lib mmdevapi.lib user32.lib advapi32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
del /q out\dev\*.exp out\dev\*.lib 2>nul

set "GAME=C:\Oculus\Games\Software\Software\ready-at-dawn-echo-arena\bin\win10\plugins"
tasklist /fi "imagename eq echovr.exe" | find /i "echovr.exe" >nul
if %ERRORLEVEL% equ 0 (
    echo Echo VR is running: close it, then run this again to install the dev build.
    exit /b 0
)
if exist "%GAME%\" (
    copy /Y out\dev\EchoVRMusic.dll "%GAME%\" >nul
    echo Installed the DEV build into %GAME%
)
