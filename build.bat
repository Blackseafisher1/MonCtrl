@echo off
setlocal
cd /d "%~dp0"

rem ============================================================
rem  MonCtrl build script
rem  builds: MonCtrl.exe, poke.exe, installer.exe
rem ============================================================

rem ---- locate compiler: MSVC (via vswhere / known path), else MinGW g++ ----
where cl >nul 2>nul
if not %errorlevel%==0 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
  )
  if not defined VCINSTALLDIR (
    if exist "D:\VS\data\VC\Auxiliary\Build\vcvars64.bat" call "D:\VS\data\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

where cl >nul 2>nul
if %errorlevel%==0 goto msvc

where g++ >nul 2>nul
if %errorlevel%==0 goto mingw

echo No compiler found. Install VS C++ tools or MinGW-w64, or run from
echo "x64 Native Tools Command Prompt for VS".
exit /b 1

:msvc
echo [1/4] rc      app.rc
rc /nologo app.rc
if errorlevel 1 goto fail

echo [2/4] cl      MonCtrl.exe
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE main.cpp app.res /Fe:MonCtrl.exe /link /SUBSYSTEM:WINDOWS advapi32.lib
if errorlevel 1 goto fail

echo [3/4] cl      poke.exe
cl /nologo /O2 /W3 poke.cpp /Fe:poke.exe /link /SUBSYSTEM:CONSOLE user32.lib dxva2.lib
if errorlevel 1 goto fail

echo [4/4] cl      installer.exe
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE installer.cpp /Fe:installer.exe /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib shell32.lib uuid.lib
if errorlevel 1 goto fail
goto summary

:mingw
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
  if exist %%f echo   %%f  -  %%~zf bytes
)
exit /b 0
