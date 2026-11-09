/*
 * "Play Budokai 3 Access.exe": checks the mod is installed, repairs it, starts the
 * game.
 *
 * Port 1.5.6 and later load code plugins themselves (plugins\*.dll, when
 * dbz3_plugins = true). There this program undoes the old wrap described
 * below if an earlier version left it, copies
 * Budokai3Access\loader\Budokai3Access.dll to plugins\ and sets
 * dbz3_plugins = true on every run (the port's launcher can switch it off).
 *
 * Older ports: the mod's loader replaces the port's graphics plugin
 * rexgpu-xenos.dll (the port's own file is kept as rexgpu-xenos-original.dll).
 * A port update, or the launcher's "Repair install", puts the port's file
 * back and the mod would stop loading. Every time this program runs it:
 *   1. looks at rexgpu-xenos.dll: if it is not ours (no "Budokai 3 Access"
 *      product name), it is a new port file, so it becomes the original;
 *   2. puts the current loader (Budokai3Access\loader\rexgpu-xenos.dll) in place
 *      if it is missing or different;
 *   3. makes sure dbz3_user.toml has "audio_maxqframes = 3" and keyboard
 *      controls, and once switches controllers to SDL (see below);
 *   4. starts dbz3.exe, unless run with --repair (used by the build scripts).
 * It only copies and renames files inside the game folder. What it did is
 * written to Budokai3Access\launch.log.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static void logw(const wchar_t *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(g_log, L"%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(g_log, fmt, ap); va_end(ap);
    fputwc(L'\n', g_log); fflush(g_log);
}

static void fail(const wchar_t *msg) {
    logw(L"error: %ls", msg);
    MessageBoxW(NULL, msg, L"Budokai 3 Access", MB_OK | MB_ICONWARNING);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* Is this our loader? Our files carry the product name "Budokai 3 Access"
 * ("DBZ 3 Access" before 1.0). */
static int is_ours(const wchar_t *path) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    if (!n) return 0;
    void *buf = HeapAlloc(GetProcessHeap(), 0, n);
    wchar_t *name = NULL; UINT len = 0; int ours = 0;
    if (buf && GetFileVersionInfoW(path, 0, n, buf) &&
        VerQueryValueW(buf, L"\\StringFileInfo\\040904B0\\ProductName", (void **)&name, &len) && name)
        ours = wcscmp(name, L"Budokai 3 Access") == 0 || wcscmp(name, L"DBZ 3 Access") == 0;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return ours;
}

static int same_file(const wchar_t *a, const wchar_t *b) {
    HANDLE fa = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE fb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int same = 0;
    if (fa != INVALID_HANDLE_VALUE && fb != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sa, sb;
        if (GetFileSizeEx(fa, &sa) && GetFileSizeEx(fb, &sb) && sa.QuadPart == sb.QuadPart && sa.QuadPart < (64 << 20)) {
            static char ba[1 << 16], bb[1 << 16];
            DWORD ra, rb;
            same = 1;
            while (same && ReadFile(fa, ba, sizeof ba, &ra, NULL) && ReadFile(fb, bb, sizeof bb, &rb, NULL) && ra) {
                if (ra != rb || memcmp(ba, bb, ra)) same = 0;
            }
        }
    }
    if (fa != INVALID_HANDLE_VALUE) CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE) CloseHandle(fb);
    return same;
}

/* dbz3.exe file version as one number (1.5.6.0 -> 0x0001000500060000). */
static unsigned long long exe_version(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsdbz3.exe", g_dir);
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    unsigned long long v = 0;
    if (!n) return 0;
    void *buf = HeapAlloc(GetProcessHeap(), 0, n);
    VS_FIXEDFILEINFO *fi; UINT len;
    if (buf && GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi)
        v = ((unsigned long long)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return v;
}
#define PORT_WITH_PLUGINS 0x0001000500060000ULL   /* 1.5.6 */

static void toml_set(const char *key, const char *value);

/* Port 1.5.6+: the port's own plugin loader. Returns 1 when the plugin is in place. */
static int repair_plugin(void) {
    wchar_t gpu[MAX_PATH], orig[MAX_PATH], ours[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH];
    swprintf(gpu, MAX_PATH, L"%lsrexgpu-xenos.dll", g_dir);
    swprintf(orig, MAX_PATH, L"%lsrexgpu-xenos-original.dll", g_dir);
    swprintf(ours, MAX_PATH, L"%lsBudokai3Access\\loader\\Budokai3Access.dll", g_dir);
    swprintf(dir, MAX_PATH, L"%lsplugins", g_dir);
    swprintf(dst, MAX_PATH, L"%lsplugins\\Budokai3Access.dll", g_dir);
    if (!exists(ours)) { fail(L"The Budokai 3 Access loader is missing (Budokai3Access\\loader\\Budokai3Access.dll). Please reinstall the mod."); return 0; }
    /* Undo the old graphics plugin wrap. */
    if (exists(gpu) && is_ours(gpu)) {
        if (!exists(orig)) {
            fail(L"The game's graphics plugin is missing (rexgpu-xenos.dll). Use Repair install in the game's launcher, then start Budokai 3 Access again.");
            return 0;
        }
        if (!MoveFileExW(orig, gpu, MOVEFILE_REPLACE_EXISTING)) {
            fail(L"Could not set up Budokai 3 Access: the game's graphics plugin is in use. Close the game and try again.");
            return 0;
        }
        logw(L"put the port's graphics plugin back (no wrapper needed from port 1.5.6)");
    } else if (exists(orig)) {
        DeleteFileW(orig);                        /* left over from before a port update */
        logw(L"removed the old rexgpu-xenos-original.dll");
    }
    CreateDirectoryW(dir, NULL);
    if (!exists(dst) || !same_file(dst, ours)) {
        if (CopyFileW(ours, dst, FALSE)) logw(L"installed plugins\\Budokai3Access.dll");
        else if (exists(dst)) logw(L"plugin in use; keeping the installed one");
        else { fail(L"Could not install the Budokai 3 Access plugin in the game's plugins folder."); return 0; }
    }
    toml_set("dbz3_plugins", "true");
    return 1;
}

/* Older ports: the graphics plugin wrapper. Returns 1 when the mod's loader is in place. */
static int repair(void) {
    wchar_t plugin[MAX_PATH], orig[MAX_PATH], ours[MAX_PATH];
    swprintf(plugin, MAX_PATH, L"%lsrexgpu-xenos.dll", g_dir);
    swprintf(orig, MAX_PATH, L"%lsrexgpu-xenos-original.dll", g_dir);
    swprintf(ours, MAX_PATH, L"%lsBudokai3Access\\loader\\rexgpu-xenos.dll", g_dir);
    if (!exists(ours)) { fail(L"The Budokai 3 Access loader is missing (Budokai3Access\\loader\\rexgpu-xenos.dll). Please reinstall the mod."); return 0; }
    if (exists(plugin) && !is_ours(plugin)) {
        /* The port's own graphics plugin (new install, port update or Repair install). */
        if (!MoveFileExW(plugin, orig, MOVEFILE_REPLACE_EXISTING)) {
            fail(L"Could not set up Budokai 3 Access: the game's graphics plugin is in use. Close the game and try again.");
            return 0;
        }
        logw(L"kept the port's graphics plugin as rexgpu-xenos-original.dll");
    }
    if (!exists(orig)) {
        fail(L"The game's graphics plugin is missing (rexgpu-xenos-original.dll). Use Repair install in the game's launcher, then start Budokai 3 Access again.");
        return 0;
    }
    if (!exists(plugin) || !same_file(plugin, ours)) {
        if (!CopyFileW(ours, plugin, FALSE)) {
            if (exists(plugin)) { logw(L"loader in use; keeping the installed one"); return 1; }
            fail(L"Could not install the Budokai 3 Access loader in the game folder.");
            return 0;
        }
        logw(L"installed the Budokai 3 Access loader");
    }
    return 1;
}

/* Adds "key = value" to dbz3_user.toml unless the key is already there. The
 * port rewrites the file when its settings change, so a line can go missing
 * and is added again; a value the player set by hand is left alone. */
static void toml_default(const char *key, const char *value, const wchar_t *why) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsdbz3_user.toml", g_dir);
    FILE *f = _wfopen(path, L"rb");
    if (f) {
        char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        if (strstr(buf, key)) return;
        f = _wfopen(path, L"ab");
        if (!f) return;
        if (n && buf[n - 1] != '\n') fputs("\n", f);
    } else {
        f = _wfopen(path, L"wb");
        if (!f) return;
    }
    fprintf(f, "%s = %s\n", key, value);
    fclose(f);
    logw(L"set %hs = %hs in dbz3_user.toml (%ls)", key, value, why);
}

/* Sound delay: the port queues audio_maxqframes sound frames of 5.3 ms
 * (default 8). Measured key press to menu click: 130 ms with 8, 105 ms with
 * 3, no dropouts in menus or fights.
 * Keyboard: the port's keyboard controls (mnk_mode) are off by default; many
 * blind players have no controller. A controller still works with it on. */
static void port_settings(void) {
    toml_default("audio_maxqframes", "3", L"less sound delay");
    toml_default("mnk_mode", "true", L"keyboard controls");
}

/* Sets "key = value" in dbz3_user.toml, replacing the key's line or adding
 * it at the end. */
static void toml_set(const char *key, const char *value) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsdbz3_user.toml", g_dir);
    char buf[16384]; size_t n = 0;
    FILE *f = _wfopen(path, L"rb");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    size_t klen = strlen(key);
    char *line = NULL;
    for (char *p = buf; *p; ) {
        if (!strncmp(p, key, klen) && (p[klen] == ' ' || p[klen] == '=')) { line = p; break; }
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    f = _wfopen(path, L"wb");
    if (!f) return;
    if (line) {
        char *end = strchr(line, '\n');
        if (end && end > line && end[-1] == '\r') end--;
        fwrite(buf, 1, line - buf, f);
        fprintf(f, "%s = %s", key, value);
        if (end) fputs(end, f);                   /* rest of the file, from the line break on */
        else fputs("\n", f);
    } else {
        fwrite(buf, 1, n, f);
        if (n && buf[n - 1] != '\n') fputs("\n", f);
        fprintf(f, "%s = %s\n", key, value);
    }
    fclose(f);
}

/* Controllers: the port reads pads through XInput by default, which cannot
 * see PlayStation controllers. Its SDL backend reads PS4/PS5 and Xbox pads.
 * dbz3_input_backend overrides input_backend (and the port saves
 * input_backend from it), so both are set. Done once, marked by
 * Budokai3Access\controller.txt, so a player who picks XInput again in the
 * port's launcher keeps it. */
static void controller_backend(void) {
    wchar_t mark[MAX_PATH];
    swprintf(mark, MAX_PATH, L"%lsBudokai3Access\\controller.txt", g_dir);
    if (exists(mark)) return;
    toml_set("input_backend", "\"sdl\"");
    toml_set("dbz3_input_backend", "\"sdl\"");
    FILE *f = _wfopen(mark, L"wb");
    if (f) {
        fputs("Budokai 3 Access switched the port to SDL controller support once (PS4/PS5 and Xbox controllers).\r\n"
              "Delete this file to have it switched again.\r\n", f);
        fclose(f);
    }
    logw(L"set the controller backend to SDL (PlayStation controllers)");
}

/* The port's launcher is drawn by the game (ImGui): screen readers see an
 * empty window and Tab moves around it silently. dbz3_skip_launcher boots
 * straight into the game. Done once, marked by Budokai3Access\launcher.txt,
 * so a player who deletes the line to get the launcher back keeps it. */
static void skip_launcher(void) {
    wchar_t mark[MAX_PATH];
    swprintf(mark, MAX_PATH, L"%lsBudokai3Access\\launcher.txt", g_dir);
    if (exists(mark)) return;
    toml_set("dbz3_skip_launcher", "true");
    FILE *f = _wfopen(mark, L"wb");
    if (f) {
        fputs("Budokai 3 Access set the game to skip the port's launcher once (dbz3_skip_launcher = true in dbz3_user.toml).\r\n"
              "To get the launcher back, change that line to false. Delete this file to have it skipped again.\r\n", f);
        fclose(f);
    }
    logw(L"set the port's launcher to be skipped");
}

/* The mod only knows the English game text. The port's game language
 * (dbz3_language, Xbox ids: 1 English, 2 Japanese, 3 German, 4 French,
 * 5 Spanish, 6 Italian) defaults to English; if the player picked another in
 * the port's launcher, offer to switch it back. */
static void english_check(void) {
    static const wchar_t *names[] = { L"", L"English", L"Japanese", L"German", L"French", L"Spanish", L"Italian" };
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsdbz3_user.toml", g_dir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    char *line = strstr(buf, "dbz3_language");
    if (!line || (line > buf && line[-1] != '\n')) return;
    char *eq = strchr(line, '=');
    char *end = strchr(line, '\n');
    if (!eq || (end && eq > end)) return;
    int lang = atoi(eq + 1);
    if (lang == 1) return;
    logw(L"game language is %d, not English", lang);
    wchar_t msg[400];
    swprintf(msg, 400, L"The game language is set to %ls. Budokai 3 Access can only read the game in English.\n\n"
             L"Switch the game to English?", (lang >= 2 && lang <= 6) ? names[lang] : L"another language");
    if (MessageBoxW(NULL, msg, L"Budokai 3 Access", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    toml_set("dbz3_language", "1");
    logw(L"set dbz3_language = 1 (English)");
}

/* Port 1.5.6's Gateway saga from Budokai 1 wants Zarbon (and Dodoria) imported
 * from the player's Budokai 1 disc: it looks for origen_fuente = "b1",
 * origen_clave = "13" / "14" in the fighter's personaje.toml. The Mod Kit import
 * also needs a Budokai 3 PS2 disc, and neither the Kit nor the port's launcher
 * can be used with a screen reader. Nova's Mods' Zarbon and Dodoria are the same
 * Budokai 1 ports (gritos = "b1:13" / "b1:14") without that tag, so it is added. */
static void tag_b1_ports(void) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%lsmods\\*", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%lsmods\\%ls\\personaje.toml", g_dir, fd.cFileName);
        FILE *f = _wfopen(path, L"rb");
        if (!f) continue;
        static char buf[65536];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        int whole = feof(f);
        fclose(f);
        buf[n] = 0;
        if (!whole || strstr(buf, "origen_fuente")) continue;
        const char *key = strstr(buf, "gritos = \"b1:13\"") ? "13" : strstr(buf, "gritos = \"b1:14\"") ? "14" : NULL;
        char *head = strstr(buf, "[personaje]");
        if (!key || !head) continue;
        char *eol = strchr(head, '\n');
        if (!eol) continue;
        eol++;
        const char *nl = (eol - 2 >= buf && eol[-2] == '\r') ? "\r\n" : "\n";
        f = _wfopen(path, L"wb");
        if (!f) continue;
        fwrite(buf, 1, (size_t)(eol - buf), f);
        fprintf(f, "origen_fuente = \"b1\"%sorigen_clave = \"%s\"%s", nl, key, nl);
        fwrite(eol, 1, n - (size_t)(eol - buf), f);
        fclose(f);
        logw(L"marked mods\\%ls as the Budokai 1 import the port's Namek saga asks for", fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)show;
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    wchar_t logp[MAX_PATH];
    swprintf(logp, MAX_PATH, L"%lsBudokai3Access\\launch.log", g_dir);
    g_log = _wfopen(logp, L"a, ccs=UTF-8");
    int repair_only = cmd && wcsstr(cmd, L"--repair") != NULL;
    wchar_t exe[MAX_PATH];
    swprintf(exe, MAX_PATH, L"%lsdbz3.exe", g_dir);
    if (!exists(exe)) { fail(L"Put Play Budokai 3 Access.exe in the game folder, next to dbz3.exe."); return 1; }
    unsigned long long ver = exe_version();
    int plugins = ver >= PORT_WITH_PLUGINS;
    logw(L"port %u.%u.%u.%u: %ls", (unsigned)(ver >> 48), (unsigned)(ver >> 32) & 0xffff, (unsigned)(ver >> 16) & 0xffff,
         (unsigned)ver & 0xffff, plugins ? L"port plugin loader" : L"graphics plugin wrapper");
    int ok = plugins ? repair_plugin() : repair();
    port_settings();
    controller_backend();
    tag_b1_ports();
    if (repair_only) return ok ? 0 : 1;
    english_check();
    skip_launcher();
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        fail(L"Could not start dbz3.exe.");
        return 1;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    logw(L"started the game%ls", ok ? L"" : L" (without the mod)");
    if (g_log) fclose(g_log);
    return 0;
}
