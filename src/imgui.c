/*
 * The port's own screens (Dragon HUB, Tag Team, Online, Hero Mode, help
 * pop-ups...) are not game pictures: from port 1.5 they are drawn with Dear
 * ImGui, which rexruntime.dll exports by name. Every piece of text goes
 * through ImDrawList::AddText (the font overload; the short overload and
 * ImGui's widgets call it), so hooking that one export sees all of it.
 *
 * The render thread collects one frame of text at a time; ImGui::NewFrame
 * publishes the finished frame for the mod thread (im_frame).
 */
#include "access.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

typedef struct { float x, y; } ImVec2;
typedef void (__cdecl *addtext_fn)(void *self, void *font, float size, const ImVec2 *pos, uint32_t col,
                                   const char *b, const char *e, float wrap, const void *clip);
typedef void (__cdecl *newframe_fn)(void);
typedef void (__cdecl *addrect_fn)(void *self, const ImVec2 *a, const ImVec2 *b, uint32_t col, float rounding, int flags, float thick);

static addtext_fn o_addtext;
static newframe_fn o_newframe;
static addrect_fn o_addrect;
static void *g_addtext_target, *g_newframe_target, *g_addrect_target;

/* The port's highlighted row: style::Highlight frames the row's stripe with a
 * thin turquoise rectangle, IM_COL32(165, 244, 230, 170). */
#define HL_COL 0xaae6f4a5u
typedef struct { void *list; float y0, y1; } HlRect;
#define HL_MAX 8
static HlRect g_hl_build[HL_MAX], g_hl_done[HL_MAX];
static int g_nhl_build, g_nhl_done;

/* Two frames: the one being drawn and the last finished one. */
static ImText g_build[IM_MAX], g_done[IM_MAX];
static int g_nbuild, g_ndone;
static volatile LONG g_frame_no;
static CRITICAL_SECTION g_lock;
static int g_ready;
volatile LONG g_imlog_left;

static void __cdecl h_addtext(void *self, void *font, float size, const ImVec2 *pos, uint32_t col,
                              const char *b, const char *e, float wrap, const void *clip) {
    InterlockedIncrement(&g_inflight);
    if (b && g_nbuild < IM_MAX && (col >> 24) != 0) {
        size_t n = e ? (size_t)(e - b) : strlen(b);
        if (n > 0 && n < IM_TEXT) {
            ImText *t = &g_build[g_nbuild++];
            memcpy(t->s, b, n); t->s[n] = 0;
            t->x = pos ? pos->x : 0; t->y = pos ? pos->y : 0;
            t->col = col; t->size = size; t->list = self;
        }
    }
    o_addtext(self, font, size, pos, col, b, e, wrap, clip);
    InterlockedDecrement(&g_inflight);
}

static void __cdecl h_addrect(void *self, const ImVec2 *a, const ImVec2 *b, uint32_t col, float rounding, int flags, float thick) {
    InterlockedIncrement(&g_inflight);
    if (col == HL_COL && a && b && g_nhl_build < HL_MAX) {
        HlRect *r = &g_hl_build[g_nhl_build++];
        r->list = self; r->y0 = a->y; r->y1 = b->y;
    }
    o_addrect(self, a, b, col, rounding, flags, thick);
    InterlockedDecrement(&g_inflight);
}

static void __cdecl h_newframe(void) {
    InterlockedIncrement(&g_inflight);
    EnterCriticalSection(&g_lock);
    memcpy(g_done, g_build, sizeof(ImText) * (size_t)g_nbuild);
    g_ndone = g_nbuild;
    memcpy(g_hl_done, g_hl_build, sizeof(HlRect) * (size_t)g_nhl_build);
    g_nhl_done = g_nhl_build;
    g_nhl_build = 0;
    InterlockedIncrement(&g_frame_no);
    LeaveCriticalSection(&g_lock);
    g_nbuild = 0;
    o_newframe();
    InterlockedDecrement(&g_inflight);
}

/* Copies the last finished frame; returns how many texts (0 if none). */
static HlRect g_hl[HL_MAX];
static int g_nhl;

int im_frame(ImText *out, int max, LONG *frame_no) {
    if (!g_ready) return 0;
    EnterCriticalSection(&g_lock);
    int n = g_ndone < max ? g_ndone : max;
    memcpy(out, g_done, sizeof(ImText) * (size_t)n);
    memcpy(g_hl, g_hl_done, sizeof g_hl);
    g_nhl = g_nhl_done;
    if (frame_no) *frame_no = g_frame_no;
    LeaveCriticalSection(&g_lock);
    return n;
}

int im_ready(void) { return g_ready; }

void im_init(void) {
    HMODULE rt = GetModuleHandleW(L"rexruntime.dll");
    if (!rt) { log_line("imgui: no rexruntime.dll"); return; }
    g_addtext_target = (void *)GetProcAddress(rt, "?AddText@ImDrawList@@QEAAXPEAUImFont@@MAEBUImVec2@@IPEBD2MPEBUImVec4@@@Z");
    g_newframe_target = (void *)GetProcAddress(rt, "?NewFrame@ImGui@@YAXXZ");
    g_addrect_target = (void *)GetProcAddress(rt, "?AddRect@ImDrawList@@QEAAXAEBUImVec2@@0IMHM@Z");
    if (!g_addtext_target || !g_newframe_target) { log_line("imgui: exports not found (port too old?)"); return; }
    InitializeCriticalSection(&g_lock);
    if (MH_CreateHook(g_addtext_target, (void *)h_addtext, (void **)&o_addtext) != MH_OK ||
        MH_CreateHook(g_newframe_target, (void *)h_newframe, (void **)&o_newframe) != MH_OK) {
        log_line("imgui: hooks failed"); return;
    }
    MH_EnableHook(g_addtext_target);
    MH_EnableHook(g_newframe_target);
    if (g_addrect_target && MH_CreateHook(g_addrect_target, (void *)h_addrect, (void **)&o_addrect) == MH_OK) MH_EnableHook(g_addrect_target);
    else g_addrect_target = NULL;
    g_ready = 1;
    log_line("imgui: text hooks on");
}

void im_shutdown(void) {
    if (!g_ready) return;
    g_ready = 0;
    MH_DisableHook(g_addtext_target);
    MH_DisableHook(g_newframe_target);
    MH_RemoveHook(g_addtext_target);
    MH_RemoveHook(g_newframe_target);
    if (g_addrect_target) { MH_DisableHook(g_addrect_target); MH_RemoveHook(g_addrect_target); }
}

/* imdump: log every text of the last frame with position and colour. */
void im_dump(void) {
    static ImText f[IM_MAX];
    LONG no = 0;
    int n = im_frame(f, IM_MAX, &no);
    log_line("imgui frame %ld: %d texts", no, n);
    for (int i = 0; i < n; i++)
        log_line("  [%d] list %p at %.0f,%.0f col %08x size %.0f: %s", i, f[i].list, f[i].x, f[i].y, f[i].col, f[i].size, f[i].s);
}

/* ---------- reader ----------
 * A frame's texts become lines: repeats are dropped (outlined titles are
 * drawn a dozen times, shifted by a pixel). Lines that continue one another
 * (same draw list and size, next line down) form a paragraph, since wrapped
 * descriptions are drawn line by line.
 * A line is spoken once it has been up for a moment and forgotten once it is
 * gone, like the game text in text.c; the new lines of one paragraph are
 * spoken together, so a box whose last line stays (the HUB's "Rank: ...")
 * only says what changed. A spot on screen whose text keeps changing (a
 * timer, a counter) goes quiet for a while. */
typedef struct { const char *s; void *list; float x, y, size; int para; int kind; int off; } Line;

/* Menu rows of the port's screens are drawn by its style::MenuText in fixed
 * colours (top half of the letters): the highlighted row, an ordinary row, a
 * row that can't be chosen. Ordinary rows aren't read as they appear; the
 * highlighted one is read when it changes. */
enum { K_TEXT, K_ROW, K_ROW_SEL, K_ROW_OFF };
static int kind_of(uint32_t col) {
    if (col == 0xffe7d202u) return K_ROW_SEL;   /* IM_COL32(2, 210, 231, 255) */
    if (col == 0xffc3b228u) return K_ROW;       /* IM_COL32(40, 178, 195, 255) */
    if (col == 0xebac9e96u) return K_ROW_OFF;   /* IM_COL32(150, 158, 172, 235) */
    return K_TEXT;
}

static int build_lines(ImText *f, int n, Line *l, int maxl) {
    int nl = 0, para = -1;
    for (int i = 0; i < n && nl < maxl; i++) {
        ImText *t = &f[i];
        if (!strcmp(t->s, "Debug")) continue;          /* ImGui's implicit window */
        int kind = kind_of(t->col), dup = 0;
        for (int i2 = 0; i2 < g_nhl && kind != K_TEXT; i2++)      /* in the highlight stripe */
            if (g_hl[i2].list == t->list && t->y >= g_hl[i2].y0 - 2 && t->y <= g_hl[i2].y1) kind = K_ROW_SEL;
        int off = t->col == 0xebac9e96u;
        for (int j = 0; j < nl && !dup; j++)
            if (!strcmp(l[j].s, t->s)) {
                dup = 1;
                if (kind == K_ROW_SEL || (kind != K_TEXT && l[j].kind == K_TEXT)) l[j].kind = kind;
                l[j].off |= off;
            }
        if (dup) continue;
        Line *last = nl ? &l[nl - 1] : NULL;
        float gap = last ? t->y - last->y : 0;
        int joins = last && last->list == t->list && last->size == t->size && t->size > 0 &&
                    gap > t->size * 0.5f && gap < t->size * 1.9f;
        if (!joins) para++;
        l[nl].s = t->s; l[nl].list = t->list; l[nl].x = t->x; l[nl].y = t->y; l[nl].size = t->size; l[nl].para = para; l[nl].kind = kind; l[nl].off = off;
        nl++;
    }
    /* Menu rows stand alone; a row-coloured line inside a block of lines is text. */
    for (int k = 0; k < nl; k++) {
        if (l[k].kind == K_TEXT) continue;
        int lines = 0;
        for (int j = 0; j < nl; j++) lines += l[j].para == l[k].para;
        if (lines > 1) l[k].kind = K_TEXT;
    }
    /* A row colour only makes a row when a highlighted row of that list and size is up
     * (the transform selector's "Ki to transform" line uses the row colour too). */
    for (int k = 0; k < nl; k++) {
        if (l[k].kind != K_ROW && l[k].kind != K_ROW_OFF) continue;
        int list = 0;
        for (int j = 0; j < nl && !list; j++) list = l[j].kind == K_ROW_SEL && l[j].list == l[k].list && l[j].size == l[k].size;
        if (!list) l[k].kind = K_TEXT;
    }
    return nl;
}

/* The HUB's rows are pictures; the description under them names the row. */
static const struct { const char *start, *name; } k_rows[] = {
    { "Tag Team, Online, quick battle", "HUB" },          /* main menu row */
    { "Teams of 2 or 3 fighters", "Tag Team" },
    { "Play against another person over the internet", "Online" },
    { "A fight right away against the computer", "Quick Battle" },
    { "Tower, Time Towers", "Tower" },
    { "Survival, Survival with shop", "Survival" },
    { "Kumite, Boss Kumite", "Kumite" },
    { "Each fighter's rank", "Ranking" },
    { "Practice against a dummy", "Laboratory" },
    { "Stories and missions from Budokai 2", "Gateway" },
    { "Needs your copy of Dragon Ball Z Budokai (PS2)", "Legend of Hercule, not available" },
    { "A hundred missions by saga", "Mission 100" },
    /* Tag Team's first screen */
    { "Your team against the computer's", "1 P versus computer" },
    { "Two players, each picks a team", "1 P versus 2 P" },
    { "Watch two teams of your choice", "Computer versus computer" },
};

/* Lines that come back over and over: the HUB's status line under every row
 * ("Rank: Kyu 5 (0 PE)") and button legends (LT Previous, RT Next...). Said
 * the first time, then at most every 30 seconds (F2 reads them any time).
 * An entry ending in a space is a prefix. */
static const char *k_tails[] = { "Rank: ", "Previous", "Next", "Close", "Don't show again", "Finish", "BETA", "EXPERIMENTAL" };
#define NTAILS (sizeof k_tails / sizeof k_tails[0])
static DWORD g_tail_said[NTAILS];

static int tail_quiet(const char *line) {
    DWORD now = GetTickCount();
    for (size_t i = 0; i < NTAILS; i++) {
        size_t n = strlen(k_tails[i]);
        int match = k_tails[i][n - 1] == ' ' ? !strncmp(line, k_tails[i], n) : !strcmp(line, k_tails[i]);
        if (!match) continue;
        if (g_tail_said[i] && now - g_tail_said[i] < 30000) return 1;
        g_tail_said[i] = now;
        return 0;
    }
    return 0;
}

static const char *row_name(const char *text) {
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++)
        if (!strncmp(text, k_rows[i].start, strlen(k_rows[i].start))) return k_rows[i].name;
    return NULL;
}

#define IV 96
typedef struct { char s[IM_TEXT]; DWORD first, last; int spoken; int slot; int para; int kind; } IVis;
static IVis g_iv[IV];
/* Spots on screen (rounded height): how many new texts lately, quiet until. */
typedef struct { int key; int count; DWORD since, quiet_until; } Slot;
static Slot g_slots[32];
static int g_im_mute;
static char g_last_row[64];
static DWORD g_row_seen;           /* when a row description was last on screen */
static char g_sel[IM_TEXT];        /* highlighted menu row last spoken, as drawn */
#define NSEL 16
static struct { char s[IM_TEXT]; void *list; float x, y; DWORD seen; } g_sels[NSEL];   /* highlighted rows on screen */
static DWORD g_sel_said;

/* Is the cursor on one of the port's rows (its description is up)? */
int im_row_on(void) { return g_ready && GetTickCount() - g_row_seen < 100; }
/* Was a port row or screen up in the last ms milliseconds? (screens fade between rows) */
int im_port_recent(DWORD ms);
/* Is one of the port's own menus up (a highlighted row on screen)? */
static DWORD g_sel_seen;
int im_menu_on(void) { return g_ready && GetTickCount() - g_sel_seen < 150; }
int im_port_recent(DWORD ms) {
    DWORD now = GetTickCount();
    return g_ready && (now - g_sel_seen < ms || now - g_row_seen < ms);
}

void im_mute(int on) { g_im_mute = on; }
const char *im_row(void) { return g_last_row; }

static int slot_of(const Line *p) {
    int key = (int)(p->y / 8) * 64 + (int)p->size;
    DWORD now = GetTickCount();
    int freei = 0;
    for (int i = 0; i < 32; i++) {
        if (g_slots[i].key == key && g_slots[i].since) return i;
        if (g_slots[i].since < g_slots[freei].since) freei = i;
    }
    g_slots[freei].key = key; g_slots[freei].count = 0; g_slots[freei].since = now; g_slots[freei].quiet_until = 0;
    return freei;
}

int im_any_text(void) {
    DWORD now = GetTickCount();
    for (int i = 0; i < IV; i++) if (g_iv[i].s[0] && now - g_iv[i].last < 300) return 1;
    return 0;
}

/* A line visible now that starts with this prefix, or NULL. */
const char *im_find(const char *prefix) {
    DWORD now = GetTickCount();
    size_t n = strlen(prefix);
    for (int i = 0; i < IV; i++) if (g_iv[i].s[0] && now - g_iv[i].last < 300 && !strncmp(g_iv[i].s, prefix, n)) return g_iv[i].s;
    return NULL;
}

static void say_para(const char *text) {
    const char *row = row_name(text);
    char buf[2048];
    if (row && _stricmp(row, g_sel)) {                /* not when the row's own label was just read (ONLINE) */
        snprintf(g_last_row, sizeof g_last_row, "%s", row);
        snprintf(buf, sizeof buf, "%s. %s", row, text);
    } else snprintf(buf, sizeof buf, "%s", text);
    for (char *c = buf; *c; c++) if (*c == '\n') *c = ' ';
    log_line("imgui say: %s", buf);
    speak_utf8(buf, 0);
}

void im_poll(void) {
    static ImText f[IM_MAX];
    static Line l[IM_MAX];
    static LONG seen_frame;
    if (!g_ready) return;
    DWORD now = GetTickCount();
    LONG fno = 0;
    int n = im_frame(f, IM_MAX, &fno);
    if (fno != seen_frame) {
        seen_frame = fno;
        int nl = build_lines(f, n, l, IM_MAX);
        for (int k = 0; k < nl; k++) if (row_name(l[k].s)) g_row_seen = now;
        /* Highlighted rows (a screen can have more than one: Tag Team's team slot and form
         * picker): say each one that changes, with its place among the rows of its list. */
        int nsel = 0;
        for (int k = 0; k < nl; k++) {
            if (l[k].kind != K_ROW_SEL) continue;
            nsel++;
            g_sel_seen = now;
            int known = 0;
            for (int i = 0; i < NSEL && !known; i++) known = g_sels[i].s[0] && !strcmp(g_sels[i].s, l[k].s);
            for (int i = 0; i < NSEL; i++) if (g_sels[i].s[0] && !strcmp(g_sels[i].s, l[k].s)) g_sels[i].seen = now;
            if (known) continue;
            /* forget what was at the same spot (text is centred, so only height and side count), take the oldest entry */
            int slot = 0;
            for (int i = 0; i < NSEL; i++) {
                if (g_sels[i].s[0] && g_sels[i].list == l[k].list && (g_sels[i].x < 640) == (l[k].x < 640) &&
                    g_sels[i].y > l[k].y - l[k].size * 0.5f && g_sels[i].y < l[k].y + l[k].size * 0.5f) g_sels[i].s[0] = 0;
                if (!g_sels[i].s[0] ? g_sels[slot].s[0] != 0 : g_sels[i].seen < g_sels[slot].seen && g_sels[slot].s[0]) slot = i;
            }
            snprintf(g_sels[slot].s, sizeof g_sels[slot].s, "%s", l[k].s);
            g_sels[slot].list = l[k].list; g_sels[slot].x = l[k].x; g_sels[slot].y = l[k].y; g_sels[slot].seen = now;
            snprintf(g_sel, sizeof g_sel, "%s", l[k].s);
            g_sel_said = now;
            int nrows = 1, pos = 0, others = 0;
            for (int j = 0; j < nl; j++)
                if (j != k && (l[j].kind == K_ROW || l[j].kind == K_ROW_OFF) && l[j].list == l[k].list && l[j].size == l[k].size) {
                    nrows++; others++;
                    if (l[j].y < l[k].y - 1) pos++;
                }
            char say[IM_TEXT];
            snprintf(say, sizeof say, "%s", l[k].s);
            char *dash = strstr(say, "- - -");             /* ONLINE's empty room code */
            if (dash) snprintf(dash, sizeof say - (size_t)(dash - say), "empty");
            if (g_im_mute) continue;
            const char *off = l[k].off ? ", unavailable" : "";
            if (others) speakf(nsel == 1, "%s%s, %d of %d", say, off, pos + 1, nrows);
            else speakf(nsel == 1, "%s%s", say, off);
            log_line("imgui row: %s (%d of %d)", l[k].s, pos + 1, nrows);
        }
        for (int i = 0; i < NSEL; i++)        /* a selector that hides while the game scrolls stays quiet */
            if (g_sels[i].s[0] && now - g_sels[i].seen > 2000) g_sels[i].s[0] = 0;
        for (int k = 0; k < nl; k++) {
            int found = 0, freei = -1;
            for (int i = 0; i < IV; i++) {
                if (g_iv[i].s[0] && !strcmp(g_iv[i].s, l[k].s)) { g_iv[i].last = now; g_iv[i].para = l[k].para; found = 1; break; }
                if (freei < 0 && !g_iv[i].s[0]) freei = i;
            }
            if (found || freei < 0) continue;
            IVis *v = &g_iv[freei];
            snprintf(v->s, sizeof v->s, "%.*s", IM_TEXT - 1, l[k].s);
            v->first = v->last = now; v->slot = slot_of(&l[k]); v->para = l[k].para; v->kind = l[k].kind;
            v->spoken = v->kind != K_TEXT;                /* rows: only the highlighted one is read, above */
        }
    }
    /* Lines ready to speak, in screen order (paragraph number), joined per paragraph. */
    int ready[IV], nr = 0;
    for (int i = 0; i < IV; i++) {
        IVis *v = &g_iv[i];
        if (!v->s[0]) continue;
        if (now - v->last > 400) { v->s[0] = 0; continue; }
        if (v->spoken || now - v->first < 120) continue;
        v->spoken = 1;
        Slot *sl = &g_slots[v->slot];
        if (now - sl->since > 3000) { sl->since = now; sl->count = 0; }
        if (++sl->count > 4) sl->quiet_until = now + 5000;
        if (now < sl->quiet_until || g_im_mute) continue;
        ready[nr++] = i;
    }
    for (int a = 1; a < nr; a++)                      /* insertion sort by paragraph */
        for (int b = a; b > 0 && g_iv[ready[b]].para < g_iv[ready[b - 1]].para; b--) { int t = ready[b]; ready[b] = ready[b - 1]; ready[b - 1] = t; }
    char buf[2048];
    for (int a = 0; a < nr;) {
        int para = g_iv[ready[a]].para;
        buf[0] = 0;
        for (; a < nr && g_iv[ready[a]].para == para; a++) {
            if (tail_quiet(g_iv[ready[a]].s)) continue;
            if (buf[0] && strlen(buf) + 1 < sizeof buf) strcat(buf, " ");
            strncat(buf, g_iv[ready[a]].s, sizeof buf - strlen(buf) - 1);
        }
        if (buf[0]) say_para(buf);
    }
}

/* F2: everything of the port's screens that is up now, in screen order. Returns how many lines. */
int im_read_all(int interrupt) {
    DWORD now = GetTickCount();
    int idx[IV], n = 0;
    for (int i = 0; i < IV; i++) if (g_iv[i].s[0] && now - g_iv[i].last < 400) idx[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && g_iv[idx[b]].para < g_iv[idx[b - 1]].para; b--) { int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t; }
    for (int a = 0; a < n; a++) speak_utf8(g_iv[idx[a]].s, interrupt && !a);
    return n;
}
