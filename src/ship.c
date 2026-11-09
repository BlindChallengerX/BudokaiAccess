/*
 * Babidi's Spaceship (port 1.5.6, Gateway > Babidi's Spaceship, from the
 * player's Budokai 2 disc). The port draws the mode itself (pasarela_nave.cpp)
 * from disc pictures; none of its state is in guest memory and the only ImGui
 * text is the button legend. What the mod has instead:
 *  - the port's log (logs\dbz3_NNN.log): a line for every screen change
 *    ("dbz3 nave: pantalla modo / planta N / rueda 1P / menu / rueda COM /
 *    burbuja / resultado / otra"), every cursor sound ("efecto canal 0 id 2"
 *    = the cursor moved), the wheel's starting place ("restaura ... pagina P
 *    casilla C"), page changes, each fight's start and end, the Kili result
 *    and the prizes;
 *  - user_data\dbz3\pasarela\progreso.toml: nave_planta (the floor cursor,
 *    written on every move), nave_pers_lo/hi (fighters unlocked, bit = B2 ID),
 *    nave_esc (stages unlocked);
 *  - the button pressed last (pad.c), which says which way a cursor sound
 *    moved: left/right on the wheel (wraps; "?" is the last place), up/down on
 *    the Normal/Custom/Edit Skills menu (wraps), left/right on the stage
 *    bubble (stops at the ends; Y jumps to "?").
 * Names on the first wheel page (the Budokai 2 cast) come from the mod's own
 * character names; later pages show the name as ImGui text, which imgui.c
 * reads, so only "Random" is said there.
 */
#include "access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static long prog_int(const char *key, long def);

/* speech, off while the log of a session already running is replayed to catch up */
static int g_silent;
static void say(int interrupt, const char *fmt, ...) {
    if (g_silent) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    speak_utf8(buf, interrupt);
}

/* pasarela_nave.cpp: wheel ring order (B2 IDs) and B2 -> B3 IDs (B3DeB2, B1aB3) */
static const int k_ring_b2[29] = { 0, 2, 28, 24, 33, 6, 4, 8, 7, 21, 35, 32, 29, 30, 31, 25, 20, 18, 17, 16, 15, 11, 12, 9, 10, 3, 5, 27, 26 };
static const int k_b1_b3[25] = { 0, 2, 3, 7, 10, 8, 11, 12, 13, 18, 19, 20, 21, 38, 19, 27, 28, 29, 30, 32, 33, 14, 42, 43, 5 };
static int b3_of_b2(int c) {
    switch (c) {
    case 25: return 32; case 26: return 6; case 27: return 9; case 28: return 4; case 29: return 34;
    case 30: return 35; case 31: return 36; case 32: return 37; case 33: return 15; case 35: return 16;
    }
    return c >= 0 && c < 25 ? k_b1_b3[c] : -1;
}
/* stage bubble slots (kEscenas2) and which save bit opens which slot (kBitCasilla) */
static const char *const k_stages[10] = { "Plains", "World Tournament", "Snowy Mountains", "Sea Tower", "Urban Area",
                                          "Red Ribbon Base", "Temple", "Hyperbolic Time Chamber", "Namek", "Random stage" };
static const int k_bit_slot[9] = { 0, 4, 7, 1, 5, 2, 6, 3, 8 };
static const char *const k_floors[4] = {
    "Floor 1, survival: one opponent after another, and your health doesn't come back. 25 Kili for each opponent you beat.",
    "Floor 2, endurance: one health bar each. Last as long as you can, up to 10 minutes. 2 Kili for each second.",
    "Floor 3, hits: 99 seconds to land as many hits as you can. 4 Kili for each hit.",
    "Floor 4, ki deflect: 99 seconds. The opponent doesn't attack. Send ki blasts back by guarding just as they hit. 3 Kili for each one.",
};

/* The port's Budokai 2 main menu (DibujarPrincipalB2, pasarela_misiones.cpp DrawMenuB2):
 * leaving the ship lands here on the ship's row; up/down wrap; the ship row is there once
 * it is unlocked (progreso.toml tablero_b2_nave). B goes back to the HUB. */
static const char *const k_b2menu[7] = { "Dragon World", "Duel", "World Tournament", "Practice", "Edit Skills", "Options", "Babidi's Spaceship" };
static int g_b2menu = -1;                         /* its cursor, -1 = not on that menu */
static int g_b2cursor = 6;

static int b2menu_rows(void) { return prog_int("tablero_b2_nave", 0) ? 7 : 6; }
static void say_b2menu(int interrupt, const char *head) {
    int n = b2menu_rows();
    if (g_b2cursor >= n) g_b2cursor = 0;
    if (head) say(interrupt, "%s. %s, %d of %d", head, k_b2menu[g_b2cursor], g_b2cursor + 1, n);
    else say(interrupt, "%s, %d of %d", k_b2menu[g_b2cursor], g_b2cursor + 1, n);
}

enum { S_NONE, S_MODE, S_FLOOR, S_WHEEL, S_MENU, S_BUBBLE, S_RESULT };
static int g_scr = S_NONE, g_active;
static int g_page, g_idx, g_menu, g_bubble, g_2p_sel;
static int g_ring[29], g_nring;                   /* page 0: unlocked B3 IDs in ring order */
static int g_extra[128], g_nextra;                /* later pages: B3 IDs, 29 a page */
static int g_cells[10], g_ncells;
static int g_last_dir;                            /* last button: 1 up 2 down 4 left 8 right, 0x8000 Y, other 0 */
static int g_wheel_cpu;
static DWORD g_enter_at;                          /* when the ship or the B2 menu opened */                           /* the wheel picks the opponent */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_log_name[MAX_PATH];
static char g_part[1024];
static size_t g_npart;
static DWORD g_skip_at;                           /* "dbz3 modos: sin menu final": the port skips the after-match menu */

static long prog_int(const char *key, long def) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsuser_data\\dbz3\\pasarela\\progreso.toml", g_game_dir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return def;
    char line[256]; long v = def; size_t kn = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kn) || (line[kn] != ' ' && line[kn] != '=')) continue;
        char *eq = strchr(line, '=');
        if (eq) v = strtol(eq + 1, NULL, 0);
        break;
    }
    fclose(f);
    return v;
}

static void build_ring(void) {
    unsigned long lo = (unsigned long)prog_int("nave_pers_lo", 0x1C0002D1), hi = (unsigned long)prog_int("nave_pers_hi", 0);
    g_nring = 0;
    for (int i = 0; i < 29; i++) {
        int b2 = k_ring_b2[i];
        int open = b2 < 32 ? (lo >> b2) & 1 : (hi >> (b2 - 32)) & 1;
        if (open) g_ring[g_nring++] = b3_of_b2(b2);
    }
}

static void build_cells(void) {
    unsigned long esc = (unsigned long)prog_int("nave_esc", 0x05);
    g_ncells = 0;
    for (int c = 0; c < 9; c++)
        for (int b = 0; b < 9; b++)
            if (k_bit_slot[b] == c && ((esc >> b) & 1)) { g_cells[g_ncells++] = c; break; }
    g_cells[g_ncells++] = 9;
}

static int page_size(void) {
    if (g_page == 0) return g_nring;
    int first = (g_page - 1) * 29, n = g_nextra - first;
    return n < 0 ? 0 : n > 29 ? 29 : n;
}

static void say_wheel(int interrupt, const char *head) {
    int n = page_size();
    char name[64] = "";
    if (g_idx >= n) snprintf(name, sizeof name, "Random");
    else if (g_page == 0 && !ship_char_name(g_ring[g_idx], name, sizeof name)) snprintf(name, sizeof name, "Character %d", g_ring[g_idx]);
    if (!name[0] && !head) return;                /* later pages: the port draws the name, imgui.c reads it */
    if (head && name[0]) say(interrupt, "%s. %s", head, name);
    else if (head) say(interrupt, "%s", head);
    else say(interrupt, "%s", name);
}

static void say_floor(int interrupt) {
    long f = prog_int("nave_planta", 0);
    if (f < 0 || f > 3) f = 0;
    say(interrupt, "%s", k_floors[f]);
}

static void say_menu(int interrupt) {
    static const char *const m[3] = { "Normal", "Custom", "Edit Skills" };
    say(interrupt, "%s, %d of 3", m[g_menu], g_menu + 1);
}

static void say_bubble(int interrupt, const char *head) {
    if (g_bubble < 0) g_bubble = 0;
    if (g_bubble >= g_ncells) g_bubble = g_ncells - 1;
    const char *s = k_stages[g_cells[g_bubble]];
    if (head) say(interrupt, "%s. %s, %d of %d", head, s, g_bubble + 1, g_ncells);
    else say(interrupt, "%s, %d of %d", s, g_bubble + 1, g_ncells);
}

/* the cursor sound: move the mirrored cursor the way the last button points */
static void cursor_moved(void) {
    int d = g_last_dir;
    switch (g_scr) {
    case S_MODE:
        if (d & 3) { g_2p_sel = !g_2p_sel; say(1, "%s", g_2p_sel ? "2 P" : "1 P"); }
        break;
    case S_FLOOR:
        say_floor(1);
        break;
    case S_WHEEL: {
        int n = page_size() + 1;
        if (d & 4) g_idx = (g_idx + n - 1) % n;
        else if (d & 8) g_idx = (g_idx + 1) % n;
        else return;
        say_wheel(1, NULL);
        break;
    }
    case S_MENU:
        if (d & 1) g_menu = (g_menu + 2) % 3;
        else if (d & 2) g_menu = (g_menu + 1) % 3;
        else return;
        say_menu(1);
        break;
    case S_BUBBLE:
        if (d & 4) g_bubble--;
        else if (d & 8) g_bubble++;
        else if (d & 0x8000) g_bubble = g_ncells - 1;
        else return;
        say_bubble(1, NULL);
        break;
    }
}

static const char *after(const char *line, const char *key) {
    const char *p = strstr(line, key);
    return p ? p + strlen(key) : NULL;
}

static void handle(const char *line) {
    const char *p;
    /* Budokai 2 main menu */
    if (strstr(line, "dbz3 misiones: salida de la nave al menu principal B2")) g_b2cursor = 6;
    if (strstr(line, "dbz3 misiones: nave paso") && strstr(line, ": menu principal B2")) {
        g_b2menu = 1; g_active = 0; g_scr = S_NONE; g_enter_at = GetTickCount();
        say_b2menu(0, "Budokai 2 main menu");
        return;
    }
    /* the row picked ("fila N", the real cursor): Dragon World and the ship are the port's,
     * the other rows open the game's own screens, coming back here afterwards */
    /* Dragon World and the ship stay in the port's screens (2 = busy, not reading the menu) */
    if ((p = after(line, "dbz3 nave: menu B2 salida fila "))) { g_b2cursor = atoi(p); g_b2menu = (g_b2cursor == 0 || g_b2cursor == 6) ? 2 : 0; return; }
    if ((p = after(line, "dbz3 misiones: menu principal B2 fila "))) { g_b2cursor = atoi(p); g_b2menu = 0; return; }
    if (strstr(line, "dbz3 misiones: vuelve al menu principal B2")) {
        g_b2menu = 1; g_enter_at = GetTickCount();
        say_b2menu(0, "Budokai 2 main menu");
        return;
    }
    if (strstr(line, "dbz3 misiones: menu principal B2 -> HUB")) { g_b2menu = -1; return; }
    /* replayed cursor sounds can't be placed (which button was it?): skipped */
    if (g_silent && strstr(line, "dbz3 nave: efecto canal")) return;
    if (g_b2menu == 1 && strstr(line, "dbz3 nave: efecto canal 0 id 2 ")) {
        int n = b2menu_rows();
        if (g_last_dir & 1) g_b2cursor = (g_b2cursor + n - 1) % n;
        else if (g_last_dir & 2) g_b2cursor = (g_b2cursor + 1) % n;
        else return;
        say_b2menu(1, NULL);
        return;
    }
    if ((p = after(line, "dbz3 nave: entrar ("))) {
        g_enter_at = GetTickCount();
        g_active = 1; g_b2menu = -1; g_menu = 0; g_2p_sel = 0; g_page = 0;
        build_ring();
        say(1, "Babidi's Spaceship. Ship bar: %d Kili", atoi(p));
        log_line("ship: entered");
        return;
    }
    if ((p = after(line, "dbz3 nave: roster de la rueda: "))) {
        const char *ids = strstr(p, "extra: ");
        g_nextra = 0;
        if (ids) for (ids += 7; *ids && g_nextra < 128; ) {
            char *end; long v = strtol(ids, &end, 10);
            if (end == ids) break;
            g_extra[g_nextra++] = (int)v; ids = end;
        }
        return;
    }
    if (!g_active) return;
    if ((p = after(line, "dbz3 nave: pantalla "))) {
        if (!strncmp(p, "modo", 4)) { g_scr = S_MODE; say(0, "How many players? %s", g_2p_sel ? "2 P" : "1 P"); }
        else if (!strncmp(p, "planta", 6)) { g_scr = S_FLOOR; say(0, "Pick a floor"); say_floor(0); }
        else if (!strncmp(p, "rueda", 5)) {
            g_scr = S_WHEEL;
            g_wheel_cpu = strstr(p, "COM") != NULL;
            say_wheel(0, strstr(p, "COM") ? "Pick your opponent" : strstr(p, "2P") ? "Player 2, pick your fighter" : "Pick your fighter");
        }
        else if (!strncmp(p, "menu", 4)) { g_scr = S_MENU; say_menu(0); }
        else if (!strncmp(p, "burbuja", 7)) { g_scr = S_BUBBLE; build_cells(); g_bubble = 0; say_bubble(0, "Pick the stage"); }
        else if (!strncmp(p, "resultado", 9)) g_scr = S_RESULT;
        else if (!strncmp(p, "otra", 4)) { g_scr = S_NONE; say(0, "Next opponent"); }
        return;
    }
    if ((p = after(line, "dbz3 nave: efecto canal 0 id 2 "))) { cursor_moved(); return; }
    if ((p = after(line, "dbz3 nave: restaura "))) {
        const char *pg = strstr(p, "pagina "), *cs = strstr(p, "casilla ");
        if (pg) g_page = atoi(pg + 7) - 1;
        if (cs) g_idx = atoi(cs + 8);
        if (g_page < 0) g_page = 0;
        return;
    }
    if ((p = after(line, "dbz3 nave: pagina "))) {
        g_page = atoi(p) - 1; g_idx = 0;
        const char *sl = strchr(p, '/');
        say(1, "Page %d of %d", g_page + 1, sl ? atoi(sl + 1) : 0);
        say_wheel(0, NULL);
        return;
    }
    if ((p = after(line, "dbz3 nave: traje "))) {   /* "traje T id I -> T2 de N" */
        const char *ar = strstr(p, "-> "), *de = strstr(p, " de ");
        if (ar && de) say(1, "Costume %d of %d", atoi(ar + 3) + 1, atoi(de + 4));
        return;
    }
    if ((p = after(line, "dbz3 nave: menu ")) && strstr(p, "lista de capsulas")) return;
    if ((p = after(line, "dbz3 nave: prueba I: el rival se levanta; combate "))) { say(1, "Opponent %d", atoi(p)); return; }
    if ((p = after(line, "dbz3 nave: prueba "))) {   /* "N: A contra B en STAGE (vida ...)" */
        char a[64] = "", b[64] = "";
        const char *c = strstr(p, ": "), *v = strstr(p, " contra "), *e = v ? strstr(v, " en ") : NULL;
        if (c && v && e) {
            snprintf(a, sizeof a, "%.*s", (int)(v - c - 2), c + 2);
            snprintf(b, sizeof b, "%.*s", (int)(e - v - 8), v + 8);
            say(1, "Floor %d: %s versus %s", atoi(p), a, b);
        }
        return;
    }
    if ((p = after(line, "dbz3 nave: fin: gana el lado "))) {
        int w = atoi(p);
        say(1, "%s", w == 1 ? "You won" : w == 2 ? "You lost" : "Fight over");
        return;
    }
    if ((p = after(line, "dbz3 nave: resultado de la prueba "))) {   /* "N: A x B = C Kili (barra D, ...)" */
        int n = 0, a = 0, b = 0, c = 0, d = 0;
        static const char *const unit[4] = { "opponents beaten", "seconds", "hits", "ki blasts sent back" };
        if (sscanf(p, "%d: %d x %d = %d Kili (barra %d", &n, &a, &b, &c, &d) == 5 && n >= 1 && n <= 4)
            say(0, "Result: %d %s, %d Kili", a, unit[n - 1], c);
        return;
    }
    if ((p = after(line, "dbz3 nave: barra de la nave: "))) { say(0, "Ship bar: %d Kili", atoi(p)); return; }
    if ((p = after(line, "dbz3 nave: premio: "))) {   /* "NAME (ID i, k Kili, maximo m) nuevo" */
        const char *par = strstr(p, " (");
        if (par) say(0, "Prize: %.*s%s. X turns the card over, A closes it", (int)(par - p), p, strstr(par, " nuevo") ? ", new" : "");
        return;
    }
}

/* the newest logs\dbz3_*.log, opened at its end */
static void open_log(void) {
    wchar_t pat[MAX_PATH], best[MAX_PATH] = L"";
    FILETIME bt = { 0, 0 };
    swprintf(pat, MAX_PATH, L"%lslogs\\dbz3_*.log", g_game_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; wcscpy(best, fd.cFileName); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best[0] || !wcscmp(best, g_log_name)) return;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lslogs\\%ls", g_game_dir, best);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    g_log = f;
    wcscpy(g_log_name, best);
    /* a log already running: start at its end; the port writes a new one per launch, so
     * the "roster de la rueda" line of this session is read from the start */
    char *buf = NULL;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < (16 << 20) && (buf = malloc((size_t)sz.QuadPart + 1))) {
        DWORD got = 0;
        ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL);
        buf[got] = 0;
        /* replay this session's ship lines silently: the mod may start (or reload) mid-way */
        g_silent = 1;
        for (char *ln = buf, *nl; ln && *ln; ln = nl ? nl + 1 : NULL) {
            nl = strchr(ln, '\n'); if (nl) *nl = 0;
            if (strstr(ln, "dbz3 nave: ") || strstr(ln, "dbz3 misiones: ")) handle(ln);
            else if (strstr(ln, "dbz3 pasarela: ")) hercule_line(ln, 1);
        }
        g_silent = 0;
        free(buf);
    }
    g_npart = 0;
    log_line("ship: following %ls", best);
}

int ship_busy(void) { return g_active || g_b2menu > 0; }
int port_menu_skipped(void) { return g_skip_at && GetTickCount() - g_skip_at < 4000; }

void ship_poll(void) {
    static DWORD last_open;
    DWORD now = GetTickCount();
    if (now - last_open > 5000) { last_open = now; open_log(); }
    uint16_t e = pad_ui_take_edges();
    if (e & 0x800f) g_last_dir = e & 0x800f;
    else if (e) g_last_dir = 0;
    hercule_poll();
    /* the ship has no highlighted ImGui rows; one of the port's menus being up means we left it */
    /* (the Gateway list stays up for a moment as the ship opens) */
    if ((g_active || g_b2menu > 0) && im_menu_on() && now - g_enter_at > 4000) { g_active = 0; g_b2menu = -1; g_scr = S_NONE; log_line("ship: left"); }
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[8192];
    DWORD got;
    while (ReadFile(g_log, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                g_part[g_npart] = 0;
                if (strstr(g_part, "dbz3 nave: ") || strstr(g_part, "dbz3 misiones: ")) handle(g_part);
                else if (strstr(g_part, "dbz3 pasarela: ")) hercule_line(g_part, 0);
                else if (strstr(g_part, "dbz3 modos: sin menu final")) g_skip_at = GetTickCount();
                g_npart = 0;
            } else if (ch != '\r' && g_npart < sizeof g_part - 1) g_part[g_npart++] = ch;
        }
        if (got < sizeof buf) break;
    }
}
