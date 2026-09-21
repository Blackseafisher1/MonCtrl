# MonCtrl

Minimal Windows-native (pure Win32, no Qt, static allocation only) tray app
controlling monitor brightness and contrast over DDC/CI. Behavior-mirror of the
Linux ddcutil_simple_tray_ui tray app.

Display detection copies the PowerToys "Power Display" pipeline:

1. `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` inventory: GDI device name,
   friendly monitor name, stable device-path id, Windows monitor number
2. `EnumDisplayMonitors` -> HMONITOR list
3. Per HMONITOR: `GetMonitorInfo` -> GDI name -> match inventory entry
4. `GetNumberOfPhysicalMonitorsFromHMONITOR` -> `GetPhysicalMonitorsFromHMONITOR`
   with 3 x 200 ms retry and NULL-handle filtering
5. VCP probe (0x10 brightness / 0x12 contrast): 3 attempts, 100 ms pacing
6. Name: FriendlyName (non-Generic) -> physical description (non-Generic/PnP)
   -> "External Display"; config key: sanitized device path

If nothing is acquired, discovery re-runs (up to 20 rounds, then again in the
background) since handles can come back NULL for a while when the I2C bus is
held by another DDC app or the driver is still warming up.

## Features (mirrored from the Linux app)

- Tray flyout window: opens next to the tray icon, closes when you click away
- Left/middle/double-click toggles the window, right-click menu (Display Controls / Quit)
- Sync mode: one brightness slider + per-monitor offsets (-100..100)
- Individual mode: brightness + contrast slider per monitor with live value
- Min/Max buttons: `Min +off`, `Max +off`, `Min abs`, `Max abs`
- Vertical scrollbar + tall window when monitors overflow
- Debounced (250 ms) DDC writes on a background thread (100 ms transaction pacing)
- Config: `%APPDATA%\ddc-tray.conf` (`[UI] sync=`, `[Offset] <device-path>=`)
- Single instance, per-monitor DPI aware, rounded corners (Win11)
- RAM stays far below 5 MB

## Build

MSVC (any prompt; build.bat loads the VS environment itself):

```sh
build.bat
```

CMake:

```sh
cmake -B build
cmake --build build --config Release
```

MinGW-w64:

```sh
g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE main.cpp -o MonCtrl.exe -luser32 -lgdi32 -lcomctl32 -lshell32 -ldxva2
```
