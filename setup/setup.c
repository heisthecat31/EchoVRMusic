/* EchoVRMusic Setup: installs EchoVRMusic into Echo VR, edits its settings live, and explains it.
 *
 * Install
 *   <Echo>\bin\win10\plugins\            created if missing
 *   <Echo>\bin\win10\dbgcore.dll         the plugin loader (loads every DLL in plugins\):
 *     missing                         -> downloaded (EchoXR Hands release) and installed
 *     already a plugin loader         -> kept (it names the plugins folder)
 *     anything else                   -> moved to plugins\dbgcoreoriginal.dll, loader installed
 *   <Echo>\bin\win10\plugins\EchoVRMusic.dll   carried inside this exe
 *
 * Settings live in HKCU\Software\EchoVRMusic as strings. The plugin re-reads them within half a
 * second, so every change here applies in game straight away.
 *
 * Command line (used by Spark):
 *   --dir <folder>        Echo's bin\win10 folder, instead of searching for it
 *   --install --silent    install without a window; exit code 0 ok, 1 no Echo VR there,
 *                         2 Echo is running, 3 loader download failed, 4 couldn't write files.
 *                         The steps go to %TEMP%\EchoVRMusicSetup.log.
 *   --theme <colours>     bg=#rrggbb,surface=..,line=..,text=..,muted=..,accent=.. (also
 *                         sent while running as WM_COPYDATA, dwData 'EVMT')
 *   --embed <hwnd>        open as a borderless child of that window (Spark's Music tab); the
 *                         host moves and sizes it, and it closes when the host goes away
 *
 * The window is drawn by hand (GDI): three text tabs, then plain rows with switches and sliders.
 * Hit areas are collected while painting and used for the mouse.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <urlmon.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include "setup_res.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "advapi32.lib")

#define REG_KEY        L"Software\\EchoVRMusic"
#define RELEASE_API    L"https://api.github.com/repos/heisthecat31/EchoXR-Hands/releases/latest"
#define RELEASE_FALLBACK L"https://github.com/heisthecat31/EchoXR-Hands/releases/download/v0.3.0/EchoXR-Hands-v0.3.0.zip"
#define LOADER_IN_ZIP  "EchoXR/Hands/install/dbgcore.dll"

/* ---- colours --------------------------------------------------------------------------------- */
/* Variables, not constants: Spark passes its theme (--theme, or WM_COPYDATA while running). */
static COLORREF C_BG      = RGB(16, 17, 23);
static COLORREF C_SURFACE = RGB(28, 30, 39);
static COLORREF C_LINE    = RGB(38, 41, 52);
static COLORREF C_TEXT    = RGB(236, 237, 242);
static COLORREF C_MUTED   = RGB(128, 133, 150);
static COLORREF C_ACCENT  = RGB(124, 92, 255);
static COLORREF C_ACCENT2 = RGB(0, 190, 255);
#define C_OK       RGB(76, 209, 145)
#define C_WARN     RGB(245, 182, 82)
#define C_ERR      RGB(255, 99, 102)
#define THEME_MSG  0x45564D54   /* WM_COPYDATA dwData: "EVMT", the data is a --theme string */

static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

/* "bg=#101117,surface=#1c1e27,line=#262934,text=#ecedf2,muted=#808596,accent=#7c5cff".
 * Missing keys keep their colour. With one accent, the gradient's second colour is a lighter step of it. */
static void ApplyTheme(const wchar_t* s) {
    BOOL accent2 = FALSE;
    while (s && *s) {
        const wchar_t* eq = wcschr(s, L'=');
        const wchar_t* end = wcspbrk(s, L",;");
        unsigned v;
        if (!end) end = s + wcslen(s);
        if (eq && eq < end && eq[1] == L'#' && swscanf(eq + 2, L"%6x", &v) == 1) {
            COLORREF col = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
            size_t k = (size_t)(eq - s);
            if (!_wcsnicmp(s, L"bg", k) && k == 2) C_BG = col;
            else if (!_wcsnicmp(s, L"surface", k) && k == 7) C_SURFACE = col;
            else if (!_wcsnicmp(s, L"line", k) && k == 4) C_LINE = col;
            else if (!_wcsnicmp(s, L"text", k) && k == 4) C_TEXT = col;
            else if (!_wcsnicmp(s, L"muted", k) && k == 5) C_MUTED = col;
            else if (!_wcsnicmp(s, L"accent", k) && k == 6) C_ACCENT = col;
            else if (!_wcsnicmp(s, L"accent2", k) && k == 7) { C_ACCENT2 = col; accent2 = TRUE; }
        }
        s = *end ? end + 1 : end;
    }
    if (!accent2) C_ACCENT2 = Mix(C_ACCENT, C_TEXT, 0.35);
}

/* ---- state ------------------------------------------------------------------------------------ */
enum { K_TOGGLE, K_SLIDER };

typedef struct {
    const wchar_t* reg;     /* registry value name (what the plugin reads) */
    const wchar_t* label;
    const wchar_t* hint;    /* one short line, or NULL */
    int kind;
    double min, max, step, def;
    const wchar_t* fmt;     /* value text; "%%" in it shows the value as a percentage */
    int column;
    double val;
} Setting;

static Setting g_set[] = {   /* last field before val: the column (0 speakers, 1 options, 2 advanced) */
    {L"Enabled",    L"Music on the speakers", NULL, K_TOGGLE, 0, 1, 1, 1, 0, 0},
    {L"Gain",       L"Volume", NULL, K_SLIDER, 0, 2, 0.05, 1, L"%.0f%%", 0},
    {L"Cutoff",     L"Silent beyond", L"Speakers fade out by this distance", K_SLIDER, 5, 150, 1, 30, L"%.0f m", 0},
    {L"FullVolume", L"Full volume within", NULL, K_SLIDER, 0, 20, 0.5, 3, L"%.1f m", 0},
    {L"Nearest",    L"Nearest speakers only", L"Only the closest ones play", K_SLIDER, 0, 10, 1, 0, L"%.0f", 0},
    {L"MuteStock",  L"Pause Echo's own music", NULL, K_TOGGLE, 0, 1, 1, 1, 0, 1},
    {L"Shaping",    L"Distance fade", NULL, K_TOGGLE, 0, 1, 1, 1, 0, 1},
    {L"Stereo",     L"Stereo", L"Speakers alternate left and right", K_TOGGLE, 0, 1, 1, 1, 0, 1},
    {L"Follow2D",   L"Follow me on other maps", L"Maps without speakers", K_TOGGLE, 0, 1, 1, 1, 0, 1},
    {L"Scale",      L"Echo's fade range", NULL, K_SLIDER, 0.25, 4, 0.05, 1, L"%.2fx", 2},
    {L"LatencyMs",  L"Buffer", L"Raise it if the music crackles", K_SLIDER, 40, 400, 10, 120, L"%.0f ms", 2},
};
#define NSET (int)(sizeof g_set / sizeof g_set[0])

typedef struct { RECT rc; int id; } Hot;
enum {
    H_NONE = 0, H_NAV0 = 1, /* H_NAV0 + page */
    H_BROWSE = 10, H_DETECT, H_INSTALL, H_UNINSTALL, H_OPEN, H_RESET, H_SRC_SYSTEM, H_SRC_APP, H_HELP, H_CLOSE,
    H_SETTING0 = 100 /* + index */
};

typedef struct { wchar_t text[300]; COLORREF col; } LogLine;

static HINSTANCE g_inst;
static HWND g_wnd, g_path, g_app;
static int g_hover = H_NONE, g_drag = -1, g_scroll = 0, g_scrollMax = 0;
static BOOL g_help;   /* the help panel is open */
static double g_dpi = 1.0;
static Hot g_hot[128];
static int g_nhot;
static HFONT g_fTitle, g_fH, g_fBody, g_fSmall, g_fTab, g_fBrand;
static HBRUSH g_brEdit;
static wchar_t g_source[260] = L"system";
static LogLine g_log[64];
static int g_nlog;
static CRITICAL_SECTION g_logLock;
static volatile LONG g_busy;
static BOOL g_silent;                 /* --install --silent: no window */
static HWND g_host;                   /* --embed: the window we live in */
static wchar_t g_argDir[MAX_PATH];    /* --dir */
static wchar_t g_argTheme[512];       /* --theme */
static int g_result;                  /* install exit code (see the top of the file) */

static int S(int v) { return (int)(v * g_dpi + 0.5); }

/* ---- small helpers ---------------------------------------------------------------------------- */
static BOOL FileExists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static BOOL DirExists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void Join(wchar_t* out, size_t n, const wchar_t* a, const wchar_t* b) {
    _snwprintf(out, n, L"%s%s%s", a, (a[0] && a[wcslen(a) - 1] != L'\\') ? L"\\" : L"", b);
    out[n - 1] = 0;
}

static BYTE* ReadAll(const wchar_t* path, DWORD* size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE* buf; DWORD n = 0;
    *size = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(h, NULL);
    buf = (BYTE*)malloc(*size + 2);
    if (buf && !ReadFile(h, buf, *size, &n, NULL)) { free(buf); buf = NULL; }
    CloseHandle(h);
    if (buf) { buf[*size] = 0; buf[*size + 1] = 0; }
    return buf;
}

static BOOL WriteAll(const wchar_t* path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n = 0; BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, data, size, &n, NULL) && n == size;
    CloseHandle(h);
    return ok;
}

static const void* PluginResource(DWORD* size) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_PLUGIN), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    *size = r ? SizeofResource(g_inst, r) : 0;
    return g ? LockResource(g) : NULL;
}

static BOOL EchoRunning(void) {
    PROCESSENTRY32W pe = {sizeof pe};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    BOOL found = FALSE, ok;
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    for (ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"echovr.exe") || !_wcsicmp(pe.szExeFile, L"echovr_openxr.exe")) { found = TRUE; break; }
    CloseHandle(snap);
    return found;
}

/* A plugin loader names the folder it loads from. */
static BOOL MentionsPlugins(const BYTE* b, DWORD n) {
    DWORD i;
    for (i = 0; i + 14 <= n; i++) {
        if (!memcmp(b + i, "plugins", 7)) return TRUE;
        if (!memcmp(b + i, "p\0l\0u\0g\0i\0n\0s\0", 14)) return TRUE;
    }
    return FALSE;
}

/* ---- log -------------------------------------------------------------------------------------- */
static void AddLog(COLORREF col, const wchar_t* fmt, ...) {
    va_list ap;
    EnterCriticalSection(&g_logLock);
    if (g_nlog == (int)(sizeof g_log / sizeof g_log[0])) { memmove(g_log, g_log + 1, sizeof g_log - sizeof g_log[0]); g_nlog--; }
    va_start(ap, fmt);
    _vsnwprintf(g_log[g_nlog].text, 299, fmt, ap);
    va_end(ap);
    g_log[g_nlog].text[299] = 0;
    g_log[g_nlog].col = col;
    g_nlog++;
    if (g_silent) {
        wchar_t path[MAX_PATH];
        FILE* f;
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, MAX_PATH, L"EchoVRMusicSetup.log");
        if ((f = _wfopen(path, L"a, ccs=UTF-8")) != NULL) { fwprintf(f, L"%s\n", g_log[g_nlog - 1].text); fclose(f); }
    }
    LeaveCriticalSection(&g_logLock);
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- registry --------------------------------------------------------------------------------- */
static BOOL RegGet(const wchar_t* name, wchar_t* out, DWORD chars) {
    DWORD bytes = chars * sizeof(wchar_t), type = 0;
    out[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, &type, out, &bytes) != ERROR_SUCCESS) return FALSE;
    return TRUE;
}
static void RegPut(const wchar_t* name, const wchar_t* value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void LoadSettings(void) {
    int i; wchar_t buf[260];
    for (i = 0; i < NSET; i++) g_set[i].val = RegGet(g_set[i].reg, buf, 260) ? _wtof(buf) : g_set[i].def;
    if (!RegGet(L"Source", g_source, 260) || !g_source[0]) wcscpy(g_source, L"system");
}

static void SaveSetting(int i) {
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%g", g_set[i].val);
    RegPut(g_set[i].reg, buf);
}

static void ResetSettings(void) {
    int i;
    for (i = 0; i < NSET; i++) RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, g_set[i].reg);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Source");
    LoadSettings();
    if (g_app) SetWindowTextW(g_app, L"spotify.exe");
}

/* ---- game folder ------------------------------------------------------------------------------ */
static BOOL IsGameDir(const wchar_t* dir) {
    wchar_t p[MAX_PATH];
    Join(p, MAX_PATH, dir, L"echovr.exe");
    return FileExists(p);
}

static BOOL DetectGameDir(wchar_t* out) {
    static const wchar_t* tails[] = {
        L"Oculus\\Games\\Software\\Software\\ready-at-dawn-echo-arena\\bin\\win10",
        L"Oculus\\Software\\Software\\ready-at-dawn-echo-arena\\bin\\win10",
        L"Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena\\bin\\win10",
        L"Oculus Apps\\Software\\ready-at-dawn-echo-arena\\bin\\win10",
        L"ready-at-dawn-echo-arena\\bin\\win10",
        L"Echo VR\\bin\\win10", L"EchoVR\\bin\\win10", L"Games\\EchoVR\\bin\\win10",
    };
    wchar_t p[MAX_PATH], buf[MAX_PATH];
    HKEY libs;
    int d, t;
    if (RegGet(L"InstallDir", buf, MAX_PATH) && IsGameDir(buf)) { wcscpy(out, buf); return TRUE; }
    /* Oculus libraries: HKCU\Software\Oculus VR, LLC\Oculus\Libraries\{id}\OriginalPath */
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Oculus VR, LLC\\Oculus\\Libraries", 0, KEY_READ, &libs) == ERROR_SUCCESS) {
        wchar_t sub[128]; DWORD i;
        for (i = 0;; i++) {
            DWORD n = 128, bytes = sizeof buf;
            if (RegEnumKeyExW(libs, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (RegGetValueW(libs, sub, L"OriginalPath", RRF_RT_REG_SZ, NULL, buf, &bytes) == ERROR_SUCCESS) {
                Join(p, MAX_PATH, buf, L"Software\\ready-at-dawn-echo-arena\\bin\\win10");
                if (IsGameDir(p)) { wcscpy(out, p); RegCloseKey(libs); return TRUE; }
            }
        }
        RegCloseKey(libs);
    }
    for (d = 'C'; d <= 'Z'; d++) {
        wchar_t root[4] = {(wchar_t)d, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        for (t = 0; t < (int)(sizeof tails / sizeof tails[0]); t++) {
            Join(p, MAX_PATH, root, tails[t]);
            if (IsGameDir(p)) { wcscpy(out, p); return TRUE; }
        }
    }
    return FALSE;
}

/* ---- install status ----------------------------------------------------------------------------- */
enum { LD_MISSING, LD_LOADER, LD_OTHER };
typedef struct { BOOL game, running, plugins, plugin, pluginCurrent; int loader; } Status;
static Status g_st;

static void GetGameDir(wchar_t* out) {
    if (g_path) GetWindowTextW(g_path, out, MAX_PATH);
    else wcscpy_s(out, MAX_PATH, g_argDir);
    while (out[0] && (out[wcslen(out) - 1] == L'\\' || out[wcslen(out) - 1] == L' ')) out[wcslen(out) - 1] = 0;
}

static void RefreshStatus(void) {
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    DWORD n, rn; BYTE* b; const void* res;
    GetGameDir(dir);
    memset(&g_st, 0, sizeof g_st);
    g_st.game = IsGameDir(dir);
    g_st.running = EchoRunning();
    Join(p, MAX_PATH, dir, L"plugins");
    g_st.plugins = DirExists(p);
    Join(p, MAX_PATH, dir, L"dbgcore.dll");
    b = ReadAll(p, &n);
    g_st.loader = !b ? LD_MISSING : MentionsPlugins(b, n) ? LD_LOADER : LD_OTHER;
    free(b);
    Join(p, MAX_PATH, dir, L"plugins\\EchoVRMusic.dll");
    b = ReadAll(p, &n);
    res = PluginResource(&rn);
    g_st.plugin = b != NULL;
    g_st.pluginCurrent = b && res && n == rn && !memcmp(b, res, n);
    free(b);
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- install ---------------------------------------------------------------------------------- */
static BOOL RunHidden(wchar_t* cmd) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

/* Downloads the newest EchoXR Hands release and takes the plugin loader out of it. */
static BOOL DownloadLoader(wchar_t* outPath) {
    wchar_t tmp[MAX_PATH], work[MAX_PATH], json[MAX_PATH], zip[MAX_PATH], url[1024], cmd[2048], sys[MAX_PATH];
    DWORD n; BYTE* j;
    GetTempPathW(MAX_PATH, tmp);
    Join(work, MAX_PATH, tmp, L"EchoVRMusicSetup");
    CreateDirectoryW(work, NULL);
    Join(json, MAX_PATH, work, L"release.json");
    Join(zip, MAX_PATH, work, L"EchoXR-Hands.zip");
    wcscpy(url, RELEASE_FALLBACK);
    AddLog(C_MUTED, L"Looking up the newest plugin loader (EchoXR Hands release)...");
    if (URLDownloadToFileW(NULL, RELEASE_API, json, 0, NULL) == S_OK && (j = ReadAll(json, &n)) != NULL) {
        char* s = strstr((char*)j, "\"browser_download_url\"");
        while (s) {
            char* q = strchr(s + 22, '"'), *e;
            if (!q) break;
            e = strchr(q + 1, '"');
            if (e && strstr(q, "EchoXR-Hands-v") && strstr(q, "EchoXR-Hands-v") < e && e - q > 5 && !strncmp(e - 4, ".zip", 4)) {
                int len = (int)(e - q - 1);
                MultiByteToWideChar(CP_UTF8, 0, q + 1, len, url, 1023);
                url[len < 1023 ? len : 1023] = 0;
                break;
            }
            s = strstr(e ? e : q + 1, "\"browser_download_url\"");
        }
        free(j);
    }
    AddLog(C_MUTED, L"Downloading %s", wcsrchr(url, L'/') + 1);
    if (URLDownloadToFileW(NULL, url, zip, 0, NULL) != S_OK) { AddLog(C_ERR, L"Download failed. Check your internet connection."); return FALSE; }
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, 2048, L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\" %S", sys, zip, work, LOADER_IN_ZIP);
    cmd[2047] = 0;
    if (!RunHidden(cmd)) { AddLog(C_ERR, L"Couldn't unpack the loader from the download (tar.exe)."); return FALSE; }
    Join(outPath, MAX_PATH, work, L"EchoXR\\Hands\\install\\dbgcore.dll");
    if (!FileExists(outPath)) { AddLog(C_ERR, L"The download has no plugin loader in it."); return FALSE; }
    return TRUE;
}

static DWORD WINAPI InstallThread(LPVOID uninstall) {
    wchar_t dir[MAX_PATH], plugins[MAX_PATH], p[MAX_PATH], q[MAX_PATH], dl[MAX_PATH];
    DWORD n; const void* res;
    g_result = 4;
    GetGameDir(dir);
    if (!IsGameDir(dir)) { g_result = 1; AddLog(C_ERR, L"echovr.exe isn't in that folder. Pick Echo's bin\\win10 folder."); goto done; }
    if (EchoRunning()) { g_result = 2; AddLog(C_ERR, L"Close Echo VR first: it keeps the plugin files locked."); goto done; }
    Join(plugins, MAX_PATH, dir, L"plugins");
    Join(p, MAX_PATH, plugins, L"EchoVRMusic.dll");

    if (uninstall) {
        if (FileExists(p) && !DeleteFileW(p)) AddLog(C_ERR, L"Couldn't remove EchoVRMusic.dll (error %lu)", GetLastError());
        else { g_result = 0; AddLog(C_OK, L"Removed EchoVRMusic. The plugin loader stays, other plugins use it."); }
        goto done;
    }

    AddLog(C_TEXT, L"Installing into %s", dir);
    if (!DirExists(plugins)) {
        if (!CreateDirectoryW(plugins, NULL)) { AddLog(C_ERR, L"Couldn't create the plugins folder (error %lu)", GetLastError()); goto done; }
        AddLog(C_OK, L"Created the plugins folder");
    }

    /* plugin loader */
    Join(q, MAX_PATH, dir, L"dbgcore.dll");
    {
        BYTE* b = ReadAll(q, &n);
        int state = !b ? LD_MISSING : MentionsPlugins(b, n) ? LD_LOADER : LD_OTHER;
        free(b);
        if (state == LD_LOADER) {
            AddLog(C_OK, L"Plugin loader already installed (dbgcore.dll), kept");
        } else {
            if (!DownloadLoader(dl)) { g_result = 3; goto done; }
            if (state == LD_OTHER) {
                wchar_t orig[MAX_PATH];
                Join(orig, MAX_PATH, plugins, L"dbgcoreoriginal.dll");
                if (!MoveFileExW(q, orig, MOVEFILE_REPLACE_EXISTING)) { AddLog(C_ERR, L"Couldn't move the old dbgcore.dll (error %lu)", GetLastError()); goto done; }
                AddLog(C_OK, L"Moved the old dbgcore.dll to plugins\\dbgcoreoriginal.dll (it still loads from there)");
            }
            if (!CopyFileW(dl, q, FALSE)) { AddLog(C_ERR, L"Couldn't install the plugin loader (error %lu)", GetLastError()); goto done; }
            AddLog(C_OK, L"Installed the plugin loader (dbgcore.dll)");
        }
    }

    /* plugin */
    res = PluginResource(&n);
    if (!res || !WriteAll(p, res, n)) { AddLog(C_ERR, L"Couldn't write plugins\\EchoVRMusic.dll (error %lu)", GetLastError()); goto done; }
    AddLog(C_OK, L"Installed plugins\\EchoVRMusic.dll");
    Join(q, MAX_PATH, plugins, L"EchoVRMusic.txt");
    if (FileExists(q) && DeleteFileW(q)) AddLog(C_MUTED, L"Removed the old EchoVRMusic.txt (settings are in this app now)");
    RegPut(L"InstallDir", dir);
    g_result = 0;
    AddLog(C_OK, L"Done. Start Echo VR and play some music on your PC.");
done:
    InterlockedExchange(&g_busy, 0);
    RefreshStatus();
    return 0;
}

static void StartInstall(BOOL uninstall) {
    if (InterlockedCompareExchange(&g_busy, 1, 0)) return;
    CloseHandle(CreateThread(NULL, 0, InstallThread, (LPVOID)(INT_PTR)uninstall, 0, NULL));
}

static void Browse(void) {
    BROWSEINFOW bi = {0};
    wchar_t name[MAX_PATH];
    PIDLIST_ABSOLUTE id;
    bi.hwndOwner = g_wnd;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"Pick Echo VR's bin\\win10 folder (the one with echovr.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    id = SHBrowseForFolderW(&bi);
    if (id) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(id, path)) SetWindowTextW(g_path, path);
        CoTaskMemFree(id);
        RefreshStatus();
    }
}

/* ---- drawing ---------------------------------------------------------------------------------- */
static HDC g_dc;
static int g_clipTop, g_clipBottom;   /* hit areas outside this band are dropped (scrolled away) */

static void Fill(int l, int t, int r, int b, COLORREF c) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(g_dc, &rc, br);
    DeleteObject(br);
}

static void Round(int l, int t, int r, int b, int rad, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(g_dc, br), op = SelectObject(g_dc, pen);
    RoundRect(g_dc, l, t, r, b, rad, rad);
    SelectObject(g_dc, ob); SelectObject(g_dc, op);
    DeleteObject(br); DeleteObject(pen);
}

static void Gradient(int l, int t, int r, int b, COLORREF a, COLORREF c) {
    TRIVERTEX v[2] = {
        {l, t, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0xff00},
        {r, b, (COLOR16)(GetRValue(c) << 8), (COLOR16)(GetGValue(c) << 8), (COLOR16)(GetBValue(c) << 8), 0xff00}};
    GRADIENT_RECT gr = {0, 1};
    GradientFill(g_dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
}

/* A gradient pill: the gradient clipped to a rounded rectangle. */
static void GradientPill(int l, int t, int r, int b, COLORREF a, COLORREF c) {
    HRGN rgn = CreateRoundRectRgn(l, t, r + 1, b + 1, b - t, b - t);
    SelectClipRgn(g_dc, rgn);
    Gradient(l, t, r, b, a, c);
    SelectClipRgn(g_dc, NULL);
    DeleteObject(rgn);
}

static void Text(const wchar_t* s, int l, int t, int r, int b, HFONT f, COLORREF c, UINT fmt) {
    RECT rc = {l, t, r, b};
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    DrawTextW(g_dc, s, -1, &rc, fmt | DT_NOPREFIX);
}

static int TextWidth(const wchar_t* s, HFONT f) {
    SIZE sz;
    SelectObject(g_dc, f);
    GetTextExtentPoint32W(g_dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

static int TextHeight(const wchar_t* s, int width, HFONT f) {
    RECT rc = {0, 0, width, 0};
    SelectObject(g_dc, f);
    DrawTextW(g_dc, s, -1, &rc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    return rc.bottom;
}

static void AddHot(int l, int t, int r, int b, int id) {
    if (b < g_clipTop || t > g_clipBottom) return;
    if (g_nhot < (int)(sizeof g_hot / sizeof g_hot[0])) { RECT rc = {l, t, r, b}; g_hot[g_nhot].rc = rc; g_hot[g_nhot].id = id; g_nhot++; }
}

static void Dot(int x, int y, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(g_dc, br), op = SelectObject(g_dc, pen);
    Ellipse(g_dc, x - r, y - r, x + r + 1, y + r + 1);
    SelectObject(g_dc, ob); SelectObject(g_dc, op);
    DeleteObject(br); DeleteObject(pen);
}

/* A text link: muted, lights up on hover. */
static void Link(const wchar_t* s, int x, int y, int id, BOOL rightAlign) {
    int w = TextWidth(s, g_fBody);
    if (rightAlign) x -= w;
    Text(s, x, y, x + w, y + S(22), g_fBody, g_hover == id ? C_TEXT : C_MUTED, DT_SINGLELINE);
    AddHot(x - S(4), y - S(4), x + w + S(4), y + S(24), id);
}

static void Toggle(int x, int y, BOOL on) {
    int w = S(40), h = S(22);
    if (on) GradientPill(x, y, x + w, y + h, C_ACCENT, C_ACCENT2);
    else Round(x, y, x + w, y + h, h, C_LINE);
    Dot(on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - S(4), on ? C_TEXT : C_MUTED);
}

static void Slider(int l, int y, int r, double t, BOOL active) {
    int x = l + (int)((r - l) * t);
    Round(l, y - S(2), r, y + S(2), S(4), C_LINE);
    if (x > l + S(2)) GradientPill(l, y - S(2), x, y + S(2), C_ACCENT, C_ACCENT2);
    Dot(x, y, active ? S(8) : S(7), C_TEXT);
}

/* ---- pages ------------------------------------------------------------------------------------ */
static void PageInstall(int l, int t, int r, int b) {
    int cx = (l + r) / 2, y = t + S(20), bw = S(240);
    const wchar_t* head; const wchar_t* sub; COLORREF dot;
    wchar_t dir[MAX_PATH];
    (void)b;

    /* the app icon */
    {
        HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(72), S(72), 0);
        DrawIconEx(g_dc, cx - S(36), y, ic, S(72), S(72), 0, NULL, DI_NORMAL);
        DestroyIcon(ic);
    }
    y += S(92);

    if (!g_st.game)           { head = L"Find Echo VR first";    sub = L"Pick the folder with echovr.exe below.";            dot = C_WARN; }
    else if (g_busy)          { head = L"Installing...";         sub = L"This takes a few seconds.";                         dot = C_ACCENT2; }
    else if (g_st.running)    { head = L"Close Echo VR";         sub = L"It keeps the plugin locked while it runs.";         dot = C_WARN; }
    else if (g_st.pluginCurrent) { head = L"Installed";          sub = L"Start Echo VR and play music on your PC.";          dot = C_OK; }
    else if (g_st.plugin)     { head = L"Update available";      sub = L"This version is newer than the one installed.";     dot = C_WARN; }
    else                      { head = L"Ready to install";      sub = L"Adds EchoVRMusic and the plugin loader to Echo.";   dot = C_ACCENT2; }
    {
        int w = TextWidth(head, g_fTitle);
        Dot(cx - w / 2 - S(18), y + S(19), S(5), dot);
        Text(head, l, y, r, y + S(40), g_fTitle, C_TEXT, DT_SINGLELINE | DT_CENTER);
    }
    Text(sub, l, y + S(42), r, y + S(66), g_fBody, C_MUTED, DT_SINGLELINE | DT_CENTER);
    y += S(88);

    /* folder: one line + a link */
    GetGameDir(dir);
    Text(dir[0] ? dir : L"No folder picked", l, y, r, y + S(22), g_fSmall, C_MUTED, DT_SINGLELINE | DT_CENTER | DT_PATH_ELLIPSIS);
    y += S(26);
    {
        int w = TextWidth(L"Change folder", g_fBody);
        Link(L"Change folder", cx - w / 2, y, H_BROWSE, FALSE);
    }
    y += S(44);

    /* the button */
    {
        BOOL en = !g_busy && g_st.game && !g_st.running;
        int id = H_INSTALL;
        const wchar_t* label = g_busy ? L"Installing..." : g_st.pluginCurrent ? L"Reinstall" : g_st.plugin ? L"Update" : L"Install";
        if (en) {
            BOOL hov = g_hover == id;
            GradientPill(cx - bw / 2, y, cx + bw / 2, y + S(52), hov ? Mix(C_ACCENT, C_TEXT, 0.15) : C_ACCENT, hov ? Mix(C_ACCENT2, C_TEXT, 0.15) : C_ACCENT2);
            AddHot(cx - bw / 2, y, cx + bw / 2, y + S(52), id);
        } else {
            Round(cx - bw / 2, y, cx + bw / 2, y + S(52), S(52), C_SURFACE);
        }
        Text(label, cx - bw / 2, y, cx + bw / 2, y + S(52), g_fH, en ? C_TEXT : C_MUTED, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    y += S(64);
    if (g_st.plugin && !g_busy && !g_st.running) {
        int w = TextWidth(L"Uninstall", g_fBody);
        Link(L"Uninstall", cx - w / 2, y, H_UNINSTALL, FALSE);
    }
    y += S(36);

    /* the last thing that happened */
    EnterCriticalSection(&g_logLock);
    if (g_nlog) {
        LogLine* ll = &g_log[g_nlog - 1];
        Text(ll->text, l + S(20), y, r - S(20), y + S(44), g_fSmall, ll->col == C_ERR ? C_ERR : C_MUTED, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
    LeaveCriticalSection(&g_logLock);
}

static double SliderT(const Setting* s) { return (s->val - s->min) / (s->max - s->min); }

static int SettingRow(int i, int l, int y, int r) {
    Setting* s = &g_set[i];
    int id = H_SETTING0 + i, h;
    BOOL active = g_hover == id || g_drag == i;
    wchar_t v[64];
    if (s->kind == K_TOGGLE) {
        Text(s->label, l, y, r - S(52), y + S(24), g_fH, C_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (s->hint) Text(s->hint, l, y + S(24), r - S(52), y + S(44), g_fSmall, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        h = s->hint ? S(48) : S(30);
        Toggle(r - S(40), y + (s->hint ? S(12) : S(2)), s->val >= 0.5);
        AddHot(l, y - S(8), r, y + h, id);
    } else {
        _snwprintf(v, 64, s->fmt, wcsstr(s->fmt, L"%%") ? s->val * 100 : s->val);
        if (!wcscmp(s->reg, L"Nearest") && s->val < 0.5) wcscpy(v, L"All");
        Text(v, r - S(120), y, r, y + S(24), g_fBody, active ? C_TEXT : C_MUTED, DT_SINGLELINE | DT_RIGHT);
        Text(s->label, l, y, r - TextWidth(v, g_fBody) - S(12), y + S(24), g_fH, C_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (s->hint) Text(s->hint, l, y + S(24), r, y + S(44), g_fSmall, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        h = s->hint ? S(48) : S(28);
        Slider(l + S(8), y + h + S(8), r - S(8), SliderT(s), active);
        AddHot(l, y + h - S(6), r, y + h + S(22), id);
        h += S(24);
    }
    return y + h + S(22);
}

static void PageSettings(int l, int t, int r, int b) {
    int gap = S(56), cw = (r - l - 2 * gap) / 3, top = t + S(28), col, i;
    static const wchar_t* heads[3] = {L"SPEAKERS", L"OPTIONS", L"ADVANCED"};
    BOOL app = _wcsnicmp(g_source, L"process:", 8) == 0;
    (void)b;
    for (col = 0; col < 3; col++) {
        int x0 = l + col * (cw + gap), x1 = x0 + cw, y = top;
        Text(heads[col], x0, y, x1, y + S(18), g_fSmall, C_MUTED, DT_SINGLELINE);
        y += S(36);
        if (col == 1) {
            /* where the music comes from: label, then a two-way switch across the column */
            int sw = cw / 2;
            Text(L"Music comes from", x0, y, x1, y + S(24), g_fH, C_TEXT, DT_SINGLELINE);
            y += S(32);
            Round(x0, y, x1, y + S(32), S(32), C_SURFACE);
            if (app) GradientPill(x0 + sw, y, x1, y + S(32), C_ACCENT, C_ACCENT2);
            else GradientPill(x0, y, x0 + sw, y + S(32), C_ACCENT, C_ACCENT2);
            Text(L"All of Windows", x0, y, x0 + sw, y + S(32), g_fSmall, app ? C_MUTED : C_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            Text(L"One app", x0 + sw, y, x1, y + S(32), g_fSmall, app ? C_TEXT : C_MUTED, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            AddHot(x0, y, x0 + sw, y + S(32), H_SRC_SYSTEM);
            AddHot(x0 + sw, y, x1, y + S(32), H_SRC_APP);
            y += S(42);
            if (app) {
                Round(x0, y, x1, y + S(32), S(10), C_SURFACE);
                MoveWindow(g_app, x0 + S(12), y + S(7), cw - S(24), S(20), FALSE);
                ShowWindow(g_app, SW_SHOWNA);
                y += S(42);
            } else {
                ShowWindow(g_app, SW_HIDE);
            }
            y += S(14);
        }
        for (i = 0; i < NSET; i++) if (g_set[i].column == col) y = SettingRow(i, x0, y, x1);
        if (col == 2) {
            y += S(10);
            Link(L"Reset to defaults", x0, y, H_RESET, FALSE);
            if (!g_busy && !g_st.running) Link(L"Uninstall", x1, y, H_UNINSTALL, TRUE);
        }
    }
    g_scrollMax = 0;
}

/* The help panel: drawn over the page, the page dimmed behind it. */
static void HelpPanel(int w, int h) {
    static const wchar_t* steps[][2] = {
        {L"Install", L"Close Echo VR and press Install. The plugins folder and plugin loader are set up for you."},
        {L"Play music on your PC", L"Anything Windows plays comes out of Echo's speakers. To hear it only in game, send your music app to an output you don't listen to: Windows Settings > Sound > App volume and device preferences."},
        {L"Play Echo", L"The arena, Dyson, Surge, Combustion and Fission have speakers built in, and the lobby uses its own. On other maps the music plays beside you. Ctrl+Alt+M turns it on and off."},
        {L"Can't tell speakers apart?", L"Lower \"Silent beyond\" so far speakers drop out, or play only the nearest 1 to 3. Settings change in game straight away."},
    };
    int i, pw = min(w - S(80), S(860)), l = (w - pw) / 2, r = l + pw, pad = S(28), y, t, ph = 0;
    /* dim the page */
    {
        HDC dim = CreateCompatibleDC(g_dc);
        HBITMAP px = CreateCompatibleBitmap(g_dc, 1, 1);
        HGDIOBJ old = SelectObject(dim, px);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 200, 0};
        SetPixel(dim, 0, 0, RGB(6, 7, 10));
        AlphaBlend(g_dc, 0, 0, w, h, dim, 0, 0, 1, 1, bf);
        SelectObject(dim, old); DeleteObject(px); DeleteDC(dim);
    }
    for (i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++)
        ph += S(28) + TextHeight(steps[i][1], pw - 2 * pad - S(28), g_fBody) + S(20);
    ph += S(84);
    t = max(S(20), (h - ph) / 2);
    Round(l, t, r, t + ph, S(24), C_SURFACE);
    AddHot(0, 0, w, h, H_CLOSE);              /* clicking outside closes it */
    AddHot(l, t, r, t + ph, H_NONE);          /* ...inside doesn't */
    Text(L"How it works", l + pad, t + S(24), r - pad, t + S(56), g_fBrand, C_TEXT, DT_SINGLELINE);
    Text(L"\x2715", r - S(52), t + S(18), r - S(20), t + S(50), g_fH, g_hover == H_CLOSE + 100 ? C_TEXT : C_MUTED, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    AddHot(r - S(56), t + S(14), r - S(16), t + S(54), H_CLOSE + 100);
    y = t + S(72);
    for (i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
        wchar_t n[4];
        int th = TextHeight(steps[i][1], pw - 2 * pad - S(28), g_fBody);
        _snwprintf(n, 4, L"%d", i + 1);
        Text(n, l + pad, y, l + pad + S(28), y + S(24), g_fH, C_ACCENT2, DT_SINGLELINE);
        Text(steps[i][0], l + pad + S(28), y, r - pad, y + S(24), g_fH, C_TEXT, DT_SINGLELINE);
        Text(steps[i][1], l + pad + S(28), y + S(26), r - pad, y + S(26) + th, g_fBody, C_MUTED, DT_WORDBREAK);
        y += S(28) + th + S(20);
    }
}

static BOOL ShowSettings(void) { return g_st.game && g_st.plugin && !g_busy; }

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    RECT cr;
    HDC win = BeginPaint(hwnd, &ps), mem;
    HBITMAP bmp; HGDIOBJ old;
    int w, h, pad = S(40), headY = S(28), contentT = S(84);
    GetClientRect(hwnd, &cr);
    w = cr.right; h = cr.bottom;
    mem = CreateCompatibleDC(win);
    bmp = CreateCompatibleBitmap(win, w, h);
    old = SelectObject(mem, bmp);
    g_dc = mem;
    SetBkMode(mem, TRANSPARENT);
    g_nhot = 0;
    Fill(0, 0, w, h, C_BG);

    /* the page (settings scroll under the header) */
    g_clipTop = contentT; g_clipBottom = h;
    if (ShowSettings()) {
        HRGN clip = CreateRectRgn(0, contentT, w, h);
        SelectClipRgn(mem, clip);
        PageSettings(pad, contentT, w - pad, h);
        SelectClipRgn(mem, NULL);
        DeleteObject(clip);
    } else {
        ShowWindow(g_app, SW_HIDE);
        PageInstall(pad, contentT, w - pad, h - S(24));
    }

    /* header: name, and the help button */
    g_clipTop = 0; g_clipBottom = contentT;
    Fill(0, 0, w, contentT, C_BG);
    Text(L"EchoVRMusic", pad, headY, w / 2, headY + S(30), g_fBrand, C_TEXT, DT_SINGLELINE);
    {
        int bx = w - pad - S(32), hov = g_hover == H_HELP || g_help;
        Round(bx, headY - S(1), bx + S(32), headY + S(31), S(32), hov ? C_LINE : C_SURFACE);
        Text(L"?", bx, headY - S(1), bx + S(32), headY + S(31), g_fH, hov ? C_TEXT : C_MUTED, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        AddHot(bx, headY - S(1), bx + S(32), headY + S(31), H_HELP);
        /* installed, but not the version this app carries: offer the update next to the ? */
        if (ShowSettings() && !g_st.pluginCurrent) {
            const wchar_t* label = g_st.running ? L"Close Echo to update" : L"Update";
            int pw = TextWidth(label, g_fSmall) + S(28), px = bx - S(12) - pw;
            if (g_st.running) {
                Round(px, headY - S(1), px + pw, headY + S(31), S(32), C_SURFACE);
                Text(label, px, headY - S(1), px + pw, headY + S(31), g_fSmall, C_MUTED, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            } else {
                BOOL hov2 = g_hover == H_INSTALL;
                GradientPill(px, headY - S(1), px + pw, headY + S(31), hov2 ? Mix(C_ACCENT, C_TEXT, 0.15) : C_ACCENT,
                             hov2 ? Mix(C_ACCENT2, C_TEXT, 0.15) : C_ACCENT2);
                Text(label, px, headY - S(1), px + pw, headY + S(31), g_fSmall, C_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                AddHot(px, headY - S(1), px + pw, headY + S(31), H_INSTALL);
            }
        }
    }
    if (ShowSettings()) Fill(pad, contentT - S(1), w - pad, contentT, C_LINE);

    if (g_help) {
        g_nhot = 0;            /* only the panel takes clicks while it's open */
        g_clipTop = 0; g_clipBottom = h;
        ShowWindow(g_app, SW_HIDE);
        HelpPanel(w, h);
    }

    BitBlt(win, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
    if (IsWindowVisible(g_app)) InvalidateRect(g_app, NULL, TRUE);
}

/* ---- input ------------------------------------------------------------------------------------ */
static int HitTest(int x, int y) {
    int i;
    POINT p = {x, y};
    for (i = g_nhot - 1; i >= 0; i--) if (PtInRect(&g_hot[i].rc, p)) return g_hot[i].id;
    return H_NONE;
}

static void DragSlider(int i, int x) {
    int k;
    for (k = 0; k < g_nhot; k++) {
        if (g_hot[k].id == H_SETTING0 + i) {
            Setting* s = &g_set[i];
            double l = g_hot[k].rc.left + S(8), r = g_hot[k].rc.right - S(8);
            double t = (x - l) / (r - l), v;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            v = s->min + t * (s->max - s->min);
            v = s->min + floor((v - s->min) / s->step + 0.5) * s->step;
            if (fabs(v - s->val) > 1e-9) { s->val = v; SaveSetting(i); InvalidateRect(g_wnd, NULL, FALSE); }
            return;
        }
    }
}

static void SaveSource(void) {
    if (_wcsnicmp(g_source, L"process:", 8) == 0) {
        wchar_t exe[200];
        GetWindowTextW(g_app, exe, 200);
        _snwprintf(g_source, 260, L"process:%s", exe[0] ? exe : L"spotify.exe");
    }
    RegPut(L"Source", g_source);
}

static void Click(int id, int x) {
    if (id == H_HELP) g_help = TRUE;
    else if (id == H_CLOSE || id == H_CLOSE + 100) g_help = FALSE;
    else if (id == H_BROWSE) Browse();
    else if (id == H_INSTALL) StartInstall(FALSE);
    else if (id == H_UNINSTALL) StartInstall(TRUE);
    else if (id == H_RESET) ResetSettings();
    else if (id == H_SRC_SYSTEM) { wcscpy(g_source, L"system"); SaveSource(); }
    else if (id == H_SRC_APP) { wcscpy(g_source, L"process:"); SaveSource(); SetFocus(g_app); }
    else if (id >= H_SETTING0 && id < H_SETTING0 + NSET) {
        int i = id - H_SETTING0;
        if (g_set[i].kind == K_TOGGLE) { g_set[i].val = g_set[i].val >= 0.5 ? 0 : 1; SaveSetting(i); }
        else { g_drag = i; SetCapture(g_wnd); DragSlider(i, x); }
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static HFONT MakeFont(const wchar_t* face, int px, int weight) {
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        wchar_t d[MAX_PATH] = L"", exe[200] = L"spotify.exe";
        if (!g_host) DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        g_fTitle = MakeFont(L"Segoe UI Semibold", 26, FW_SEMIBOLD);
        g_fBrand = MakeFont(L"Segoe UI Semibold", 18, FW_SEMIBOLD);
        g_fTab = MakeFont(L"Segoe UI", 15, FW_NORMAL);
        g_fH = MakeFont(L"Segoe UI", 15, FW_NORMAL);
        g_fBody = MakeFont(L"Segoe UI", 14, FW_NORMAL);
        g_fSmall = MakeFont(L"Segoe UI", 12, FW_NORMAL);
        g_brEdit = CreateSolidBrush(C_SURFACE);
        /* the folder lives in a hidden edit control; the app name is typed into a visible one */
        g_path = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)1, g_inst, NULL);
        g_app = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)2, g_inst, NULL);
        SendMessageW(g_app, WM_SETFONT, (WPARAM)g_fBody, 0);
        LoadSettings();
        if (_wcsnicmp(g_source, L"process:", 8) == 0 && g_source[8]) wcscpy(exe, g_source + 8);
        SetWindowTextW(g_app, exe);
        if (g_argDir[0]) SetWindowTextW(g_path, g_argDir);
        else if (DetectGameDir(d)) SetWindowTextW(g_path, d);
        if (g_host) SetTimer(hwnd, 1, 1000, NULL);
        RefreshStatus();
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_path) RefreshStatus();
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_app && _wcsnicmp(g_source, L"process:", 8) == 0) SaveSource();
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_SURFACE);
        return (LRESULT)g_brEdit;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_help) { g_help = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        if (!g_help && ShowSettings()) {
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA;
            if (g_scroll > g_scrollMax) g_scroll = g_scrollMax;
            if (g_scroll < 0) g_scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), h;
        if (g_drag >= 0) { DragSlider(g_drag, x); return 0; }
        h = HitTest(x, y);
        if (h != g_hover) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = H_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hover != H_NONE ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), id = HitTest(x, GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        if (id != H_NONE) Click(id, x);
        else if (g_help) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) RefreshStatus();
        break;
    case WM_SIZE:
        g_scroll = 0;
        break;
    case WM_GETMINMAXINFO: {
        if (g_host) break;
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(940);
        mm->ptMinTrackSize.y = S(580);
        return 0;
    }
    case WM_COPYDATA: {
        COPYDATASTRUCT* cd = (COPYDATASTRUCT*)lp;
        if (cd && cd->dwData == THEME_MSG && cd->lpData && cd->cbData >= sizeof(wchar_t)) {
            wchar_t buf[512];
            size_t n = min(cd->cbData / sizeof(wchar_t), 511);
            memcpy(buf, cd->lpData, n * sizeof(wchar_t));
            buf[n] = 0;
            ApplyTheme(buf);
            DeleteObject(g_brEdit);
            g_brEdit = CreateSolidBrush(C_SURFACE);
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_app) InvalidateRect(g_app, NULL, TRUE);
            return TRUE;
        }
        break;
    }
    case WM_TIMER:
        if (g_host && !IsWindow(g_host)) DestroyWindow(hwnd);   /* the host closed */
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    WNDCLASSEXW wc = {sizeof wc};
    MSG m;
    HDC screen;
    int argc = 0, i;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL install = FALSE;
    (void)prev; (void)cmd;
    g_inst = inst;
    for (i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--dir") && i + 1 < argc) wcscpy_s(g_argDir, MAX_PATH, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--install")) install = TRUE;
        else if (!_wcsicmp(argv[i], L"--silent")) g_silent = TRUE;
        else if (!_wcsicmp(argv[i], L"--theme") && i + 1 < argc) wcscpy_s(g_argTheme, 512, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--embed") && i + 1 < argc) g_host = (HWND)(INT_PTR)_wcstoi64(argv[++i], NULL, 0);
    }
    if (argv) LocalFree(argv);
    if (g_argTheme[0]) ApplyTheme(g_argTheme);
    while (g_argDir[0] && (g_argDir[wcslen(g_argDir) - 1] == L'\\' || g_argDir[wcslen(g_argDir) - 1] == L'"'))
        g_argDir[wcslen(g_argDir) - 1] = 0;
    SetProcessDPIAware();
    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, screen);
    InitializeCriticalSection(&g_logLock);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (install && g_silent) {
        if (!g_argDir[0] && !DetectGameDir(g_argDir)) { g_silent = TRUE; AddLog(C_ERR, L"Couldn't find Echo VR."); return 1; }
        g_busy = 1;
        InstallThread(NULL);
        return g_result;
    }
    wc.style = CS_HREDRAW | CS_VREDRAW;   /* repaint all of it when a host resizes us */
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"EchoVRMusicSetup";
    RegisterClassExW(&wc);
    if (g_host && IsWindow(g_host)) {
        RECT hr;
        GetClientRect(g_host, &hr);
        g_wnd = CreateWindowExW(0, wc.lpszClassName, L"EchoVRMusic", WS_CHILD | WS_CLIPCHILDREN,
                                0, 0, hr.right, hr.bottom, g_host, NULL, inst, NULL);
    } else {
        g_host = NULL;
        g_wnd = CreateWindowExW(0, wc.lpszClassName, L"EchoVRMusic", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1080), S(600), NULL, NULL, inst, NULL);
    }
    ShowWindow(g_wnd, show);
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
