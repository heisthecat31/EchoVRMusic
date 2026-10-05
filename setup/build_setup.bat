@echo off
rem Builds out\EchoVRMusicSetup.exe: the plugin (built first) is embedded in it.
setlocal
call "%~dp0..\build.bat"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

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
if not exist ..\obj mkdir ..\obj
rem The version (..\VERSION, e.g. 0.1.0) becomes the exe's file version; Spark compares it with
rem the latest GitHub release.
set "VER_MAJOR=0" & set "VER_MINOR=0" & set "VER_PATCH=0"
for /f "usebackq tokens=1-3 delims=." %%a in ("..\VERSION") do (
    set "VER_MAJOR=%%a" & set "VER_MINOR=%%b" & set "VER_PATCH=%%c"
)
echo Building EchoVRMusicSetup.exe v%VER_MAJOR%.%VER_MINOR%.%VER_PATCH%...
rc.exe /nologo /dVER_MAJOR=%VER_MAJOR% /dVER_MINOR=%VER_MINOR% /dVER_PATCH=%VER_PATCH% /fo ..\obj\setup.res setup.rc
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cl.exe /nologo /TC /O2 /W3 /MT /DUNICODE /D_UNICODE /Fo..\obj\ /Fe"..\out\EchoVRMusicSetup.exe" setup.c ^
  /link /SUBSYSTEM:WINDOWS ..\obj\setup.res
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo Built %~dp0..\out\EchoVRMusicSetup.exe
rem Spark ships the setup app in its Music tab: keep its copy current.
if exist "%~dp0..\..\Spark\EchoVRMusic\" (
    copy /Y "%~dp0..\out\EchoVRMusicSetup.exe" "%~dp0..\..\Spark\EchoVRMusic\" >nul
    echo Copied it to ..\Spark\EchoVRMusic\
)
