/*
 * Self checks, so a port update cannot crash the game through the mod.
 *
 * The game's own (Xbox 360) code and data do not change between port
 * updates, but the port's plumbing can: where the runtime keeps its table
 * of recompiled functions and how it passes registers to them. The mod
 * checks both before and after hooking in:
 *  - startup: the function table must appear within a time limit, and every
 *    hook target must be real code (guest.c);
 *  - running: the texture hook, called every frame, must see real texture
 *    packs ("#AZT"). If it is called a lot but never sees one, the register
 *    layout has changed: the mod switches its hooks off and says so.
 * The port's version is written to the log for bug reports.
 */
#include "access.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "MinHook.h"

#define TESTED_PORT "1.5.5.1"

volatile LONG g_bind_calls, g_bind_sane;   /* counted by the texture hook (text.c) */
static int g_checked, g_failed;
static DWORD g_since;
static char g_port[32] = "unknown";

static void file_version(const wchar_t *name, char *out, int cap) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls%ls", g_game_dir, name);
    snprintf(out, cap, "unknown");
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    if (!n) return;
    void *buf = malloc(n);
    VS_FIXEDFILEINFO *fi; UINT len;
    if (buf && GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi)
        snprintf(out, cap, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    free(buf);
}

const char *selfcheck_port_version(void) { return g_port; }

void selfcheck_log_versions(void) {
    char rt[32], gpu[32];
    file_version(L"dbz3.exe", g_port, sizeof g_port);
    file_version(L"rexruntime.dll", rt, sizeof rt);
    file_version(L"rexgpu-xenos-original.dll", gpu, sizeof gpu);
    log_line("port: dbz3.exe %s, rexruntime %s, graphics plugin %s (tested with %s)", g_port, rt, gpu, TESTED_PORT);
}

void selfcheck_fail(const char *what) {
    if (g_failed) return;
    g_failed = 1;
    log_line("SELF CHECK FAILED: %s (port %s)", what, g_port);
    speakf(0, "Budokai 3 Access: %s. The game port may have been updated (version %s). The mod has switched itself off "
              "so the game keeps running. Please look for a Budokai 3 Access update.", what, g_port);
}

int selfcheck_failed(void) { return g_failed; }

/* Called from the mod loop once the hooks are on. */
void selfcheck_poll(void) {
    if (g_checked || g_failed) return;
    DWORD now = GetTickCount();
    if (!g_since) g_since = now;
    if (g_bind_sane > 0) { g_checked = 1; log_line("self check passed (%ld texture calls)", (long)g_bind_calls); return; }
    /* Plenty of calls but never a real texture pack: registers are not where the mod expects. */
    if (g_bind_calls > 2000 && now - g_since > 20000) {
        MH_DisableHook(MH_ALL_HOOKS);
        selfcheck_fail("the game's text hook sees no game data");
    }
}
