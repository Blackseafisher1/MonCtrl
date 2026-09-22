// MonCtrl - Windows native tray app controlling monitor brightness/contrast via DDC/CI.
// Win32 port of ddcutil_simple_tray_ui (no Qt). Detection order is load-bearing:
//   1. EnumDisplayMonitors -> HMONITOR list
//   2. per HMONITOR: GetNumberOfPhysicalMonitorsFromHMONITOR ->
//      GetPhysicalMonitorsFromHMONITOR (zero-initialized array, NULL-handle
//      filtering), single-shot per round, retried across rounds from the main
//      message loop
//   3. single VCP read each for 0x10 (brightness) / 0x12 (contrast)
//   4. identity from registry EDID (manufacturer:model:serial config keys)
//   5. AFTER acquisition completes, QueryDisplayConfig maps GDI device names
//      to friendly monitor names - calling it before opening handles makes
//      dxva2 return NULL handles forever on some systems
// On some systems the opens only succeed while poke.exe (spawned at startup
// and on every re-detect) performs open/destroy cycles; detection therefore
// runs bounded retry rounds (~40 s) and then stops - recovery is via poke +
// Re-detect, resume re-detect, or display-change re-detect.
// All allocations are static (fixed-size arrays). RAM target < 5 MB
// (typically well under 1 MB idle).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "uxtheme.lib")
// comctl6 + DPI manifest is embedded via app.rc / app.manifest
#endif

// Delay between back-to-back DDC write transactions on the worker thread.
// Opens and VCP reads are intentionally single-shot per detection round:
// poke.exe warms the bus and detect_pump() spaces the rounds.
#define WRITE_PACE_MS 100

#define VCP_BRIGHTNESS 0x10
#define VCP_CONTRAST   0x12

#define MAX_MON 8
#define DEBOUNCE_MS 250

#define WM_APP_TRAY     (WM_APP + 1)
#define WM_APP_SHOW     (WM_APP + 3)

#define ID_SYNCCHK   1
#define ID_SYNCSCALE 2
#define ID_HINT      3
#define ID_HEADER    5
#define ID_BTN       10
#define ID_BRI       100
#define ID_CON       200
#define ID_OFF       300
#define ID_TITLE     500
#define TIMER_APPLY  1
#define TIMER_TRIM   2
#define TIMER_RESUME 3

struct Monitor {
    HMONITOR hmon_tag;
    HANDLE hphys;
    WCHAR desc[128];
    char key[160];
    int bri, con;           // target values
    int bri_max, con_max;
    int la_bri, la_con;     // last applied
    int off;
    HWND title, bri_lbl, bri_scale, bri_val;
    HWND con_lbl, con_scale, con_val;
    HWND off_lbl, off_edit, off_ud;
};

static Monitor mons[MAX_MON];
static int mon_count = 0;

static HINSTANCE g_inst;
static HWND g_main, g_header, g_sync_check, g_sync_scale, g_sync_val, g_sync_hint, g_btn[4];
static HWND g_tips;
static bool g_populated = false, g_updating = false, g_sync = false, g_timer_on = false;
static HFONT g_fnt, g_fnt_bold, g_fnt_small;
static HICON g_icon;
static HBITMAP g_icon_dib;
static UINT g_msg_tbcreated;
static UINT (WINAPI *g_pGetDpiForWindow)(HWND);
static const wchar_t g_cls[] = L"MonCtrlWnd";
static POINT g_anchor = { -1, -1 };
static DWORD g_hide_tick = 0;
static int g_scroll = 0;
static HANDLE g_worker, g_ev_apply, g_ev_quit;
static CRITICAL_SECTION g_cs;
static HANDLE g_poke = NULL;
static bool g_bg_detect = false;       // re-detect without showing the window
static bool g_dark = false;            // system theme dark
static HBRUSH g_br_dark = NULL;
static HDEVNOTIFY g_devnotify = NULL;

// GUID_DEVINTERFACE_MONITOR: monitor arrival/removal notifications
// (same GUID as in QueryDisplayConfig device paths)
static const GUID GUID_DEV_MONITOR =
    { 0xE6F07B5F, 0xEE97, 0x4A90, { 0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7 } };

// Spawns poke.exe (bounded ~12 s self-exiting run in CREATE_NO_WINDOW): its
// open/destroy cycles warm the session DDC path so our single-shot opens
// succeed. Re-spawned on manual re-detect and after resume. There is no
// in-process equivalent, so detection deliberately does no endless retry.
static void start_poke() {
    char exe[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    lstrcpynA(dir, exe, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    char poke[MAX_PATH];
    wsprintfA(poke, "%s\\poke.exe", dir);
    if (GetFileAttributesA(poke) == INVALID_FILE_ATTRIBUTES) return;
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessA(poke, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        g_poke = pi.hProcess;
        CloseHandle(pi.hThread);
    }
}

static void stop_poke() {
    if (g_poke) {
        TerminateProcess(g_poke, 0);
        CloseHandle(g_poke);
        g_poke = NULL;
    }
}

// Monitor plug/unplug or signal loss (power off, input switch) arrives as
// WM_DEVICECHANGE for the monitor interface class - poke + rediscover.
// DBT_DEVNODES_CHANGED is deliberately ignored (fires for any USB etc.).
static void register_monitor_notify() {
    DEV_BROADCAST_DEVICEINTERFACE_W dbi;
    ZeroMemory(&dbi, sizeof(dbi));
    dbi.dbcc_size = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid = GUID_DEV_MONITOR;
    g_devnotify = RegisterDeviceNotificationW(g_main, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static void relayout();
static void apply_visibility();
static void populate_ui();
static void teardown_ui();
static void present();
static void hide_window();
static void tray_menu(int x, int y);
static void do_quit();
static void detect_pump();
static void reset_detection();

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int get_dpi(HWND w) {
    if (w && g_pGetDpiForWindow) return (int)g_pGetDpiForWindow(w);
    HDC dc = GetDC(w);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(w, dc);
    return d;
}

static void set_dpi_awareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    FARPROC pf = GetProcAddress(u, "GetDpiForWindow");
    if (pf) g_pGetDpiForWindow = (UINT (WINAPI *)(HWND))pf;
    FARPROC p = GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (p) {
        BOOL (WINAPI *fn)(HANDLE) = (BOOL (WINAPI *)(HANDLE))p;
        if (fn((HANDLE)(INT_PTR)-4)) return;
    }
    SetProcessDPIAware();
}

static void trim_mem() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

// drop a control's visual styles so classic text colors apply (dark mode checkbox)
static void set_theme_window(HWND h, bool untheme) {
    if (!h) return;
    HMODULE d = GetModuleHandleW(L"uxtheme.dll");
    if (!d) d = LoadLibraryW(L"uxtheme.dll");
    if (!d) return;
    HRESULT (WINAPI *fn)(HWND, LPCWSTR, LPCWSTR) =
        (HRESULT (WINAPI *)(HWND, LPCWSTR, LPCWSTR))GetProcAddress(d, "SetWindowTheme");
    if (!fn) return;
    if (untheme) fn(h, L"", L"");
    else fn(h, NULL, NULL);
}

// ---------- system theme (dark title bar + background only) ----------

static bool is_system_dark() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD val = 1, type = 0, size = sizeof(val);
    LONG r = RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&val, &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && val == 0;
}

static void apply_theme() {
    g_dark = is_system_dark();
    if (!g_br_dark) g_br_dark = CreateSolidBrush(RGB(32, 32, 32));
    // dark/light window background via class brush
    SetClassLongPtrW(g_main, GCLP_HBRBACKGROUND,
                     (LONG_PTR)(g_dark ? g_br_dark : GetSysColorBrush(COLOR_BTNFACE)));
    // immersive dark title bar (attribute 20, fallback 19 on older builds)
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (d) {
        HRESULT (WINAPI *fn)(HWND, DWORD, LPCVOID, DWORD) =
            (HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD))GetProcAddress(d, "DwmSetWindowAttribute");
        if (fn) {
            int v = g_dark ? 1 : 0;
            if (FAILED(fn(g_main, 20, &v, sizeof(v)))) fn(g_main, 19, &v, sizeof(v));
        }
        FreeLibrary(d);
    }
    // classic-render the checkbox in dark mode so WM_CTLCOLORBTN text colors apply
    if (g_sync_check) {
        set_theme_window(g_sync_check, g_dark);
    }
    InvalidateRect(g_main, NULL, TRUE);
}

// ---------- config (same INI format as the Linux ddc-gtk-tray.conf) ----------

static char g_conf[MAX_PATH];

static void init_conf() { ExpandEnvironmentStringsA("%APPDATA%\\ddc-tray.conf", g_conf, MAX_PATH); }

static bool load_conf_sync() {
    char b[16];
    GetPrivateProfileStringA("UI", "sync", "false", b, 16, g_conf);
    return lstrcmpiA(b, "true") == 0;
}

static void load_conf_offsets() {
    for (int i = 0; i < mon_count; i++) {
        char b[16];
        GetPrivateProfileStringA("Offset", mons[i].key[0] ? mons[i].key : "bus", "0", b, 16, g_conf);
        mons[i].off = atoi(b);
    }
}

static void save_conf() {
    char b[16];
    WritePrivateProfileStringA("UI", "sync", g_sync ? "true" : "false", g_conf);
    for (int i = 0; i < mon_count; i++) {
        wsprintfA(b, "%d", mons[i].off);
        WritePrivateProfileStringA("Offset", mons[i].key[0] ? mons[i].key : "bus", b, g_conf);
    }
}

// ---------- DDC primitives ----------

// Single VCP read attempt. No in-function retry: the bus is warmed by
// poke.exe and detect_pump() re-runs the whole round on a miss.
static bool vcp_probe(HANDLE h, BYTE code, DWORD *cur, DWORD *mx) {
    return GetVCPFeatureAndVCPFeatureReply(h, code, NULL, cur, mx) ? true : false;
}

// ---------- inventory ----------
// QueryDisplayConfig is only called AFTER all monitor handles are acquired:
// calling it earlier makes subsequent GetPhysicalMonitorsFromHMONITOR opens
// return NULL handles on some systems. Post-acquisition it is used purely to
// map GDI device names to friendly monitor names.

struct InvEntry {
    WCHAR gdi[32];
    WCHAR friendly[128];
};

static int InventoryAll(InvEntry out[MAX_MON]) {
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS) return 0;
    DISPLAYCONFIG_PATH_INFO paths[MAX_MON];
    DISPLAYCONFIG_MODE_INFO modes[256];
    UINT32 pc2 = pc < MAX_MON ? pc : MAX_MON, mc2 = mc < 256 ? mc : 256;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc2, paths, &mc2, modes, NULL) != ERROR_SUCCESS)
        return 0;
    int n = 0;
    for (UINT32 i = 0; i < pc2 && n < MAX_MON; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
        ZeroMemory(&sn, sizeof(sn));
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
        ZeroMemory(&tn, sizeof(tn));
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        InvEntry *e = &out[n];
        lstrcpynW(e->gdi, sn.viewGdiDeviceName, 32);
        lstrcpynW(e->friendly, tn.monitorFriendlyDeviceName, 128);
        n++;
    }
    return n;
}

// replace generic descriptions with QueryDisplayConfig friendly names
// (called only after acquisition completed)
static bool name_is_generic(const wchar_t *s);

static void fill_friendly_names() {
    InvEntry inv[MAX_MON];
    int nInv = InventoryAll(inv);
    for (int i = 0; i < mon_count; i++) {
        MONITORINFOEXW mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mons[i].hmon_tag, (LPMONITORINFO)&mi)) continue;
        for (int k = 0; k < nInv; k++) {
            if (_wcsicmp(mi.szDevice, inv[k].gdi) == 0 &&
                !name_is_generic(inv[k].friendly)) {
                lstrcpynW(mons[i].desc, inv[k].friendly, 128);
                break;
            }
        }
    }
}

struct HArr { HMONITOR a[MAX_MON]; int n; };

// discovery state, driven from the main message loop (background detect
// threads proved unreliable on some systems; main-thread rounds always work)
static InvEntry g_inv[MAX_MON];
static int g_ninv = 0;
static HArr g_harr;
static bool g_disc_done = false;
static int g_disc_rounds = 0;
static DWORD g_last_attempt = 0;

static BOOL CALLBACK collect_hmons(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    HArr *p = (HArr *)lp;
    if (p->n < MAX_MON) p->a[p->n++] = hm;
    return TRUE;
}

static int CollectHmons(HArr *out) {
    out->n = 0;
    EnumDisplayMonitors(NULL, NULL, collect_hmons, (LPARAM)out);
    return out->n;
}

// ---------- per-monitor open + EDID identify ----------

static bool name_is_generic(const wchar_t *s) {
    if (!s || !s[0]) return true;
    // PowerDisplay rejects names containing "Generic" or "PnP"
    for (const wchar_t *p = s; *p; p++) {
        if (*p == L'G' || *p == L'g') {
            const wchar_t *q = p, *w = L"Generic";
            while (*q && *w && (*q | 0x20) == (*w | 0x20)) { q++; w++; }
            if (!*w) return true;
        }
        if (*p == L'P' || *p == L'p') {
            const wchar_t *q = p, *w = L"PnP";
            while (*q && *w && (*q | 0x20) == (*w | 0x20)) { q++; w++; }
            if (!*w) return true;
        }
    }
    return false;
}

static void sanitize_key(char *s) {
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == ':' || c == '-' || c == '_' || c == '.'))
            *s = '_';
    }
}

static void free_all_monitors() {
    for (int i = 0; i < MAX_MON; i++)
        if (mons[i].hphys) DestroyPhysicalMonitor(mons[i].hphys);
    ZeroMemory(mons, sizeof(mons));
    mon_count = 0;
}

// registry EDID read AFTER acquisition (this call order is the proven-working
// v1 pattern: no display APIs other than EnumDisplayMonitors before the open)
static bool read_edid(HMONITOR hm, BYTE out[128]) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (LPMONITORINFO)&mi)) return false;
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (wcscmp(dd.DeviceName, mi.szDevice) != 0) continue;
        if (_wcsnicmp(dd.DeviceKey, L"\\Registry\\Machine\\", 18) != 0) return false;
        HKEY k = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dd.DeviceKey + 18, 0, KEY_READ, &k) != ERROR_SUCCESS)
            return false;
        HKEY dp = NULL;
        LONG r = RegOpenKeyExW(k, L"Device Parameters", 0, KEY_READ, &dp);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS) return false;
        DWORD type = 0, size = 128;
        r = RegQueryValueExW(dp, L"EDID", NULL, &type, out, &size);
        RegCloseKey(dp);
        return r == ERROR_SUCCESS && size >= 128;
    }
    return false;
}

// one discovery round on the calling (main) thread; appends newly acquired
// monitors to mons[] (single-shot opens, retry via the round pacing)
static void discover_round() {
    if (g_disc_rounds == 0) CollectHmons(&g_harr);
    g_disc_rounds++;

    for (int i = 0; i < g_harr.n; i++) {
        bool have = false;
        for (int k = 0; k < mon_count; k++)
            if (mons[k].hmon_tag == g_harr.a[i]) { have = true; break; }
        if (have) continue;

        Monitor *m = &mons[mon_count];
        ZeroMemory(m, sizeof(*m));
        m->hmon_tag = g_harr.a[i];
        m->bri_max = m->con_max = 100;

        WCHAR phys[128] = L"";
        DWORD n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(g_harr.a[i], &n) || n == 0) continue;
        if (n > MAX_MON) n = MAX_MON;
        PHYSICAL_MONITOR pm[MAX_MON];
        ZeroMemory(pm, sizeof(pm));
        if (!GetPhysicalMonitorsFromHMONITOR(g_harr.a[i], n, pm)) continue;
        HANDLE first = NULL;
        for (DWORD d = 0; d < n; d++) {
            if (pm[d].hPhysicalMonitor && !first) {
                first = pm[d].hPhysicalMonitor;
                lstrcpynW(phys, pm[d].szPhysicalMonitorDescription, 128);
            } else {
                DestroyPhysicalMonitor(pm[d].hPhysicalMonitor);
            }
        }
        if (!first) {
            m->hphys = NULL;
            continue;
        }
        m->hphys = first;

        DWORD cur, mx;
        if (vcp_probe(m->hphys, VCP_BRIGHTNESS, &cur, &mx)) {
            m->bri = (int)cur;
            if ((int)mx > 0) m->bri_max = (int)mx;
            m->la_bri = (int)cur;
        } else {
            m->bri = 50;
        }
        if (vcp_probe(m->hphys, VCP_CONTRAST, &cur, &mx)) {
            m->con = (int)cur;
            if ((int)mx > 0) m->con_max = (int)mx;
            m->la_con = (int)cur;
        } else {
            m->con = 50;
        }

        // identity from EDID (registry); fallback to physical description
        BYTE ed[128];
        if (read_edid(g_harr.a[i], ed) && ed[0] == 0x00 && ed[1] == 0xFF && ed[2] == 0xFF) {
            unsigned id = ((unsigned)ed[8] << 8) | ed[9];
            char mfr[4];
            mfr[0] = (char)('A' - 1 + ((id >> 10) & 31));
            mfr[1] = (char)('A' - 1 + ((id >> 5) & 31));
            mfr[2] = (char)('A' - 1 + (id & 31));
            mfr[3] = 0;
            for (int j = 0; j < 3; j++)
                if (mfr[j] < 'A' || mfr[j] > 'Z') mfr[j] = '?';
            unsigned prod = ed[10] | ((unsigned)ed[11] << 8);
            unsigned ser = ed[12] | ((unsigned)ed[13] << 8) | ((unsigned)ed[14] << 16) | ((unsigned)ed[15] << 24);
            char name[14];
            int nn = 0;
            for (int d = 54; d <= 108; d += 18) {
                if (!ed[d] && !ed[d + 1] && !ed[d + 2] && ed[d + 3] == 0xFC) {
                    for (int j = 0; j < 13; j++) {
                        BYTE c = ed[d + 5 + j];
                        if (!c || c == '\n') break;
                        if (c >= 32 && c < 127) name[nn++] = (char)c;
                    }
                    break;
                }
            }
            while (nn > 0 && name[nn - 1] == ' ') nn--;
            name[nn] = 0;
            char descA[128];
            if (nn) lstrcpyA(descA, name);
            else wsprintfA(descA, "%s %04X", mfr, prod);
            int j;
            for (j = 0; descA[j] && j < 127; j++) m->desc[j] = (WCHAR)(BYTE)descA[j];
            m->desc[j] = 0;
            wsprintfA(m->key, "%s:%04X:%u", mfr, prod, ser);
            sanitize_key(m->key);
        } else if (!name_is_generic(phys)) {
            lstrcpynW(m->desc, phys, 128);
            wsprintfA(m->key, "display%d", i + 1);
        } else {
            lstrcpynW(m->desc, L"External Display", 128);
            wsprintfA(m->key, "display%d", i + 1);
        }

        mon_count++;
    }
}

// Driven from the main loop. Bounded retry window (~40 s: fast rounds first,
// then 1.5 s pacing) so a fresh poke.exe run always fits inside it; recovery
// after that is via poke + Re-detect, resume re-detect, or display-change
// re-detect - never by looping forever here.
static void detect_pump() {
    if (g_disc_done) return;
    DWORD now = GetTickCount();
    DWORD pace = g_disc_rounds < 4 ? 300 : 1500;
    if (g_last_attempt && now - g_last_attempt < pace) return;
    g_last_attempt = now;

    int before = mon_count;
    discover_round();

    bool all = (g_harr.n > 0);
    for (int i = 0; i < g_harr.n && all; i++) {
        bool got = false;
        for (int k = 0; k < mon_count; k++)
            if (mons[k].hmon_tag == g_harr.a[i]) { got = true; break; }
        if (!got) all = false;
    }

    int grown = mon_count > before;
    if (all) {
        g_disc_done = true;
        fill_friendly_names();
        populate_ui();
    } else if (grown) {
        populate_ui(); // show partial results early
    } else if (g_disc_rounds >= 30) {
        g_disc_done = true;
        if (!g_populated) populate_ui(); // empty UI once; never pops up on its own
    }
}

// Tear down monitors + UI and restart the bounded detection rounds
// (used by manual re-detect, resume re-detect and display-change).
static void reset_detection() {
    free_all_monitors();
    if (g_populated) {
        teardown_ui();
        relayout();
    }
    g_disc_done = false;
    g_disc_rounds = 0;
    g_last_attempt = 0;
}

// ---------- debounced apply worker ----------

struct Job { int idx; BYTE code; DWORD val; };
static Job g_jobs[MAX_MON * 2];
static int g_njobs;

static DWORD WINAPI worker(LPVOID) {
    HANDLE ev[2] = { g_ev_apply, g_ev_quit };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) break;
        Job jobs[MAX_MON * 2];
        int n;
        EnterCriticalSection(&g_cs);
        CopyMemory(jobs, g_jobs, sizeof(jobs));
        n = g_njobs;
        LeaveCriticalSection(&g_cs);
        for (int j = 0; j < n; j++) {
            Monitor *m = &mons[jobs[j].idx];
            if (m->hphys && SetVCPFeature(m->hphys, jobs[j].code, jobs[j].val)) {
                if (jobs[j].code == VCP_BRIGHTNESS) m->la_bri = (int)jobs[j].val;
                else m->la_con = (int)jobs[j].val;
            }
            if (j + 1 < n) Sleep(WRITE_PACE_MS);
        }
    }
    return 0;
}

static void dispatch_jobs() {
    Job jobs[MAX_MON * 2];
    int n = 0;
    for (int i = 0; i < mon_count; i++) {
        if (mons[i].bri >= 0 && mons[i].bri != mons[i].la_bri) {
            jobs[n].idx = i; jobs[n].code = VCP_BRIGHTNESS; jobs[n].val = (DWORD)mons[i].bri; n++;
        }
        if (mons[i].con >= 0 && mons[i].con != mons[i].la_con) {
            jobs[n].idx = i; jobs[n].code = VCP_CONTRAST; jobs[n].val = (DWORD)mons[i].con; n++;
        }
    }
    if (!n) return;
    EnterCriticalSection(&g_cs);
    CopyMemory(g_jobs, jobs, sizeof(jobs));
    g_njobs = n;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_ev_apply);
}

// ---------- tray icon (drawn at runtime, no .ico resource) ----------

static HICON make_icon() {
    const int S = 32;
    HICON ic = NULL;
    HDC sdc = GetDC(NULL);
    HDC dc = sdc ? CreateCompatibleDC(sdc) : NULL;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = dc ? CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        ZeroMemory(bits, S * S * 4);
        int c = S / 2, r = S * 7 / 20;
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(96, 64, 0));
        HBRUSH br = CreateSolidBrush(RGB(255, 176, 32));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        Ellipse(dc, c - r, c - r, c + r + 1, c + r + 1);
        static const int dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int dy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        for (int i = 0; i < 8; i++) {
            int r1 = r + 2, r2 = c - 3;
            if (dx[i] && dy[i]) { r1 = r1 * 3 / 4; r2 = r2 * 3 / 4; }
            MoveToEx(dc, c + dx[i] * r1, c + dy[i] * r1, NULL);
            LineTo(dc, c + dx[i] * r2, c + dy[i] * r2);
        }
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DWORD *p = (DWORD *)bits;
        for (int i = 0; i < S * S; i++)
            if (p[i] & 0x00FFFFFF) p[i] |= 0xFF000000;
        ICONINFO ii;
        ZeroMemory(&ii, sizeof(ii));
        ii.fIcon = TRUE;
        ii.hbmColor = dib;
        ic = CreateIconIndirect(&ii);
        SelectObject(dc, old);
        DeleteObject(pen);
        DeleteObject(br);
        if (ic) g_icon_dib = dib;
    }
    if (dib && !ic) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    if (sdc) ReleaseDC(NULL, sdc);
    return ic ? ic : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
}

// ---------- UI helpers ----------

static void set_val_label(HWND lbl, int v) {
    wchar_t b[16];
    wsprintfW(b, L"%d", v);
    SetWindowTextW(lbl, b);
}

static void schedule_apply() {
    SetTimer(g_main, TIMER_APPLY, DEBOUNCE_MS, NULL);
    g_timer_on = true;
}

static HWND mk_static(int id, const wchar_t *txt, DWORD style, HFONT f) {
    HWND h = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style,
                             0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)f, MAKELPARAM(TRUE, 0));
    return h;
}

static HWND mk_track(int id, int hi, int pos) {
    HWND h = CreateWindowExW(0, TRACKBAR_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                             0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(h, TBM_SETRANGEMAX, TRUE, hi);
    SendMessageW(h, TBM_SETPOS, TRUE, pos);
    return h;
}

static void add_tip(HWND target, const wchar_t *txt) {
    TOOLINFOW ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = g_main;
    ti.uId = (UINT_PTR)target;
    ti.lpszText = (LPWSTR)txt;
    SendMessageW(g_tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static HFONT mk_font(int dpi, int pt, int weight) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static BOOL CALLBACK set_font_cb(HWND h, LPARAM) {
    int id = GetDlgCtrlID(h);
    HFONT f = (id >= ID_TITLE && id < ID_TITLE + MAX_MON) || id == ID_HEADER ? g_fnt_bold
            : (id == ID_HINT) ? g_fnt_small : g_fnt;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, MAKELPARAM(TRUE, 0));
    return TRUE;
}

static void apply_fonts() { EnumChildWindows(g_main, set_font_cb, 0); }

static void recreate_fonts(int dpi) {
    HFONT old[3] = { g_fnt, g_fnt_bold, g_fnt_small };
    g_fnt = mk_font(dpi, 9, FW_NORMAL);
    g_fnt_bold = mk_font(dpi, 9, FW_SEMIBOLD);
    g_fnt_small = mk_font(dpi, 8, FW_NORMAL);
    apply_fonts();
    for (int i = 0; i < 3; i++) if (old[i]) DeleteObject(old[i]);
}

// ---------- control logic (mirrors the Qt version) ----------

static void apply_sync_values(int v) {
    for (int i = 0; i < mon_count; i++) {
        int t = clampi(v + mons[i].off, 0, mons[i].bri_max);
        mons[i].bri = t;
        SendMessageW(mons[i].bri_scale, TBM_SETPOS, TRUE, t);
        set_val_label(mons[i].bri_val, t);
    }
}

static void set_mon_bri(Monitor *m, int t) {
    t = clampi(t, 0, m->bri_max);
    m->bri = t;
    g_updating = true;
    SendMessageW(m->bri_scale, TBM_SETPOS, TRUE, t);
    g_updating = false;
    set_val_label(m->bri_val, t);
}

static void set_bri_all(int v, bool use_off) {
    if (!g_populated || !mon_count) return;
    int sv = clampi(v, 0, 100);
    g_updating = true;
    SendMessageW(g_sync_scale, TBM_SETPOS, TRUE, sv);
    g_updating = false;
    set_val_label(g_sync_val, sv);
    for (int i = 0; i < mon_count; i++)
        set_mon_bri(&mons[i], v + (use_off ? mons[i].off : 0));
    schedule_apply();
}

static void on_bri_changed(int v, Monitor *m) {
    if (g_updating || !g_populated) return;
    g_updating = true;
    if (!m) {
        apply_sync_values(v);
        set_val_label(g_sync_val, v);
    } else {
        m->bri = v;
        set_val_label(m->bri_val, v);
        int sv = clampi(v, 0, 100);
        SendMessageW(g_sync_scale, TBM_SETPOS, TRUE, sv);
        set_val_label(g_sync_val, sv);
    }
    g_updating = false;
    schedule_apply();
}

static void on_con_changed(int v, Monitor *m) {
    if (g_updating || !g_populated) return;
    m->con = v;
    set_val_label(m->con_val, v);
    schedule_apply();
}

static void on_off_changed(Monitor *m) {
    if (!g_populated) return;
    char b[16];
    GetWindowTextA(m->off_edit, b, 16);
    m->off = atoi(b);
    save_conf();
    if (g_sync) {
        int v = (int)SendMessageW(g_sync_scale, TBM_GETPOS, 0, 0);
        g_updating = true;
        apply_sync_values(v);
        g_updating = false;
        schedule_apply();
    }
}

static void on_sync_toggled() {
    if (g_sync && g_populated && mon_count) {
        int v = (int)SendMessageW(g_sync_scale, TBM_GETPOS, 0, 0);
        g_updating = true;
        apply_sync_values(v);
        g_updating = false;
        schedule_apply();
    }
    if (g_populated) apply_visibility();
    relayout();
    save_conf();
}

static void apply_visibility() {
    if (!g_populated) {
        ShowWindow(g_sync_scale, SW_HIDE);
        ShowWindow(g_sync_val, SW_HIDE);
        ShowWindow(g_sync_hint, SW_HIDE);
        return;
    }
    int sv = (g_sync && mon_count > 0) ? SW_SHOW : SW_HIDE;
    ShowWindow(g_sync_scale, sv);
    ShowWindow(g_sync_val, sv);
    ShowWindow(g_sync_hint, sv);
    int bv = g_sync ? SW_HIDE : SW_SHOW;
    for (int i = 0; i < mon_count; i++) {
        if (!mons[i].bri_lbl) continue;
        ShowWindow(mons[i].bri_lbl, bv);
        ShowWindow(mons[i].bri_scale, bv);
        ShowWindow(mons[i].bri_val, bv);
    }
}

// ---------- layout (fixed sizes, scrollable when tall) ----------

static bool s_in_layout = false;

// WS_VISIBLE style bit, not IsWindowVisible(): a child of a hidden window
// reports invisible even when shown, which broke the first-populate layout.
static bool ctl_visible(HWND h) {
    return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
}

static void mv(HWND h, int x, int y, int w, int hh) {
    MoveWindow(h, x, y - g_scroll, w, hh, TRUE);
}

static void relayout() {
    if (!g_main || s_in_layout) return;
    s_in_layout = true;
    int dpi = get_dpi(g_main);
#define MUL(v) MulDiv(v, dpi, 96)
    RECT rc;
    GetClientRect(g_main, &rc);
    int cw = rc.right;
    if (cw < MUL(200)) cw = MUL(420);
    int M = MUL(10);
    int x = M, w = cw - 2 * M, y = M;

    mv(g_header, x, y, w, MUL(20));
    y += MUL(26);
    mv(g_sync_check, x, y, w, MUL(22));
    y += MUL(28);

    if (ctl_visible(g_sync_scale)) {
        int vw = MUL(44);
        mv(g_sync_scale, x, y, w - vw - MUL(8), MUL(26));
        mv(g_sync_val, x + w - vw, y + MUL(4), vw, MUL(18));
        y += MUL(28);
        mv(g_sync_hint, x, y, w, MUL(16));
        y += MUL(24);
    }

    for (int i = 0; i < mon_count; i++) {
        Monitor *m = &mons[i];
        if (!m->title) break;
        y += MUL(6);
        mv(m->title, x, y, w, MUL(18));
        y += MUL(22);
        int lw = MUL(84), vw = MUL(44);
        int sx = x + lw + MUL(8), sw = w - lw - vw - MUL(16);
        if (ctl_visible(m->bri_scale)) {
            mv(m->bri_lbl, x, y + MUL(4), lw, MUL(18));
            mv(m->bri_scale, sx, y, sw, MUL(26));
            mv(m->bri_val, sx + sw + MUL(8), y + MUL(4), vw, MUL(18));
            y += MUL(28);
        }
        mv(m->con_lbl, x, y + MUL(4), lw, MUL(18));
        mv(m->con_scale, sx, y, sw, MUL(26));
        mv(m->con_val, sx + sw + MUL(8), y + MUL(4), vw, MUL(18));
        y += MUL(28);
        mv(m->off_lbl, x, y + MUL(2), lw, MUL(18));
        mv(m->off_edit, sx, y, MUL(70), MUL(20));
        mv(m->off_ud, sx + MUL(70), y, MUL(17), MUL(20));
        y += MUL(28);
    }

    int bw = (w - 3 * MUL(6)) / 4;
    for (int i = 0; i < 4; i++)
        mv(g_btn[i], x + i * (bw + MUL(6)), y, bw, MUL(26));
    y += MUL(32);


    int need = y + M;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int maxH = (wa.bottom - wa.top) - MUL(16);
    int ch = need < maxH ? need : maxH;
    if (ch < MUL(120)) ch = MUL(120);

    bool showV = need > ch;
    if (!showV) g_scroll = 0;
    if (g_scroll > need - ch) g_scroll = need - ch;

    DWORD cur = (DWORD)GetWindowLongPtrW(g_main, GWL_STYLE);
    DWORD want = (cur & ~(DWORD)WS_VSCROLL) | (showV ? WS_VSCROLL : 0);
    UINT swpflags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    if (want != cur) {
        SetWindowLongPtrW(g_main, GWL_STYLE, (LONG_PTR)want);
        swpflags |= SWP_FRAMECHANGED;
    }
    int b = GetSystemMetrics(SM_CXBORDER);
    int ww = cw + 2 * b + (showV ? GetSystemMetrics(SM_CXVSCROLL) : 0);
    int wh = ch + 2 * b;
    SetWindowPos(g_main, NULL, 0, 0, ww, wh, swpflags);

    if (showV) {
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = need - 1;
        si.nPage = ch;
        si.nPos = g_scroll;
        SetScrollInfo(g_main, SB_VERT, &si, TRUE);
    }
    // force a full repaint so hidden/moved controls leave no ghosts
    InvalidateRect(g_main, NULL, TRUE);
    UpdateWindow(g_main);
#undef MUL
    s_in_layout = false;
}

static void scroll_by(int d) {
    if (!d || !g_main) return;
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(g_main, SB_VERT, &si)) return;
    int old = si.nPos;
    si.nPos += d;
    si.fMask = SIF_POS;
    SetScrollInfo(g_main, SB_VERT, &si, TRUE);
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    GetScrollInfo(g_main, SB_VERT, &si);
    if (si.nPos != old) {
        g_scroll = si.nPos;
        relayout();
    }
}

// ---------- tray flyout window ----------

static void place_near(POINT a) {
    RECT wr;
    GetWindowRect(g_main, &wr);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint(a, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int x = a.x - ww + 4;
    int y = a.y - wh - 4;
    if (a.y < mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 2) y = a.y + 4;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
    SetWindowPos(g_main, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void present() {
    if (g_anchor.x < 0) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_anchor.x = wa.right - 4;
        g_anchor.y = wa.bottom - 4;
    }
    g_scroll = 0;
    relayout();
    place_near(g_anchor);
    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
    SetActiveWindow(g_main);
}

static void hide_window() {
    ShowWindow(g_main, SW_HIDE);
    g_hide_tick = GetTickCount();
    trim_mem();
}

static void teardown_ui() {
    for (int i = 0; i < MAX_MON; i++) {
        Monitor *m = &mons[i];
        HWND hs[10] = { m->title, m->bri_lbl, m->bri_scale, m->bri_val,
                        m->con_lbl, m->con_scale, m->con_val,
                        m->off_lbl, m->off_edit, m->off_ud };
        for (int k = 0; k < 10; k++)
            if (hs[k]) DestroyWindow(hs[k]);
        m->title = m->bri_lbl = m->bri_scale = m->bri_val = NULL;
        m->con_lbl = m->con_scale = m->con_val = NULL;
        m->off_lbl = m->off_edit = m->off_ud = NULL;
    }
    g_populated = false;
}

static void populate_ui() {
    if (g_populated) teardown_ui();

    if (mon_count == 0) {
        g_populated = true;
        relayout();
        if (!g_bg_detect || IsWindowVisible(g_main)) present();
        SetTimer(g_main, TIMER_TRIM, 1500, NULL);
        return;
    }

    load_conf_offsets();

    for (int i = 0; i < mon_count; i++) {
        Monitor *m = &mons[i];
        wchar_t t[200];
        wsprintfW(t, L"%d:  %s", i + 1, m->desc);
        m->title = mk_static(ID_TITLE + i, t, 0, g_fnt_bold);

        m->bri_lbl = mk_static(0, L"Brightness:", 0, g_fnt);
        m->bri_scale = mk_track(ID_BRI + i, m->bri_max, m->bri);
        m->bri_val = mk_static(0, L"-", SS_RIGHT, g_fnt);
        set_val_label(m->bri_val, m->bri);

        m->con_lbl = mk_static(0, L"Contrast:", 0, g_fnt);
        m->con_scale = mk_track(ID_CON + i, m->con_max, m->con);
        m->con_val = mk_static(0, L"-", SS_RIGHT, g_fnt);
        set_val_label(m->con_val, m->con);

        m->off_lbl = mk_static(0, L"Offset:", 0, g_fnt);
        m->off_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, g_main, (HMENU)(INT_PTR)(ID_OFF + i), g_inst, NULL);
        SendMessageW(m->off_edit, WM_SETFONT, (WPARAM)g_fnt, MAKELPARAM(TRUE, 0));
        m->off_ud = CreateWindowExW(0, UPDOWN_CLASS, L"",
                                    WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ARROWKEYS,
                                    0, 0, 10, 10, g_main, NULL, g_inst, NULL);
        SendMessageW(m->off_ud, UDM_SETBUDDY, (WPARAM)m->off_edit, 0);
        SendMessageW(m->off_ud, UDM_SETRANGE32, (WPARAM)-100, (LPARAM)100);
        SendMessageW(m->off_ud, UDM_SETPOS32, 0, (LPARAM)m->off);
        char b[16];
        GetWindowTextA(m->off_edit, b, 16);
        m->off = atoi(b);
        add_tip(m->off_edit,
            L"Added to the sync value for this monitor.\n"
            L"E.g. -10 on a brighter panel so all match.");
    }

    g_populated = true;
    g_updating = true;
    int sv = clampi(mons[0].bri, 0, 100);
    SendMessageW(g_sync_scale, TBM_SETPOS, TRUE, sv);
    set_val_label(g_sync_val, sv);
    g_updating = false;

    apply_visibility();
    relayout();
    if (!g_bg_detect || IsWindowVisible(g_main)) present();
    g_bg_detect = false; // next manual/first detection shows the window again
    SetTimer(g_main, TIMER_TRIM, 1500, NULL);
}

// ---------- tray ----------

static void add_tray() {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, L"Display Controls", 64);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void tray_menu(int x, int y) {
    POINT pt = { x, y };
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (pt.x < mi.rcWork.left) pt.x = mi.rcWork.left;
        if (pt.x > mi.rcWork.right - 4) pt.x = mi.rcWork.right - 4;
        if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;
        if (pt.y > mi.rcWork.bottom - 4) pt.y = mi.rcWork.bottom - 4;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Display Controls");
    AppendMenuW(m, MF_STRING, 2, L"Re-detect monitors");
    AppendMenuW(m, MF_STRING, 3, L"About");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 4, L"Quit");
    SetForegroundWindow(g_main);
    int c = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                pt.x, pt.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (c == 1) present();
    else if (c == 2) {
        g_bg_detect = false; // manual: show the Detecting state and window
        start_poke();
        reset_detection();
    }
    else if (c == 3) {
        MessageBoxW(g_main,
            L"MonCtrl 1.0\n\n"
            L"Native Win32 tray app controlling monitor brightness/contrast "
            L"via DDC/CI (dxva2 backend, PowerDisplay-style pipeline).\n\n"
            L"Config: %APPDATA%\\ddc-tray.conf\n"
            L"Left-click: open  |  click away: close\n\n"
            L"Contact: nice.ege.cool@gmail.com",
            L"About MonCtrl", MB_OK | MB_ICONINFORMATION);
    }
    else if (c == 4) do_quit();
}

// ---------- window ----------

static void do_quit() {
    if (g_timer_on) {
        KillTimer(g_main, TIMER_APPLY);
        g_timer_on = false;
        dispatch_jobs();
    }
    save_conf();
    SetEvent(g_ev_quit);
    if (g_worker) WaitForSingleObject(g_worker, 2000);
    stop_poke();
    free_all_monitors();
    DestroyWindow(g_main);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CLOSE:
        hide_window();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && IsWindowVisible(h)) hide_window();
        break;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_SYNCCHK && code == BN_CLICKED) {
            g_sync = SendMessageW(g_sync_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            on_sync_toggled();
        } else if (id >= ID_BTN && id < ID_BTN + 4) {
            int k = id - ID_BTN;
            set_bri_all((k == 1 || k == 3) ? 100 : 0, k < 2);
        } else if (id >= ID_OFF && id < ID_OFF + MAX_MON && code == EN_CHANGE) {
            on_off_changed(&mons[id - ID_OFF]);
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND ctl = (HWND)l;
        if (!ctl) break;
        int id = GetDlgCtrlID(ctl);
        int v = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
        if (id == ID_SYNCSCALE) on_bri_changed(v, NULL);
        else if (id >= ID_BRI && id < ID_BRI + MAX_MON) on_bri_changed(v, &mons[id - ID_BRI]);
        else if (id >= ID_CON && id < ID_CON + MAX_MON) on_con_changed(v, &mons[id - ID_CON]);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        if (!GetScrollInfo(h, SB_VERT, &si)) break;
        int old = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: si.nPos -= 16; break;
        case SB_LINEDOWN: si.nPos += 16; break;
        case SB_PAGEUP: si.nPos -= (int)si.nPage; break;
        case SB_PAGEDOWN: si.nPos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: si.nPos = si.nTrackPos; break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(h, SB_VERT, &si);
        if (si.nPos != old) {
            g_scroll = si.nPos;
            relayout();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = (int)(SHORT)HIWORD(w);
        scroll_by(-delta / 40);
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_APPLY) {
            KillTimer(h, TIMER_APPLY);
            g_timer_on = false;
            dispatch_jobs();
        } else if (w == TIMER_TRIM) {
            KillTimer(h, TIMER_TRIM);
            trim_mem();
        } else if (w == TIMER_RESUME) {
            KillTimer(h, TIMER_RESUME);
            // displays need a moment after resume; re-poke and rediscover
            start_poke();
            reset_detection();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            g_bg_detect = true; // silent: re-detect without showing the window
            SetTimer(h, TIMER_RESUME, 2500, NULL);
        }
        return TRUE;
    case WM_APP_TRAY: {
        UINT ev = LOWORD(l);
        int x = GET_X_LPARAM(w), y = GET_Y_LPARAM(w);
        if (ev == WM_CONTEXTMENU) {
            g_anchor.x = x;
            g_anchor.y = y;
            tray_menu(x, y);
        } else if (ev == NIN_SELECT || ev == NIN_KEYSELECT ||
                   ev == WM_LBUTTONUP || ev == WM_MBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            // v4 shells can deliver NIN_SELECT *and* WM_LBUTTONUP for one click,
            // so never toggle here: open-only (click-away closes the flyout)
            if (IsWindowVisible(g_main)) {
                SetForegroundWindow(g_main);
            } else if (GetTickCount() - g_hide_tick >= 300) {
                if (x || y) {
                    g_anchor.x = x;
                    g_anchor.y = y;
                } else {
                    GetCursorPos(&g_anchor);
                }
                present();
            }
        }
        return 0;
    }
    case WM_APP_SHOW:
        present();
        return 0;
    case WM_CTLCOLORSTATIC:
        if (g_dark) {
            SetBkColor((HDC)w, RGB(32, 32, 32));
            SetTextColor((HDC)w, RGB(240, 240, 240));
            return (LRESULT)g_br_dark;
        }
        SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CTLCOLORBTN:
        if (g_dark) {
            SetBkColor((HDC)w, RGB(32, 32, 32));
            SetTextColor((HDC)w, RGB(240, 240, 240));
            return (LRESULT)g_br_dark;
        }
        break;
    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((const wchar_t *)l, L"ImmersiveColorSet") == 0) {
            apply_theme();
            relayout();
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        recreate_fonts(get_dpi(h));
        relayout();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        g_bg_detect = true; // silent rediscovery on topology change
        reset_detection();
        return 0;
    case WM_DEVICECHANGE:
        if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE) {
            g_bg_detect = true; // silent: monitor (un)plugged or signal lost
            start_poke();
            reset_detection();
        }
        return TRUE;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = h;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    default:
        if (msg == g_msg_tbcreated) {
            add_tray();
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void round_corners(HWND h) {
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    HRESULT (WINAPI *fn)(HWND, DWORD, LPCVOID, DWORD) =
        (HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD))GetProcAddress(d, "DwmSetWindowAttribute");
    if (fn) {
        INT_PTR pref = 2; // DWMWCP_ROUND
        fn(h, 33, &pref, sizeof(pref)); // DWMWA_WINDOW_CORNER_PREFERENCE
    }
    FreeLibrary(d);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    HANDLE single = CreateEventW(NULL, TRUE, FALSE, L"Local\\MonCtrl2.SingleInstance");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND ex = FindWindowW(g_cls, NULL);
        if (ex) SendMessageW(ex, WM_APP_SHOW, 0, 0);
        return 0;
    }

    set_dpi_awareness();
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    init_conf();
    InitializeCriticalSection(&g_cs);
    g_ev_apply = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ev_quit = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    g_msg_tbcreated = RegisterWindowMessageW(L"TaskbarCreated");

    // embedded icon (app.rc), fallback to runtime-drawn
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                               LR_DEFAULTCOLOR);
    if (!g_icon) g_icon = make_icon();

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = g_cls;
    wc.hIcon = g_icon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, g_cls, L"Display Controls",
                             WS_POPUP | WS_BORDER,
                             CW_USEDEFAULT, CW_USEDEFAULT, 420, 200,
                             NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    round_corners(g_main);
    apply_theme();

    recreate_fonts(get_dpi(g_main));

    g_header = mk_static(ID_HEADER, L"Display Controls", 0, g_fnt_bold);
    g_sync_check = CreateWindowExW(0, L"BUTTON", L"Sync all displays (brightness)",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, 10, 10, g_main, (HMENU)(INT_PTR)ID_SYNCCHK, inst, NULL);
    g_sync_scale = mk_track(ID_SYNCSCALE, 100, 50);
    g_sync_val = mk_static(0, L"-", SS_RIGHT, g_fnt);
    g_sync_hint = mk_static(ID_HINT, L"per-monitor offsets apply", 0, g_fnt_small);

    static const wchar_t *btxt[4] = { L"Min +off", L"Max +off", L"Min abs", L"Max abs" };
    static const wchar_t *btip[4] = {
        L"All monitors to minimum, with per-monitor offsets applied",
        L"All monitors to maximum, with per-monitor offsets applied",
        L"All monitors to absolute minimum (ignores offsets)",
        L"All monitors to absolute maximum (ignores offsets)" };
    for (int i = 0; i < 4; i++) {
        g_btn[i] = CreateWindowExW(0, L"BUTTON", btxt[i],
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 10, 10, g_main, (HMENU)(INT_PTR)(ID_BTN + i), inst, NULL);
    }

    g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, inst, NULL);
    SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, 320);

    g_sync = load_conf_sync();
    SendMessageW(g_sync_check, BM_SETCHECK, g_sync ? BST_CHECKED : BST_UNCHECKED, 0);

    apply_fonts();
    apply_theme();
    apply_visibility();
    relayout();
    for (int i = 0; i < 4; i++) add_tip(g_btn[i], btip[i]);
    add_tray();
    register_monitor_notify();
    start_poke();

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            if (!IsDialogMessageW(g_main, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        detect_pump();
        Sleep(30);
    }
done:
    if (g_devnotify) UnregisterDeviceNotification(g_devnotify);
    if (g_icon) DestroyIcon(g_icon);
    if (g_icon_dib) DeleteObject(g_icon_dib);
    return 0;
}

