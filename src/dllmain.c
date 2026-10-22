/*
 * Budokai3Access - screen reader support for the Dragon Ball Z Budokai 3 HD
 * Collection PC port (dbz3.exe, ReXGlue recompilation).
 *
 * This is Budokai3Access\Budokai3Access.dll, started by the loader (plugin.c,
 * installed as rexgpu-xenos.dll), which calls core_start and, when
 * reloading during development, core_stop.
 */
#include "access.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

wchar_t g_game_dir[MAX_PATH];
wchar_t g_mod_dir[MAX_PATH];
static FILE *g_logf;
static CRITICAL_SECTION g_loglock;
static long g_log_bytes;

char *to_utf8(const wchar_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

wchar_t *from_utf8(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? (size_t)n : 1));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

void log_line(const char *fmt, ...) {
    if (!g_logf) return;
    EnterCriticalSection(&g_loglock);
    if (g_log_bytes < 50L * 1024 * 1024) {
        SYSTEMTIME st; GetLocalTime(&st);
        g_log_bytes += fprintf(g_logf, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt);
        g_log_bytes += vfprintf(g_logf, fmt, ap);
        va_end(ap);
        fputc('\n', g_logf);
        fflush(g_logf);
    }
    LeaveCriticalSection(&g_loglock);
}

/* ---------- mod thread ---------- */
int game_has_focus(void) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Keys the mod listens to. F1, F3, F4, F7 and F10 belong to the port (quick
 * menu, FPS panel, settings, achievements, dev overlay), so the mod stays off
 * them. */
static const int k_keys[] = { VK_F2, VK_F5, VK_F6, VK_F8, VK_F9, VK_F11, VK_F12, VK_PRIOR, VK_NEXT };
#define NKEYS (int)(sizeof k_keys / sizeof k_keys[0])

static volatile int g_stop;
static HANDLE g_thread;
volatile LONG g_inflight;               /* game threads inside mod hooks */

static DWORD WINAPI mod_thread(LPVOID unused) {
    (void)unused;
    tools_init();
    if (!getenv("DBZ3A_NOPAD")) pad_init();
    selfcheck_log_versions();
    log_line("waiting for the game code");
    DWORD t0 = GetTickCount();
    while (!guest_ready()) {
        if (g_stop) return 0;
        if (GetTickCount() - t0 > 120000) { selfcheck_fail("it could not find the game's code"); return 0; }
        Sleep(50);
    }
    log_line("game code ready");
    if (!getenv("DBZ3A_NOGAME")) game_init();
    if (guest_hooks_bad()) { selfcheck_fail("the game's code is not where it expects"); return 0; }
    guest_hooks_apply();
    /* The game window appears after the code is ready, and the screen reader
     * cuts off anything said before it announces the window: wait until the
     * window has had focus for a second. */
    DWORD focus_since = 0;
    int announced = 0;
    int down[NKEYS] = { 0 };
    while (!g_stop) {
        Sleep(16);
        if (!announced) {
            if (!game_has_focus()) focus_since = 0;
            else if (!focus_since) focus_since = GetTickCount();
            else if (GetTickCount() - focus_since > 1000) {
                speak(L"Budokai 3 Access nightly loaded", 0);
                announced = 1;
            }
        }
        if (game_has_focus()) {
            int shift = GetAsyncKeyState(VK_SHIFT) < 0, ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
            for (int i = 0; i < NKEYS; i++) {
                int d = GetAsyncKeyState(k_keys[i]) < 0;
                if (d && !down[i]) game_key(k_keys[i], shift, ctrl);
                down[i] = d;
            }
        }
        selfcheck_poll();
        if (selfcheck_failed()) continue;      /* hooks are off: keep quiet, keep the game safe */
        game_poll();
        tools_poll();
    }
    return 0;
}

/* One running copy per game process: an old version.dll loader left in the
 * folder would otherwise start the mod a second time. */
static HANDLE g_once;

__declspec(dllexport) void core_start(void) {
    wchar_t name[64];
    swprintf(name, 64, L"Budokai3Access_running_%lu", GetCurrentProcessId());
    g_once = CreateMutexW(NULL, FALSE, name);
    if (g_once && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(g_once); g_once = NULL; return; }
    GetModuleFileNameW(NULL, g_game_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_game_dir, L'\\');
    if (slash) slash[1] = 0;
    swprintf(g_mod_dir, MAX_PATH, L"%lsBudokai3Access\\", g_game_dir);
    CreateDirectoryW(g_mod_dir, NULL);
    InitializeCriticalSection(&g_loglock);
    wchar_t logpath[MAX_PATH];
    swprintf(logpath, MAX_PATH, L"%lsBudokai3Access.log", g_mod_dir);
    g_logf = _wfopen(logpath, L"a");
    log_line("Budokai3Access " MOD_VERSION " starting");
    speech_init();
    buttons_load();
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, mod_thread, NULL, 0, NULL);
}

/* Development reload: unhook everything and wait until no game thread is
 * inside mod code, so the DLL can be unloaded. */
__declspec(dllexport) void core_stop(void) {
    if (!g_once) return;                   /* this copy never started */
    CloseHandle(g_once); g_once = NULL;
    g_stop = 1;
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    pad_shutdown();
    game_shutdown();
    tools_shutdown();
    for (int i = 0; i < 300 && g_inflight; i++) Sleep(10);
    Sleep(200);
    speech_shutdown();
    log_line("Budokai3Access stopped for reload");
    fclose(g_logf); g_logf = NULL;
    DeleteCriticalSection(&g_loglock);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
