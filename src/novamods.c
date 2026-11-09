/*
 * Budokai 3 Access Mods.exe: installs Nova's Mods (the port's official extra
 * characters, costumes, story and texture packs) from a console window that a
 * screen reader reads, in place of the port's launcher tab, which a screen
 * reader can't use.
 *
 * It does what the port's launcher does (src/launcher/novas_mods.cpp and
 * src/mods.cpp of novapowers0/DBZ-Budokai-3-HD-Collection, port 1.5.6):
 *  - reads catalogo.toml from the Nova's Mods repository with the read-only
 *    key the port ships in novas_mods.key (hex XOR 0x5A, or the token as is);
 *  - downloads the zip (Contents API) or its parts (release assets), checks
 *    the size and SHA-256 from the catalog and that it is a zip;
 *  - moves an installed copy (and the mods listed in "excluye") to
 *    mods_respaldo, never deleting anything, keeps a disabled mod disabled;
 *  - extracts with Windows' tar.exe into mods\.install_<stamp> and places it
 *    like FinishInstall (a "mods" folder of several mods, one wrapping
 *    folder, or the mod itself), named after the zip;
 *  - writes the catalog version into the mod's manifest.txt;
 *  - removes a mod by moving it to mods_respaldo;
 *  - afterwards rebuilds the character roster (mod center hd\roster_build.py
 *    construir), as the launcher does when PLAY is pressed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#define HOST L"api.github.com"
#define REPO "/repos/novapowers0/DBZ3HD-Novas-Mods/"

static wchar_t g_dir[MAX_PATH];     /* game folder, with a trailing backslash */
static char g_token[512];

typedef struct {
    char id[64], version[32], tipo[32], zip[260], sha[72], name[160], desc[1024];
    long long size;
    long long parts[16]; int nparts;
    char excl[4][64]; int nexcl;
} Mod;
static Mod g_mods[256];
static int g_nmods;

/* ---------- small helpers ---------- */

static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int is_dir(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

static void to_wide(const char *s, wchar_t *out, int cap) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap); }

static void stamp(wchar_t *out, int cap) {
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(out, cap, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static int plain_id(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    return 1;
}

static int read_line(char *buf, int cap) {
    if (!fgets(buf, cap, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

/* ---------- key ---------- */

static int load_token(void) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%lsnovas_mods.key", g_dir);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    char hex[1024] = "";
    if (fscanf(f, "%1023s", hex) != 1) hex[0] = 0;
    fclose(f);
    if (!strncmp(hex, "github_pat_", 11)) { snprintf(g_token, sizeof g_token, "%s", hex); return 1; }
    size_t n = 0;
    for (size_t i = 0; hex[i] && hex[i + 1] && n + 1 < sizeof g_token; i += 2) {
        if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1])) return 0;
        char two[3] = { hex[i], hex[i + 1], 0 };
        g_token[n++] = (char)(strtol(two, NULL, 16) ^ 0x5A);
    }
    g_token[n] = 0;
    return n > 0;
}

/* ---------- HTTP (WinHTTP, redirects followed by hand: the signed download
 * address must not get the GitHub key) ---------- */

typedef struct { FILE *file; char *mem; size_t len, cap; long long got, total; DWORD tick; } Sink;

static int sink_write(Sink *s, const char *d, DWORD n) {
    if (s->file) { if (fwrite(d, 1, n, s->file) != n) return 0; }
    else {
        if (s->len + n + 1 > s->cap) {
            size_t c = s->cap ? s->cap * 2 : 65536;
            while (c < s->len + n + 1) c *= 2;
            char *m = realloc(s->mem, c);
            if (!m) return 0;
            s->mem = m; s->cap = c;
        }
        memcpy(s->mem + s->len, d, n); s->len += n; s->mem[s->len] = 0;
    }
    s->got += n;
    if (s->total > (1 << 20) && GetTickCount() - s->tick > 5000) {
        s->tick = GetTickCount();
        say("  %d percent\n", (int)(s->got * 100 / s->total));
    }
    return 1;
}

static int http_get(const wchar_t *host, const wchar_t *path, const char *accept, int with_key, Sink *s, int depth) {
    if (depth > 5) return 0;
    int ok = 0;
    HINTERNET ses = WinHttpOpen(L"Budokai3Access-Mods/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) ses = WinHttpOpen(L"Budokai3Access-Mods/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    if (req) {
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
        wchar_t hdr[1024];
        if (with_key) {
            wchar_t tok[512]; to_wide(g_token, tok, 512);
            wchar_t acc[128]; to_wide(accept, acc, 128);
            swprintf(hdr, 1024, L"Authorization: Bearer %ls\r\nAccept: %ls\r\nX-GitHub-Api-Version: 2022-11-28\r\n", tok, acc);
        } else hdr[0] = 0;
        if (WinHttpSendRequest(req, hdr[0] ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS, hdr[0] ? (DWORD)-1L : 0, NULL, 0, 0, 0) &&
            WinHttpReceiveResponse(req, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
            if (code >= 300 && code < 400) {
                wchar_t loc[4096]; DWORD ls = sizeof loc;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, NULL, loc, &ls, NULL)) {
                    URL_COMPONENTS u; memset(&u, 0, sizeof u); u.dwStructSize = sizeof u;
                    wchar_t h2[256], p2[4096];
                    u.lpszHostName = h2; u.dwHostNameLength = 256; u.lpszUrlPath = p2; u.dwUrlPathLength = 4096;
                    wchar_t extra[4096]; u.lpszExtraInfo = extra; u.dwExtraInfoLength = 4096;
                    if (WinHttpCrackUrl(loc, 0, 0, &u)) {
                        wchar_t full[8192]; swprintf(full, 8192, L"%ls%ls", p2, extra);
                        int same = !_wcsicmp(h2, HOST);
                        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
                        return http_get(h2, full, accept, with_key && same, s, depth + 1);
                    }
                }
            } else if (code == 200) {
                char buf[65536]; DWORD n;
                ok = 1;
                while (WinHttpReadData(req, buf, sizeof buf, &n) && n) if (!sink_write(s, buf, n)) { ok = 0; break; }
            } else say("  The server answered %lu.\n", code);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static char *get_repo_file(const char *file) {
    wchar_t path[1024], wf[512]; to_wide(file, wf, 512);
    swprintf(path, 1024, L"%lscontents/%ls", L"" REPO, wf);
    Sink s; memset(&s, 0, sizeof s);
    if (!http_get(HOST, path, "application/vnd.github.raw", 1, &s, 0)) { free(s.mem); return NULL; }
    return s.mem;
}

/* ---------- catalog (only what the port reads; later entries with the same id win) ---------- */

static void unquote(const char *v, char *out, size_t cap) {
    size_t n = 0;
    if (*v == '"') v++;
    for (; *v && *v != '"' && n + 1 < cap; v++) {
        if (*v == '\\' && v[1]) v++;
        out[n++] = *v;
    }
    out[n] = 0;
}

static void commit(Mod *cur) {
    if (!plain_id(cur->id) || (!cur->zip[0] && !cur->nparts) || strstr(cur->zip, "..") || strpbrk(cur->zip, "?#\\")) return;
    for (int i = 0; i < g_nmods; i++)
        if (!strcmp(g_mods[i].id, cur->id)) { memmove(&g_mods[i], &g_mods[i + 1], sizeof(Mod) * (size_t)(g_nmods - i - 1)); g_nmods--; break; }
    if (!cur->name[0]) snprintf(cur->name, sizeof cur->name, "%s", cur->id);
    if (g_nmods < 256) g_mods[g_nmods++] = *cur;
}

static void field(Mod *cur, const char *k, char *v) {
    if (!strcmp(k, "id")) unquote(v, cur->id, sizeof cur->id);
    else if (!strcmp(k, "version")) unquote(v, cur->version, sizeof cur->version);
    else if (!strcmp(k, "tipo")) unquote(v, cur->tipo, sizeof cur->tipo);
    else if (!strcmp(k, "zip")) { if (!cur->zip[0]) unquote(v, cur->zip, sizeof cur->zip); }
    else if (!strcmp(k, "archivo")) unquote(v, cur->zip, sizeof cur->zip);   /* the port: "archivo" wins over "zip" */
    else if (!strcmp(k, "size")) cur->size = _atoi64(v);
    else if (!strcmp(k, "sha256")) { unquote(v, cur->sha, sizeof cur->sha); _strlwr(cur->sha); }
    else if (!strcmp(k, "nombre_en")) unquote(v, cur->name, sizeof cur->name);
    else if (!strcmp(k, "nombre_es") && !cur->name[0]) unquote(v, cur->name, sizeof cur->name);
    else if (!strcmp(k, "descripcion_en")) unquote(v, cur->desc, sizeof cur->desc);
    else if (!strcmp(k, "partes")) {
        for (char *p = strchr(v, '['); p && cur->nparts < 16;) {
            char *e; long long x = _strtoi64(p + 1, &e, 10);
            if (e == p + 1) break;
            if (x > 0) cur->parts[cur->nparts++] = x;
            p = strchr(e, ',');
        }
    } else if (!strcmp(k, "excluye")) {
        for (char *p = strchr(v, '"'); p && cur->nexcl < 4;) {
            char *e = strchr(p + 1, '"'); if (!e) break;
            snprintf(cur->excl[cur->nexcl++], 64, "%.*s", (int)(e - p - 1), p + 1);
            p = strchr(e + 1, '"');
        }
    }
}

static int parse_catalog(char *text) {
    static Mod cur;
    int in = 0;
    g_nmods = 0;
    for (char *line = text, *nl; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        size_t ln = strlen(l); while (ln && (l[ln - 1] == '\r' || l[ln - 1] == ' ')) l[--ln] = 0;
        if (!strcmp(l, "[[mod]]")) { if (in) commit(&cur); memset(&cur, 0, sizeof cur); in = 1; continue; }
        char *eq = in ? strstr(l, " = ") : NULL;
        if (eq) { *eq = 0; field(&cur, l, eq + 3); }
    }
    if (in) commit(&cur);
    return g_nmods;
}

/* ---------- installed state ---------- */

/* installed version ("" = not installed, "?" = no version), and whether it is switched off */
static void installed(const char *id, char *ver, size_t cap, int *disabled) {
    ver[0] = 0; if (disabled) *disabled = 0;
    wchar_t wid[64]; to_wide(id, wid, 64);
    for (int k = 0; k < 2; k++) {
        wchar_t d[MAX_PATH];
        swprintf(d, MAX_PATH, L"%lsmods\\%ls%ls", g_dir, wid, k ? L".disabled" : L"");
        if (!is_dir(d)) continue;
        if (disabled) {
            wchar_t m[MAX_PATH]; swprintf(m, MAX_PATH, L"%ls\\.disabled", d);
            *disabled = k || exists(m);
        }
        snprintf(ver, cap, "?");
        wchar_t man[MAX_PATH]; swprintf(man, MAX_PATH, L"%ls\\manifest.txt", d);
        FILE *f = _wfopen(man, L"rb");
        if (f) {
            char l[256];
            while (fgets(l, sizeof l, f)) if (!strncmp(l, "version=", 8)) { l[strcspn(l, "\r\n")] = 0; snprintf(ver, cap, "%s", l + 8); break; }
            fclose(f);
        }
        return;
    }
}

/* "4 MB", or "under 1 MB" */
static const char *mb(long long size) {
    static char b[32];
    if (size < (1 << 19)) return "under 1 MB";
    snprintf(b, sizeof b, "%lld MB", (size + (1 << 19)) >> 20);
    return b;
}

static const char *kind(const char *tipo) {
    if (!strcmp(tipo, "personaje")) return "character";
    if (!strcmp(tipo, "traje")) return "costume";
    if (!strcmp(tipo, "campana")) return "story";
    if (!strcmp(tipo, "texturas")) return "texture pack";
    return tipo;
}

/* ---------- files ---------- */

static int move_aside(const wchar_t *path, const wchar_t *name) {
    wchar_t bak[MAX_PATH], st[32];
    swprintf(bak, MAX_PATH, L"%lsmods_respaldo", g_dir);
    CreateDirectoryW(bak, NULL);
    stamp(st, 32);
    swprintf(bak, MAX_PATH, L"%lsmods_respaldo\\%ls_%ls", g_dir, name, st);
    for (int i = 0; i < 10; i++) { if (MoveFileExW(path, bak, 0)) return 1; Sleep(300); }
    return 0;
}

static int run_wait(wchar_t *cmd, const wchar_t *cwd, int show) {
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!show) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, show ? 0 : CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

static int has_mod_content(const wchar_t *d) {
    static const wchar_t *const k[] = { L"us", L"eu", L"personaje.toml", L"traje.toml", L"roster.toml", L"manifest.txt", L"ui_ingame" };
    for (int i = 0; i < 7; i++) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\%ls", d, k[i]); if (exists(p)) return 1; }
    return 0;
}

/* moves dir to mods\name, setting aside what is there; returns 1 on success */
static int publish(const wchar_t *dir, const wchar_t *name) {
    wchar_t dest[MAX_PATH];
    swprintf(dest, MAX_PATH, L"%lsmods\\%ls", g_dir, name);
    if (is_dir(dest) && !move_aside(dest, name)) return 0;
    for (int i = 0; i < 10; i++) { if (MoveFileExW(dir, dest, 0)) return 1; Sleep(300); }
    return 0;
}

/* the extracted zip in staging, placed like the port's FinishInstall; out = the mod's folder name */
static int place(const wchar_t *staging, const wchar_t *name, wchar_t *out) {
    wcscpy(out, name);
    wchar_t inner[MAX_PATH];
    swprintf(inner, MAX_PATH, L"%ls\\mods", staging);
    if (!has_mod_content(staging) && is_dir(inner)) {    /* a pack of several mods */
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*", inner);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        int any = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                wchar_t kid[MAX_PATH]; swprintf(kid, MAX_PATH, L"%ls\\%ls", inner, fd.cFileName);
                if (has_mod_content(kid) && publish(kid, fd.cFileName)) { if (!any) wcscpy(out, fd.cFileName); any = 1; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (any) move_aside(staging, L"pack_documents");
        return any;
    }
    if (!has_mod_content(staging)) {                       /* one wrapping folder */
        wchar_t pat[MAX_PATH], only[MAX_PATH] = L""; int n = 0;
        swprintf(pat, MAX_PATH, L"%ls\\*", staging);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && wcscmp(fd.cFileName, L"__MACOSX")) {
                    n++; swprintf(only, MAX_PATH, L"%ls\\%ls", staging, fd.cFileName);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (n == 1 && has_mod_content(only)) {
            if (!publish(only, name)) return 0;
            move_aside(staging, L"zip_leftovers");
            return 1;
        }
    }
    return publish(staging, name);
}

static void set_manifest_version(const wchar_t *mod, const char *version) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lsmods\\%ls\\manifest.txt", g_dir, mod);
    char buf[65536] = ""; size_t n = 0;
    FILE *f = _wfopen(p, L"rb");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    f = _wfopen(p, L"wb");
    if (!f) return;
    int done = 0;
    for (char *l = buf, *nl; l && *l; l = nl) {
        nl = strchr(l, '\n'); if (nl) nl++;
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if (!strncmp(l, "version=", 8)) { fprintf(f, "version=%s\r\n", version); done = 1; }
        else fwrite(l, 1, len, f);
    }
    if (!done) fprintf(f, "%sversion=%s\r\n", (n && buf[n - 1] != '\n') ? "\r\n" : "", version);
    fclose(f);
}

static int sha_ok(const wchar_t *path, const char *want) {
    if (!want[0]) return 1;
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    int ok = 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) >= 0) {
        FILE *f = _wfopen(path, L"rb");
        if (f) {
            static unsigned char buf[1 << 20]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) BCryptHashData(h, buf, (ULONG)n, 0);
            fclose(f);
            unsigned char d[32]; char hex[65];
            if (BCryptFinishHash(h, d, 32, 0) >= 0) {
                for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
                ok = !strcmp(hex, want);
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ---------- actions ---------- */

static int install(const Mod *m) {
    wchar_t tmp[MAX_PATH], zip[MAX_PATH], wid[64];
    to_wide(m->id, wid, 64);
    swprintf(tmp, MAX_PATH, L"%lsnovas_tmp", g_dir);
    CreateDirectoryW(tmp, NULL);
    swprintf(zip, MAX_PATH, L"%ls\\%ls.zip", tmp, wid);
    ULARGE_INTEGER freeb;
    if (GetDiskFreeSpaceExW(g_dir, &freeb, NULL, NULL) && m->size > 0 && freeb.QuadPart < (unsigned long long)m->size * 2 + (256ull << 20)) {
        say("Not enough free disk space. %lld MB are needed.\n", (m->size * 2 >> 20) + 256);
        return 0;
    }
    say("Downloading %s, %s...\n", m->name, mb(m->size));
    FILE *f = _wfopen(zip, L"wb");
    if (!f) { say("Could not write the download to the game folder.\n"); return 0; }
    Sink s; memset(&s, 0, sizeof s); s.file = f; s.total = m->size; s.tick = GetTickCount();
    int ok = 1;
    if (m->nparts) {
        for (int i = 0; ok && i < m->nparts; i++) {
            wchar_t path[256]; swprintf(path, 256, L"%lsreleases/assets/%lld", L"" REPO, m->parts[i]);
            ok = http_get(HOST, path, "application/octet-stream", 1, &s, 0);
        }
    } else {
        wchar_t path[1024], wz[512]; to_wide(m->zip, wz, 512);
        swprintf(path, 1024, L"%lscontents/%ls", L"" REPO, wz);
        ok = http_get(HOST, path, "application/vnd.github.raw", 1, &s, 0);
    }
    fclose(f);
    char pk[2] = { 0, 0 };
    if ((f = _wfopen(zip, L"rb"))) { fread(pk, 1, 2, f); fclose(f); }
    if (!ok || s.got < 4 || pk[0] != 'P' || pk[1] != 'K' || (m->size > 0 && s.got != m->size)) {
        say("The download failed. Check your internet connection and try again.\n");
        DeleteFileW(zip); return 0;
    }
    say("Checking the download...\n");
    if (!sha_ok(zip, m->sha)) {
        say("The download does not match the catalog (damaged or changed). Nothing was installed.\n");
        DeleteFileW(zip); return 0;
    }
    say("Installing...\n");
    /* an installed copy (on or off) goes to mods_respaldo; a switched-off mod stays off */
    int was_off = 0;
    for (int k = 0; k < 2; k++) {
        wchar_t d[MAX_PATH], nm[80];
        swprintf(nm, 80, L"%ls%ls", wid, k ? L".disabled" : L"");
        swprintf(d, MAX_PATH, L"%lsmods\\%ls", g_dir, nm);
        if (!is_dir(d)) continue;
        wchar_t mark[MAX_PATH]; swprintf(mark, MAX_PATH, L"%ls\\.disabled", d);
        was_off |= k || exists(mark);
        if (!move_aside(d, nm)) { say("The installed copy is open in another program. Close the game and try again.\n"); DeleteFileW(zip); return 0; }
    }
    wchar_t staging[MAX_PATH], st[32], cmd[2048], sys[MAX_PATH];
    stamp(st, 32);
    swprintf(staging, MAX_PATH, L"%lsmods\\.install_%ls", g_dir, st);
    CreateDirectoryW(staging, NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, 2048, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, staging);
    if (run_wait(cmd, NULL, 0) != 0) {
        say("The zip could not be extracted.\n");
        move_aside(staging, L"failed_install");
        DeleteFileW(zip); return 0;
    }
    /* the folder is named after the zip, as the port does */
    wchar_t name[MAX_PATH], out[MAX_PATH];
    const wchar_t *base = wcsrchr(zip, L'\\') + 1;
    swprintf(name, MAX_PATH, L"%.*ls", (int)(wcslen(base) - 4), base);
    if (m->zip[0]) {
        wchar_t wz[512]; to_wide(m->zip, wz, 512);
        const wchar_t *b = wcsrchr(wz, L'/'); b = b ? b + 1 : wz;
        size_t bl = wcslen(b);
        if (bl > 4 && !_wcsicmp(b + bl - 4, L".zip")) swprintf(name, MAX_PATH, L"%.*ls", (int)(bl - 4), b);
    }
    if (!place(staging, name, out)) { say("The mod could not be put in the mods folder. Close the game and try again.\n"); DeleteFileW(zip); return 0; }
    if (m->version[0]) set_manifest_version(out, m->version);
    if (was_off) {
        wchar_t mark[MAX_PATH]; swprintf(mark, MAX_PATH, L"%lsmods\\%ls\\.disabled", g_dir, out);
        FILE *mf = _wfopen(mark, L"wb"); if (mf) fclose(mf);
    }
    for (int i = 0; i < m->nexcl; i++) {                    /* the other version of the same pack */
        if (!plain_id(m->excl[i]) || !strcmp(m->excl[i], m->id)) continue;
        wchar_t we[64]; to_wide(m->excl[i], we, 64);
        for (int k = 0; k < 2; k++) {
            wchar_t d[MAX_PATH], nm[80];
            swprintf(nm, 80, L"%ls%ls", we, k ? L".disabled" : L"");
            swprintf(d, MAX_PATH, L"%lsmods\\%ls", g_dir, nm);
            if (is_dir(d)) move_aside(d, nm);
        }
    }
    DeleteFileW(zip);
    say("%s installed%s.\n", m->name, was_off ? " (left switched off, as it was)" : "");
    return 1;
}

static int uninstall(const Mod *m) {
    wchar_t wid[64]; to_wide(m->id, wid, 64);
    int any = 0;
    for (int k = 0; k < 2; k++) {
        wchar_t d[MAX_PATH], nm[80];
        swprintf(nm, 80, L"%ls%ls", wid, k ? L".disabled" : L"");
        swprintf(d, MAX_PATH, L"%lsmods\\%ls", g_dir, nm);
        if (!is_dir(d)) continue;
        if (!move_aside(d, nm)) { say("It is open in another program. Close the game and try again.\n"); return 0; }
        any = 1;
    }
    if (any) say("%s removed. A copy is kept in the mods_respaldo folder.\n", m->name);
    return any;
}

/* the launcher rebuilds the character roster when PLAY is pressed; the mod skips the launcher */
static void rebuild_roster(void) {
    wchar_t py[MAX_PATH], script[MAX_PATH], cwd[MAX_PATH], us[MAX_PATH], cmd[4096];
    swprintf(py, MAX_PATH, L"%lspython\\python.exe", g_dir);
    swprintf(cwd, MAX_PATH, L"%lsmod center hd", g_dir);
    swprintf(script, MAX_PATH, L"%ls\\roster_build.py", cwd);
    if (!exists(py) || !exists(script)) { say("The port's roster tool is missing; the new characters appear after you press PLAY in the port's launcher once.\n"); return; }
    say("Updating the character list for the game...\n");
    wchar_t mods[MAX_PATH]; swprintf(mods, MAX_PATH, L"%lsmods", g_dir);
    us[0] = 0;
    static const wchar_t *const dirs[] = { L"us", L"assets\\us", L"eu", L"assets\\eu" };
    for (int i = 0; i < 4 && !us[0]; i++) {
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls%ls\\data_cmn.afs", g_dir, dirs[i]);
        if (exists(p)) swprintf(us, MAX_PATH, L"%ls%ls", g_dir, dirs[i]);
    }
    if (us[0]) swprintf(cmd, 4096, L"\"%ls\" \"%ls\" construir --mods \"%ls\" --us \"%ls\"", py, script, mods, us);
    else swprintf(cmd, 4096, L"\"%ls\" \"%ls\" construir --mods \"%ls\"", py, script, mods);
    int r = run_wait(cmd, cwd, 0);
    say(r == 0 ? "Character list updated.\n" : "Updating the character list failed (code %d). Press PLAY once in the port's launcher, or tell the mod's author.\n", r);
}

/* ---------- menu ---------- */

static int list_mods(int *order) {
    static const char *const kinds[] = { "personaje", "traje", "campana", "texturas", NULL };
    int n = 0;
    for (int k = 0; k <= 4; k++)
        for (int i = 0; i < g_nmods; i++) {
            int match = kinds[k] ? !strcmp(g_mods[i].tipo, kinds[k]) : 1;
            if (!kinds[k]) for (int j = 0; j < 4; j++) if (!strcmp(g_mods[i].tipo, kinds[j])) match = 0;
            if (match) order[n++] = i;
        }
    say("\n");
    for (int j = 0; j < n; j++) {
        const Mod *m = &g_mods[order[j]];
        char ver[32]; int off;
        installed(m->id, ver, sizeof ver, &off);
        char state[96];
        if (!ver[0]) snprintf(state, sizeof state, "not installed");
        else if (m->version[0] && strcmp(ver, m->version)) snprintf(state, sizeof state, "installed, update available%s", off ? ", switched off" : "");
        else snprintf(state, sizeof state, "installed%s", off ? ", switched off" : "");
        say("%d. %s, %s, %s\n", j + 1, m->name, kind(m->tipo), state);
    }
    return n;
}

int wmain(int argc, wchar_t **argv) {
    (void)argc; (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Budokai 3 Access Mods");
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0;
    wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%lsdbz3.exe", g_dir);
    say("Budokai 3 Access Mods: Nova's Mods for the Budokai 3 HD port.\n");
    if (!exists(exe)) { say("Put this program in the game folder, next to dbz3.exe. Press Enter to close.\n"); getchar(); return 1; }
    if (!load_token()) { say("The port's novas_mods.key file is missing, so Nova's Mods can't be downloaded. It comes with port 1.5 or later. Press Enter to close.\n"); getchar(); return 1; }
    say("Getting the list of mods...\n");
    char *cat = get_repo_file("catalogo.toml");
    if (!cat || !parse_catalog(cat)) { say("Could not get the list. Check your internet connection. Press Enter to close.\n"); getchar(); return 1; }
    free(cat);
    int changed = 0, order[256];
    for (;;) {
        int n = list_mods(order);
        say("\nType a number and press Enter to choose a mod, or press Enter alone to finish.\n");
        char in[64];
        if (!read_line(in, sizeof in) || !in[0]) break;
        int k = atoi(in);
        if (k < 1 || k > n) { say("There is no number %s.\n", in); continue; }
        const Mod *m = &g_mods[order[k - 1]];
        char ver[32]; int off;
        installed(m->id, ver, sizeof ver, &off);
        say("\n%s, %s, version %s, %s.\n", m->name, kind(m->tipo), m->version[0] ? m->version : "?", mb(m->size));
        if (m->desc[0]) say("%s\n", m->desc);
        if (ver[0]) {
            say("Installed: version %s%s.\n", ver, off ? ", switched off" : "");
            say("Type I to install it again or update it, R to remove it, or press Enter to go back.\n");
        } else say("Type I to install it, or press Enter to go back.\n");
        if (!read_line(in, sizeof in)) break;
        if (in[0] == 'i' || in[0] == 'I') changed |= install(m);
        else if (ver[0] && (in[0] == 'r' || in[0] == 'R')) changed |= uninstall(m);
    }
    if (changed) rebuild_roster();
    say("Done. Start the game with Play Budokai 3 Access.exe. Press Enter to close.\n");
    getchar();
    return 0;
}
