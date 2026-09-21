// poke - keeps the session's DDC/CI path warm for MonCtrl.
// On some systems GetPhysicalMonitorsFromHMONITOR returns NULL handles forever
// until another process performs open/destroy cycles on the monitor handles;
// this helper does exactly that every ~2 s for MonCtrl's lifetime.
// Console subsystem, user32+dxva2 only - the process shape that works.

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <physicalmonitorenumerationapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dxva2.lib")

#define MAX_MON 8

struct Ctx { HMONITOR a[MAX_MON]; int n; };

static BOOL CALLBACK cb(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    Ctx *c = (Ctx *)lp;
    if (c->n < MAX_MON) c->a[c->n++] = hm;
    return TRUE;
}

int main(void) {
    // bounded run: ~12 s of open/destroy cycles, then exit
    for (int round = 0; round < 10; round++) {
        Ctx c;
        c.n = 0;
        EnumDisplayMonitors(NULL, NULL, cb, (LPARAM)&c);
        for (int i = 0; i < c.n; i++) {
            DWORD n = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(c.a[i], &n) || n == 0) continue;
            if (n > MAX_MON) n = MAX_MON;
            PHYSICAL_MONITOR pm[MAX_MON];
            ZeroMemory(pm, sizeof(pm));
            if (!GetPhysicalMonitorsFromHMONITOR(c.a[i], n, pm)) continue;
            Sleep(50);
            for (DWORD k = 0; k < n; k++)
                if (pm[k].hPhysicalMonitor) DestroyPhysicalMonitor(pm[k].hPhysicalMonitor);
        }
        Sleep(1200);
    }
    return 0;
}
