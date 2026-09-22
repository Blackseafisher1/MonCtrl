@echo off
setlocal
cd /d "%~dp0"

rem ============================================================
rem  MonCtrl build script
rem  usage: build.bat [-x64] [-x32 ^| -x86] [-arm ^| -arm64]
rem    default (no args) : x64 build into current folder
rem    -x32              : 32-bit build into .\x32\
rem    -arm              : ARM64 cross build into .\arm\
rem                        (needs the ARM64 build-tools workload)
rem ============================================================

set ARCH=x64
set OUTDIR=.
set VCVARS=vcvars64.bat
set VCVARSARG=
set ARCHOK=

if "%~1"=="" set ARCHOK=1
if /i "%~1"=="-x64" set ARCHOK=1
if /i "%~1"=="-x86" set ARCH=x86
if /i "%~1"=="-x86" set OUTDIR=x32
if /i "%~1"=="-x86" set VCVARS=vcvars32.bat
if /i "%~1"=="-x86" set ARCHOK=1
if /i "%~1"=="-x32" set ARCH=x86
if /i "%~1"=="-x32" set OUTDIR=x32
if /i "%~1"=="-x32" set VCVARS=vcvars32.bat
if /i "%~1"=="-x32" set ARCHOK=1
if /i "%~1"=="-arm" set ARCH=arm64
if /i "%~1"=="-arm" set OUTDIR=arm
if /i "%~1"=="-arm" set VCVARS=vcvarsall.bat
if /i "%~1"=="-arm" set VCVARSARG=amd64_arm64
if /i "%~1"=="-arm" set ARCHOK=1
if /i "%~1"=="-arm64" set ARCH=arm64
if /i "%~1"=="-arm64" set OUTDIR=arm
if /i "%~1"=="-arm64" set VCVARS=vcvarsall.bat
if /i "%~1"=="-arm64" set VCVARSARG=amd64_arm64
if /i "%~1"=="-arm64" set ARCHOK=1

if not defined ARCHOK goto usage

if not "%OUTDIR%"=="." if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem ---- locate compiler: MSVC (via vswhere / known path), else MinGW g++ ----
rem NOTE: no nested parens in this section: %ProgramFiles(x86)% would break them
where cl >nul 2>nul
if %errorlevel%==0 goto msvc

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if not exist "%VSWHERE%" goto vsdir_fallback
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
:vsdir_fallback
if not defined VSDIR if exist "D:\VS\data\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=D:\VS\data"
if not defined VSDIR goto no_compiler
if defined VCVARSARG goto call_cross
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" >nul
goto msvc_check
:call_cross
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" %VCVARSARG% >nul
:msvc_check
where cl >nul 2>nul
if %errorlevel%==0 goto verify_arch
if /i "%ARCH%"=="arm64" goto no_arm64
echo Could not initialize the %ARCH% toolchain.
exit /b 1

:verify_arch
if /i not "%ARCH%"=="arm64" goto msvc
cl 2>&1 | findstr /C:"for ARM64" >nul
if not errorlevel 1 goto msvc
goto no_arm64

:no_arm64
echo ARM64 build tools are not installed.
echo In the Visual Studio Installer, add the "C++ ARM64 build tools" workload,
echo then run: build.bat -arm
exit /b 1

:no_compiler
where g++ >nul 2>nul
if %errorlevel%==0 goto mingw_check
echo No compiler found. Install VS C++ tools or MinGW-w64, or run from
echo "x64 Native Tools Command Prompt for VS".
exit /b 1

:mingw_check
if /i not "%ARCH%"=="x64" (
  echo MinGW cross builds are not supported by this script; use MSVC: build.bat -%ARCH%
  exit /b 1
)
goto mingw

:usage
echo usage: build.bat [-x64] [-x32 ^| -x86] [-arm ^| -arm64]
exit /b 1

:msvc
echo target: %ARCH% -^> %OUTDIR%\
echo [1/4] rc      app.rc
rc /nologo /fo"%OUTDIR%/app.res" app.rc
if errorlevel 1 goto fail

echo [2/4] cl      %OUTDIR%/MonCtrl.exe
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE main.cpp "%OUTDIR%/app.res" /Fo"%OUTDIR%/main.obj" /Fe"%OUTDIR%/MonCtrl.exe" /link /SUBSYSTEM:WINDOWS advapi32.lib
if errorlevel 1 goto fail

echo [3/4] cl      %OUTDIR%/poke.exe
cl /nologo /O2 /W3 poke.cpp /Fo"%OUTDIR%/poke.obj" /Fe"%OUTDIR%/poke.exe" /link /SUBSYSTEM:CONSOLE user32.lib dxva2.lib
if errorlevel 1 goto fail

echo [4/4] cl      %OUTDIR%/installer.exe
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE installer.cpp /Fo"%OUTDIR%/installer.obj" /Fe"%OUTDIR%/installer.exe" /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib shell32.lib uuid.lib
if errorlevel 1 goto fail
goto summary

:mingw
echo target: x64 -^> .\
echo [1/4] windres app.rc
windres app.rc -O coff -o app_rc.o
if errorlevel 1 goto fail

echo [2/4] g++     MonCtrl.exe
g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE main.cpp app_rc.o -o MonCtrl.exe -luser32 -lgdi32 -lcomctl32 -lshell32 -ldxva2 -ladvapi32
if errorlevel 1 goto fail

echo [3/4] g++     poke.exe
g++ -O2 poke.cpp -o poke.exe -luser32 -ldxva2
if errorlevel 1 goto fail

echo [4/4] g++     installer.exe
g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE installer.cpp -o installer.exe -lole32 -lshell32 -luuid
if errorlevel 1 goto fail
goto summary

:summary
echo.
echo Build complete:
for %%f in (MonCtrl.exe poke.exe installer.exe) do (
  if exist "%OUTDIR%/%%f" echo   %OUTDIR%/%%f
)
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
