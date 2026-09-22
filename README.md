# MonCtrl

Minimal Windows-native (pure Win32, no Qt, static allocation only) tray app
controlling monitor brightness and contrast over DDC/CI. Behavior-mirror of the
Linux ddcutil_simple_tray_ui tray app.

## How detection works

The detection pipeline follows PowerToys "Power Display" / Twinkle Tray's
node-ddcci (both use dxva2):

1. `EnumDisplayMonitors` -> HMONITOR list
2. Per HMONITOR: `GetNumberOfPhysicalMonitorsFromHMONITOR` ->
   `GetPhysicalMonitorsFromHMONITOR` (zero-initialized array, NULL-handle
   filtering), single-shot per round, retried across rounds from the main
   message loop
3. Single VCP read each for 0x10 (brightness) / 0x12 (contrast)
4. Identity from registry EDID (manufacturer:model:serial config keys)
5. AFTER acquisition completes, `QueryDisplayConfig` maps GDI device names to
   friendly monitor names ("24E3", ...) - calling it before opening handles
   makes dxva2 return NULL handles forever on some systems

### The poke helper (required part of the app)

`GetPhysicalMonitorsFromHMONITOR` only returns valid handles while some process
performs open/destroy cycles on the monitor handles - without that, it keeps
returning NULL handles and no monitor can ever be opened. `poke.exe` is therefore
a fundamental, always-required part of MonCtrl, not an optional workaround: it
runs automatically at every startup and on every re-detect, hidden, at ~0.5 MB,
for a bounded ~12 s run, then exits by itself. This changes nothing about the
app being lightweight.

If detection ever gets stuck again, right-click the tray icon and use
**Re-detect monitors** - it re-runs the poke and rediscovery.                                                                                              

## Features (mirrored from the Linux app)

- Tray flyout window: opens next to the tray icon (left-click), closes when you
  click away; right-click menu: Display Controls / Re-detect monitors / About / Quit
- Resume from suspend re-pokes and re-detects silently in the background
- Monitor unplug / signal loss (power off, input switch) re-pokes and
  re-detects via device notifications, also silently in the background
- Dark title bar + background following the system theme (controls stay native)
- Sync mode: one brightness slider + per-monitor offsets (-100..100)
- Individual mode: brightness + contrast slider per monitor with live value
- Min/Max buttons: `Min +off`, `Max +off`, `Min abs`, `Max abs`
- Vertical scrollbar + tall window when monitors overflow
- Debounced (250 ms) DDC writes on a background thread (100 ms transaction pacing)
- Config: `%APPDATA%\ddc-tray.conf` (`[UI] sync=`, `[Offset] mfr:model:serial=`)
- Single instance, per-monitor DPI aware, rounded corners (Win11)
- RAM stays far below 5 MB (app ~0.3-3 MB, poke ~0.5 MB for 12 s)

## Build

MSVC (any prompt; build.bat finds the VS environment itself):

```sh
build.bat            # x64, into the project folder
build.bat -x32       # 32-bit, into .\x32\
build.bat -arm       # ARM64 cross build into .\arm\ (needs ARM64 build tools)
```

CMake:

```sh
cmake -B build
cmake --build build --config Release
```

MinGW-w64 (x64 only; cross builds need MSVC):

```sh
g++ -O2 -mwindows -municode -DUNICODE -D_UNICODE main.cpp -o MonCtrl.exe -luser32 -lgdi32 -lcomctl32 -lshell32 -ldxva2
```

## Distribution

`build.bat` produces three binaries: `MonCtrl.exe`, `poke.exe`, `installer.exe`.
The installer does **not** bundle the app exes - it is a copy stub (~100 KB):
ship all three files in one folder and run `installer.exe` from it. It asks
for a target folder (auto-appends `\MonCtrl` unless the folder is already
named that), copies `MonCtrl.exe` + `poke.exe` there, and offers to launch.

## Autostart

To start MonCtrl with Windows:

1. Right-click `MonCtrl.exe` -> **Show more options** (old context menu) ->
   **Create shortcut**
2. Press Win+R, type `shell:startup`, press Enter
3. Drop the shortcut link into that folder - MonCtrl will autostart on login
