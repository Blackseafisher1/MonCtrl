@echo off
cd /d "%~dp0"

rem Find cl; if missing, load the Visual Studio environment
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
if %errorlevel%==0 (
  rc /nologo app.rc
  if errorlevel 1 exit /b 1
  cl /nologo /O2 /W3 /DUNICODE /D_UNICODE main.cpp app.res /Fe:MonCtrl.exe /link /SUBSYSTEM:WINDOWS advapi32.lib
  if errorlevel 1 exit /b 1
  cl /nologo /O2 poke.cpp /Fe:poke.exe /link /SUBSYSTEM:CONSOLE user32.lib dxva2.lib
  if errorlevel 1 exit /b 1
  cl /nologo /O2 /W3 /DUNICODE /D_UNICODE installer.cpp /Fe:installer.exe /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib shell32.lib uuid.lib
  exit /b %errorlevel%
)

where g++ >nul 2>nul
if %errorlevel%==0 (
  windres app.rc -O coff -o app_rc.o
  if errorlevel 1 exit /b 1
  g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE main.cpp app_rc.o -o MonCtrl.exe -luser32 -lgdi32 -lcomctl32 -lshell32 -ldxva2 -ladvapi32
  if errorlevel 1 exit /b 1
  g++ -O2 poke.cpp -o poke.exe -luser32 -ldxva2
  if errorlevel 1 exit /b 1
  g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE installer.cpp -o installer.exe -lole32 -lshell32 -luuid
  exit /b %errorlevel%
)

echo No compiler found. Install VS C++ tools or MinGW-w64, or run from
echo "x64 Native Tools Command Prompt for VS".
exit /b 1
