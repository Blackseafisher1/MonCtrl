// installer - tiny native installer for MonCtrl.
// Pick a folder (auto-appends "\MonCtrl" unless the folder is already named
// MonCtrl), copies MonCtrl.exe and poke.exe from the installer's own folder.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

static bool pick_folder(HWND owner, wchar_t *out, DWORD cap) {
    IFileDialog *dlg = NULL;
    bool ok = false;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose install location");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                lstrcpynW(out, path, (int)cap);
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool file_exists(const wchar_t *p) {
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;

    wchar_t self[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    lstrcpynW(dir, self, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    wchar_t src_exe[MAX_PATH], src_poke[MAX_PATH];
    wsprintfW(src_exe, L"%s\\MonCtrl.exe", dir);
    wsprintfW(src_poke, L"%s\\poke.exe", dir);
    if (!file_exists(src_exe) || !file_exists(src_poke)) {
        MessageBoxW(NULL, L"MonCtrl.exe and poke.exe must be next to the installer.",
                    L"Installer", MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t target[MAX_PATH] = L"";
    if (!pick_folder(NULL, target, MAX_PATH)) return 0;

    // append \MonCtrl unless the chosen folder is already named MonCtrl
    size_t len = wcslen(target);
    while (len > 3 && target[len - 1] == L'\\') { target[--len] = 0; }
    const wchar_t *last = wcsrchr(target, L'\\');
    last = last ? last + 1 : target;
    if (_wcsicmp(last, L"MonCtrl") != 0) {
        if (len + 9 >= MAX_PATH) {
            MessageBoxW(NULL, L"Path too long.", L"Installer", MB_OK | MB_ICONERROR);
            return 1;
        }
        lstrcatW(target, L"\\MonCtrl");
    }

    int cr = SHCreateDirectoryExW(NULL, target, NULL);
    if (cr != ERROR_SUCCESS && cr != ERROR_ALREADY_EXISTS && cr != ERROR_FILE_EXISTS) {
        MessageBoxW(NULL, L"Could not create the target folder.", L"Installer",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t dst_exe[MAX_PATH], dst_poke[MAX_PATH];
    wsprintfW(dst_exe, L"%s\\MonCtrl.exe", target);
    wsprintfW(dst_poke, L"%s\\poke.exe", target);

    if (!CopyFileW(src_exe, dst_exe, FALSE) || !CopyFileW(src_poke, dst_poke, FALSE)) {
        MessageBoxW(NULL, L"Copying files failed (target in use or no access?).",
                    L"Installer", MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t msg[MAX_PATH + 80];
    wsprintfW(msg, L"Installed to:\n%s\n\nStart MonCtrl now?", target);
    if (MessageBoxW(NULL, msg, L"Installer", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        if (CreateProcessW(dst_exe, NULL, NULL, NULL, FALSE, 0, NULL, target, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    CoUninitialize();
    return 0;
}
