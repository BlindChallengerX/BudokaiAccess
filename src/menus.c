/*
 * Menus whose items are drawn as one picture, so the text reader cannot tell
 * which item is highlighted. Each reader finds its menu's cursor and speaks
 * the item when the cursor moves.
 *
 * Simple menus: a task that exists while the menu is open, and the cursor
 * in that task's work data (or behind a fixed pointer). Lists with item IDs
 * (the skill shop) have their own reader below.
 */
#include "access.h"
#include <stdio.h>
#include <string.h>

/* Capsule (skill) names: pack 69475272 of the text table holds every capsule
 * name with the capsule ID as its texture index. */
#define CAPSULE_NAMES 0x69475272u
const char *capsule_name(int id) {
    const char *t = text_lookup(CAPSULE_NAMES, (uint32_t)id);
    if (!t) t = roster_capsule_name(id);         /* capsules of characters added by mods */
    return t ? t : "unknown skill";
}

typedef struct {
    const char *task;        /* task that exists while the menu is open */
    const char *not_task;    /* menu is hidden while this task runs (submenu), or NULL */
    uint32_t ptr;            /* if set: fixed address holding a pointer to the menu object */
    int cursor_off;          /* offset of the cursor in the task work data / menu object */
    int size;                /* 1 = byte, 4 = 32-bit */
    int count;
    const char *items[12];
    int last;                /* last cursor spoken, -1 when closed */
    int extra;               /* rows the port adds below the game's own (port 1.5: HUB, ONLINE), read by imgui.c */
    int on_extra;            /* the cursor is on one of them */
    LONG moves;              /* g_pad_moves while on it */
    DWORD wait0;             /* port 1.5.5: cursor 0 is the first row or a port row, wait for its description */
    int closed_on_extra;     /* the menu closed from a port row (HUB, ONLINE): it comes back under that screen */
} Menu;

static Menu k_menus[] = {
    /* Main menu: the mode under the cursor (Goku's comment on each is muted, see menus_poll).
     * Dragon Arena is the 7th item once unlocked. */
    { "# MAIN MENU #", NULL, 0, 0x96, 1, 7, { "Dragon Universe", "Dueling", "World Tournament", "Practice", "Skill Editing", "Options", "Dragon Arena" }, -1, 1 },
    /* Character select, after choosing a fighter. */
    { "# CHARASEL #", NULL, 0, 0, 1, 3, { "Normal", "Custom", "Edit Skills" }, -1 },
    /* Dueling: who plays. */
    { "# VS MODE #", "VS CHARASEL", 0, 0x53, 1, 4, { "1 P versus computer", "1 P versus 2 P", "Computer versus 2 P", "Computer versus computer" }, -1, 1 },
    /* Dragon Arena: who fights. */
    { "# ARENA MODE #", "# ARENA CHARASE", 0, 0x35, 1, 3, { "1 P versus computer", "1 P versus 2 P", "Computer versus computer" }, -1 },
    /* Dragon Universe title menu. */
    { "# STR_TITLE #", "# DRAW2 #", 0x8247a8d0, 0xb0, 4, 4, { "New Story", "Continue", "Reset Character", "Bonus" }, -1 },
    /* Edit Skills main menu (main menu, Edit Skills). */
    { "ESS MAIN", "ESS SHOP", 0x823ee7dc, 0x54, 4, 4, { "Edit Capsules", "Skill Shop", "Skill List", "Help" }, -1 },
};
#define NMENUS (int)(sizeof k_menus / sizeof k_menus[0])

static uint32_t menu_base(const Menu *m) {
    if (m->ptr) return task_running(m->task) ? g32(m->ptr) : 0;
    return task_work(m->task);
}

#define LIVE_G_EARLY 0x824ba110u   /* same as LIVE_G below */
/* The game's own rows. Main menu: Dragon Arena only once its ticket (capsule 516)
 * is unlocked (unlock flags at G+0x2afe9 + capsule, as for the Bonus items). */
static int menu_rows(const Menu *m) {
    if (!strcmp(m->task, "# MAIN MENU #") && !g8(LIVE_G_EARLY + 0x2afe9 + 516)) return 6;
    return m->count;
}

static void say_item(const Menu *m, int c, int interrupt) {
    int n = c < menu_rows(m) ? menu_rows(m) : m->count;
    if (im_ready()) n += m->extra;
    speakf(interrupt, "%s, %d of %d", m->items[c], c + 1, n);
}

/* ---------- skill shop ----------
 * Pointer at 823ee7e4 while the shop is open. Object:
 *   +0x60 tab (0 all, 1..4 capsule colours)   +0x64 scroll   +0x68 row on screen (0..3)
 *   +0x6c number of records   +0x78 records of 0x14 bytes:
 *        +0 flag (2 = new)  +4 capsule ID  +0xc price  +0x10 category (high 16 bits, tab - 1)
 * A tab other than 0 shows only the records of its category. */
#define SHOP_PTR 0x823ee7e4u
/* Your Zenie (the same wallet as in Dragon Universe, du.c MONEY_TOTAL). */
#define SHOP_MONEY 0x824a5898u
static int g_shop_money = -1;
static int shop_money(void) { int m = (int)g32(SHOP_MONEY); return m >= 0 && m <= 99999999 ? m : -1; }
int shop_open(void) { return task_running("ESS SHOP") && g32(SHOP_PTR) != 0; }
/* F8 in the shop: your money. */
void shop_say_money(void) {
    int m = shop_money();
    if (m >= 0) speakf(1, "You have %d zenie", m);
}
static const char *k_shop_tabs[] = { "All skills", "Ability capsules", "Equipment capsules", "Item capsules", "System capsules" };
static int g_shop_tab = -1, g_shop_pos = -1;

static int shop_item(uint32_t o, int tab, int pos, int *id, int *price, int *total) {
    int n = (int)g32(o + 0x6c), k = 0, found = 0;
    if (n < 0 || n > 600) return 0;
    for (int i = 0; i < n; i++) {
        uint32_t r = o + 0x78 + 0x14 * (uint32_t)i;
        int cat = (int16_t)g16(r + 0x10);
        if (tab != 0 && cat != tab - 1) continue;
        if (k == pos) { *id = (int)g32(r + 4); *price = (int)g32(r + 0xc); found = 1; }
        k++;
    }
    *total = k;
    return found;
}

static void shop_poll(int force) {
    uint32_t o = task_running("ESS SHOP") ? g32(SHOP_PTR) : 0;
    if (!o || !guest_ok(o + 0x60, 0x20)) { g_shop_tab = g_shop_pos = -1; g_shop_money = -1; return; }
    int tab = (int)g32(o + 0x60), pos = (int)g32(o + 0x64) + (int)g32(o + 0x68);
    if (tab < 0 || tab > 4) return;
    /* money: said when the shop opens and after each purchase */
    int money = shop_money();
    if (money >= 0 && money != g_shop_money) {
        if (g_shop_money >= 0) speakf(0, "You now have %d zenie", money);
        g_shop_money = money;
    }
    int opening = g_shop_tab < 0;
    if (!force && tab == g_shop_tab && pos == g_shop_pos) return;
    char buf[300] = "";
    if ((opening || force) && money >= 0) snprintf(buf, sizeof buf, "You have %d zenie. ", money);
    if (tab != g_shop_tab || force) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s. ", k_shop_tabs[tab]);
    int id = 0, price = 0, total = 0;
    if (shop_item(o, tab, pos, &id, &price, &total))
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s, %d zenie, %d of %d", capsule_name(id), price, pos + 1, total);
    else if (total == 0) strncat(buf, "Nothing for sale here", sizeof buf - strlen(buf) - 1);
    speak_utf8(buf, 1);
    g_shop_tab = tab; g_shop_pos = pos;
}

/* ---------- Practice pause menu ----------
 * Task "[FIX]PSDRW" work +9 = highlighted row, +0xb = Yes/No in the exit
 * question. Option values live in the training settings. */
#define PAUSE_TEXTS 0x4e500b05u     /* pack with the pause menu's words */
typedef struct { const char *name; uint32_t addr; const char *vals[5]; } PauseRow;
static const PauseRow k_practice_pause[] = {
    { "Continue", 0, { 0 } },
    { "COM action", 0x823d6b97, { "Do nothing", "Guard 1", "Guard 2", "Match", "Player Controls" } },
    { "COM level", 0x823d6b98, { "Very weak", "Weak", "Average", "Strong", "Very strong" } },
    { "Defend against ki blast waves", 0x823d6b99, { "Do nothing", "Deflect", "Deflect back" } },
    { "Counterattack", 0x823d6b9a, { "Off", "On" } },
    { "Break-fall", 0x823d6b9b, { "Off", "On" } },
    { "Display", 0x823cd7b3, { "Status and Command", "Status", "Command", "No display" } },
    { "Reset Position", 0, { 0 } },
    { "Skill List", 0, { 0 } },
    { "Reselect Characters", 0, { 0 } },
    { "Return to Main Menu", 0, { 0 } },
};
#define NPAUSE (int)(sizeof k_practice_pause / sizeof k_practice_pause[0])
static int g_pause_row = -1, g_pause_val = -1, g_pause_yn = -1;

static const char *pause_value(int row) {
    const PauseRow *r = &k_practice_pause[row];
    if (!r->addr) return NULL;
    int v = g8(r->addr);
    return v >= 0 && v < 5 ? r->vals[v] : NULL;
}

static void pause_say(int row, int interrupt) {
    const char *v = pause_value(row);
    if (v) speakf(interrupt, "%s: %s, %d of %d", k_practice_pause[row].name, v, row + 1, NPAUSE);
    else speakf(interrupt, "%s, %d of %d", k_practice_pause[row].name, row + 1, NPAUSE);
}

/* Other pause-style menus, told apart by which labels are on screen
 * (texture numbers in the pause pack). */
static const struct { int labels[6]; } k_pause_layouts[] = {
    { { 6, 8, 10, 52, -1 } },     /* fight pause: Continue Battle, Refer to Skills, Reselect Characters, Return to Main Menu */
    { { 7, 10, 52, -1 } },        /* after a match: Battle Again?, Reselect Characters, Return to Main Menu */
    { { 6, 8, -1 } },             /* Dragon Universe battle pause: Continue Battle, Refer to Skills */
    { { 6, 8, 53, 43, 52, -1 } }, /* Training lesson: ..., Review instructions, Return to Training Menu, Return to Main Menu */
};
static int g_layout = -1, g_lay_row = -1;

static int layout_on_screen(int l) {
    for (int i = 0; k_pause_layouts[l].labels[i] >= 0; i++)
        if (!text_visible(PAUSE_TEXTS, (uint32_t)k_pause_layouts[l].labels[i])) return 0;
    return 1;
}

static int layout_len(int l) { int n = 0; while (k_pause_layouts[l].labels[n] >= 0) n++; return n; }

/* The layout with the most of its labels on screen wins: a short menu's
 * labels are also part of the longer ones. */
static void layout_poll(uint32_t w) {
    int best = -1;
    for (int l = 0; l < (int)(sizeof k_pause_layouts / sizeof k_pause_layouts[0]); l++)
        if (layout_on_screen(l) && (best < 0 || layout_len(l) > layout_len(best))) best = l;
    if (best != g_layout) { g_layout = best; g_lay_row = -1; }
    if (g_layout < 0) return;
    int n = layout_len(g_layout);
    int row = g8(w + 9);
    if (row < 0 || row >= n || row == g_lay_row) return;
    const char *t = text_lookup(PAUSE_TEXTS, (uint32_t)k_pause_layouts[g_layout].labels[row]);
    speakf(g_lay_row >= 0, "%s, %d of %d", t ? t : "item", row + 1, n);
    g_lay_row = row;
}

/* Skill list opened from the pause menu: rows captured by text.c, the
 * highlighted row at pause data +0xb. */
static int g_skill_row = -1;
static int skill_list_poll(uint32_t w) {
    int n = skill_rows();
    if (!n) { g_skill_row = -1; return 0; }
    int row = g8(w + 0xb);
    if (row != g_skill_row && row >= 0 && row < n) {
        char buf[400];
        if (skill_row_text(row, buf, sizeof buf - 20)) {
            snprintf(buf + strlen(buf), 20, ". %d of %d", row + 1, n);
            speak_utf8(buf, 1);
        }
        g_skill_row = row;
    }
    return 1;
}

static void pause_poll(void) {
    uint32_t w = task_work("[FIX]PSDRW");
    if (w && skill_list_poll(w)) { g_layout = -1; g_lay_row = -1; g_pause_row = -1; return; }
    int practice = task_running("[FIX]TRAIN DRAW");
    if (!w) { g_pause_row = -1; g_layout = -1; g_lay_row = -1; return; }
    if (!practice) {
        g_pause_row = -1;
        if (!(text_visible(PAUSE_TEXTS, 13) && text_visible(PAUSE_TEXTS, 14))) layout_poll(w);
    }
    /* Yes (13) / No (14) question, answer at +0xb. The title is its own
     * picture: Exit? (3), Continue? (5, after losing a story battle; its
     * answer is the row at +9) or Retry? (45). */
    if (text_visible(PAUSE_TEXTS, 13) && text_visible(PAUSE_TEXTS, 14)) {
        /* Wait until the answer has held for a moment: cancelling flips it
         * just before the question closes. */
        static int pend = -1; static DWORD pend_t;
        int cont = text_visible(PAUSE_TEXTS, 5);
        int yn = g8(w + (cont ? 9 : 0xb));
        if (yn != pend) { pend = yn; pend_t = GetTickCount(); }
        if (yn != g_pause_yn && GetTickCount() - pend_t > 120) {
            const char *title = cont ? "Continue?" : text_visible(PAUSE_TEXTS, 45) ? "Retry?"
                              : text_visible(PAUSE_TEXTS, 3) ? "Exit?" : "";
            speakf(1, g_pause_yn < 0 ? "%s %s" : "%s%s", g_pause_yn < 0 ? title : "", yn == 0 ? "Yes" : "No");
            g_pause_yn = yn;
        }
        return;
    }
    if (g_pause_yn >= 0) { g_pause_yn = -1; g_pause_row = -1; g_lay_row = -1; }
    if (!practice) return;
    int row = g8(w + 9);
    if (row < 0 || row >= NPAUSE) return;
    const PauseRow *r = &k_practice_pause[row];
    int val = r->addr ? g8(r->addr) : -1;
    if (row != g_pause_row) pause_say(row, g_pause_row >= 0);
    else if (val != g_pause_val && pause_value(row)) speak_utf8(pause_value(row), 1);
    g_pause_row = row; g_pause_val = val;
}

/* ---------- stage select ----------
 * Task "STAGE SELECT", work data: +0x244 the stages you can pick (16-bit
 * IDs, -1 after the last), +0x25d the stage under the cursor, +0x260 its
 * position in that list, +0x261 how many there are. Stage ID 0..11 is the
 * stage capsule 517 + ID (its name is in the capsule names); 12 is random.
 * Port 1.5 adds stages (Budokai 1/2 and Infinite World ones made from the
 * player's discs): ID 13 + k is the k-th [[escenario]] of the enabled mods'
 * stage.toml files (mods in folder order, ones with selector = false left
 * out, and the "Campo IW" folders: Infinite World minigame fields), named
 * by its titulo (or nombre). */
#define STAGE_CAPSULE0 517
static int g_stage_last = -1;
static char g_new_stages[48][64];
static int g_nnew_stages = -1;

static void stage_value(char *line, char *out, size_t cap) {
    char *v = strchr(line, '=');
    if (!v) { out[0] = 0; return; }
    v++;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '"') { v++; char *e = strchr(v, '"'); if (e) *e = 0; }
    else { char *e = strpbrk(v, "#\r\n"); if (e) *e = 0; }
    snprintf(out, cap, "%s", v);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
}

/* English names for the stages the port makes from the player's PS2 discs
 * (their stage.toml titles are Spanish). */
static const struct { const char *es, *en; } k_stage_en[] = {
    { "Templo (B2)", "Kami's Lookout (Budokai 2)" },
    { "Muscle Tower (B2)", "Muscle Tower (Budokai 2)" },
    { "Base de la Red Ribbon (B2)", "Red Ribbon Base (Budokai 2)" },
    { "Zona urbana (B2)", "Urban Area (Budokai 2)" },
    { "Planeta Namek (B2)", "Planet Namek (Budokai 2)" },
    { "Llanura (B2)", "Plains (Budokai 2)" },
    { "Mundo de Kaiyoshin (B2)", "Supreme Kai's World (Budokai 2)" },
    { "Nave de Goku por dentro (IW)", "Inside Goku's Ship (Infinite World)" },
    { "Tierra en vuelo (IW)", "Flying over Earth (Infinite World)" },
    { "Namek en vuelo (IW)", "Flying over Namek (Infinite World)" },
    { "Camino de la Serpiente (IW)", "Snake Way (Infinite World)" },
    { "Planeta del Kaio (IW)", "King Kai's Planet (Infinite World)" },
    { "Namek que explota (IW)", "Exploding Namek (Infinite World)" },
    { "Llanura de Raditz (IW)", "Raditz's Plain (Infinite World)" },
    { "Llanura de Piccolo (IW)", "Piccolo's Plain (Infinite World)" },
    { "Roquedal (IW)", "Rocky Field (Infinite World)" },
    { "Páramo al atardecer", "Wasteland at Sunset (Budokai 1)" },
    { "Ring de Cell (B1)", "Cell Ring (Budokai 1)" },
};

static void load_new_stages(void) {
    g_nnew_stages = 0;
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%lsmods\\*", g_game_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);           /* NTFS lists folders in name order, like the port */
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (!wcsncmp(fd.cFileName, L"Campo IW", 8)) continue;    /* Infinite World minigame fields: the port keeps them off the list */
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%lsmods\\%ls\\.disabled", g_game_dir, fd.cFileName);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
        swprintf(path, MAX_PATH, L"%lsmods\\%ls\\stage.toml", g_game_dir, fd.cFileName);
        FILE *f = _wfopen(path, L"r");
        if (!f) continue;
        char line[512], name[64] = "", title[64] = "";
        int in = 0, shown = 1;
        for (;;) {
            int eof = !fgets(line, sizeof line, f);
            char *t = line; while (*t == ' ' || *t == '\t') t++;
            if (eof || *t == '[') {
                if (in && shown && g_nnew_stages < 48) {
                    const char *t = title[0] ? title : name;
                    for (size_t i = 0; i < sizeof k_stage_en / sizeof k_stage_en[0]; i++)
                        if (!strcmp(t, k_stage_en[i].es)) t = k_stage_en[i].en;
                    snprintf(g_new_stages[g_nnew_stages++], 64, "%s", t);
                }
                in = !eof && (!strncmp(t, "[[escenario]]", 13) || !strncmp(t, "[escenario]", 11));
                name[0] = title[0] = 0; shown = 1;
                if (eof) break;
                continue;
            }
            if (!in) continue;
            char v[64];
            if (!strncmp(t, "titulo", 6)) stage_value(t, title, sizeof title);
            else if (!strncmp(t, "nombre", 6)) stage_value(t, name, sizeof name);
            else if (!strncmp(t, "selector", 8)) { stage_value(t, v, sizeof v); shown = strcmp(v, "false") && strcmp(v, "no") && strcmp(v, "0"); }
        }
        fclose(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    log_line("stages added by mods: %d", g_nnew_stages);
}

static int stage_say(int interrupt) {
    uint32_t w = task_work("STAGE SELECT");
    if (!w || !guest_ok(w + 0x244, 0x20)) return 0;
    int id = g8(w + 0x25d), pos = g8(w + 0x260), n = g8(w + 0x261);
    if (n < 1 || n > 64 || pos >= n) return 0;
    if (g_nnew_stages < 0) load_new_stages();
    const char *name = id == 12 ? "Random stage" : id < 12 ? capsule_name(STAGE_CAPSULE0 + id)
                     : id - 13 < g_nnew_stages && g_new_stages[id - 13][0] ? g_new_stages[id - 13] : "Stage";
    speakf(interrupt, "%s, %d of %d", name, pos + 1, n);
    return 1;
}

static void stage_poll(void) {
    uint32_t w = task_work("STAGE SELECT");
    if (!w || !guest_ok(w + 0x244, 0x20)) { g_stage_last = -1; return; }
    int id = g8(w + 0x25d);
    if (id != g_stage_last) stage_say(g_stage_last >= 0);
    g_stage_last = id;
}

/* ---------- Dragon Universe Bonus screen ----------
 * Dragon Universe title menu, Bonus: one item at a time, left and right to
 * change (game code 821d1488). "# DRAW2 #" runs while it is open. Title
 * object (pointer 8247a8d0) +0xb4 = item 0..40. Item table of 8-byte
 * records, at 823752b8 when the byte at G+0x2c0c9 is set, else 82375408:
 * +0 capsule ID (the "Memories of ..." capsules 533..572), -1 for the last
 * item. A capsule item is unlocked when the byte at G+0x2afe9+ID is set;
 * the last item when the word at G+0x2afbc is not 0. Locked items show ???. */
#define DU_TITLE_PTR 0x8247a8d0u
#define LIVE_G 0x824ba110u
#define BONUS_COUNT 41
static int g_bonus_last = -1;

static int bonus_say(int interrupt) {
    uint32_t o = task_running("# STR_TITLE #") && task_running("# DRAW2 #") ? g32(DU_TITLE_PTR) : 0;
    if (!o || !guest_ok(o + 0xb4, 4)) return 0;
    int i = (int)g32(o + 0xb4);
    if (i < 0 || i >= BONUS_COUNT) return 0;
    uint32_t table = g8(LIVE_G + 0x2c0c9) ? 0x823752b8u : 0x82375408u;
    int id = (int16_t)g16(table + 8 * (uint32_t)i);
    char buf[200];
    if (id < 0) {
        if (g32(LIVE_G + 0x2afbc)) snprintf(buf, sizeof buf, "Special bonus, %d of %d", i + 1, BONUS_COUNT);
        else snprintf(buf, sizeof buf, "Locked, %d of %d", i + 1, BONUS_COUNT);
    } else if (g8(LIVE_G + 0x2afe9 + (uint32_t)id)) {
        snprintf(buf, sizeof buf, "%s, %d of %d", capsule_name(id), i + 1, BONUS_COUNT);
    } else {
        snprintf(buf, sizeof buf, "Locked, %d of %d", i + 1, BONUS_COUNT);
    }
    speak_utf8(buf, interrupt);
    g_bonus_last = i;
    return 1;
}

static void bonus_poll(void) {
    uint32_t o = task_running("# STR_TITLE #") && task_running("# DRAW2 #") ? g32(DU_TITLE_PTR) : 0;
    if (!o || !guest_ok(o + 0xb4, 4)) { g_bonus_last = -1; return; }
    int i = (int)g32(o + 0xb4);
    if (i == g_bonus_last) return;
    if (g_bonus_last < 0) speak(L"Bonus", 1);
    bonus_say(0);
}

/* ---------- Dragon Universe character select ----------
 * Task "# STR SELECT #" with "CHARASEL": CHARASEL work +4 = character ID
 * under the cursor (Goku 0, Kid Gohan 2, Krillin 10, Piccolo 11 ...). The
 * name comes from the Bonus item table, which lists the "Memories of <name>"
 * capsule of each character in ID order (see bonus_index); the text reader is muted here so
 * the labels and numbers are not read on top of it. STR SELECT work: +0x5c seven stat
 * levels (Health, Ki, Attack, Guard, Arts, Ability, Com, one byte each),
 * +0x63 level, +0x64 experience, +0x68 experience to the next level. */
static int g_strsel_last = -1;

/* Character number -> position in the Bonus list: the same number. The
 * select wheel runs 0..37 in that order (Frieza 22, Androids 16..18 23..25,
 * Dr. Gero 26, Cell 27 ... Cooler 32, Bardock 33, Broly 34, Omega Shenron 35,
 * Saibamen 36, Cell Jr. 37; 38 is Random), checked on a save with everyone
 * unlocked. (1.0.4 and earlier skipped 22..26 after the new-characters pack's
 * numbering, which shifted every name from Frieza on.) */
#define CHAR_COUNT 38
static int bonus_index(int id) {
    return id >= 0 && id < CHAR_COUNT ? id : -1;
}

static const char *char_name(int id, char *buf, size_t cap) {
    id = bonus_index(id);
    if (id < 0 || id >= BONUS_COUNT - 1) return NULL;
    uint32_t table = g8(LIVE_G + 0x2c0c9) ? 0x823752b8u : 0x82375408u;
    int cap_id = (int16_t)g16(table + 8 * (uint32_t)id);
    const char *t = cap_id > 0 ? text_lookup(CAPSULE_NAMES, (uint32_t)cap_id) : NULL;
    if (!t || strncmp(t, "Memories of ", 12)) return NULL;
    t += 12;
    /* say them as the character select's own name pictures do */
    if (!strcmp(t, "Gt. Saiyaman")) t = "Great Saiyaman";
    if (!strncmp(t, "Android #", 9)) { snprintf(buf, cap, "Android %s", t + 9); return buf; }
    snprintf(buf, cap, "%s", t);
    return buf;
}

/* Dragon Arena uses the same select with its own data: one of the
 * "# ARENA CHARASE" tasks has work with the stats at +0x16, level +0x1d,
 * experience +0x28 and to next level +0x2c. */
typedef struct { uint32_t w; int stats, level, exp, next; } StrSel;
static int strsel_src(StrSel *s) {
    if (task_running("# STR SELECT #")) {
        *s = (StrSel){ task_work("# STR SELECT #"), 0x5c, 0x63, 0x64, 0x68 };
        return s->w && guest_ok(s->w + 0x5c, 0x10);
    }
    uint32_t w[4];
    int n = task_works("# ARENA CHARASE", w, 4);
    for (int i = 0; i < n; i++)
        if (w[i] && guest_ok(w[i] + 0x10, 0x20)) { *s = (StrSel){ w[i], 0x16, 0x1d, 0x28, 0x2c }; return 1; }
    return 0;
}

static int strsel_say(int interrupt, int details) {
    StrSel s;
    uint32_t cw = task_work("CHARASEL");
    if (!strsel_src(&s) || !cw || !guest_ok(cw + 4, 1)) return 0;
    int id = g8(cw + 4);
    char name[64], buf[300];
    if (!char_name(id, name, sizeof name)) snprintf(name, sizeof name, "Character %d", id);
    int n = snprintf(buf, sizeof buf, "%s, level %d", name, g8(s.w + s.level));
    if (details) {
        static const char *k_stats[] = { "Health", "Ki", "Attack", "Guard", "Arts", "Ability", "Com" };
        n += snprintf(buf + n, sizeof buf - n, ". Experience %d, %d to next level.",
                      (int)g32(s.w + s.exp), (int)g32(s.w + s.next));
        for (int i = 0; i < 7; i++)
            n += snprintf(buf + n, sizeof buf - n, " %s %d%s", k_stats[i], g8(s.w + s.stats + (uint32_t)i), i < 6 ? "," : ".");
    }
    speak_utf8(buf, interrupt);
    g_strsel_last = id;
    return 1;
}

/* The stats follow the cursor a few frames later, so the new character is
 * spoken once the cursor has rested 150 ms. */
static void strsel_poll(void) {
    static int seen = -1; static DWORD since;
    uint32_t cw = task_running("# STR SELECT #") || task_running("# ARENA CHARASE") ? task_work("CHARASEL") : 0;
    if (!cw || !guest_ok(cw + 4, 1)) { g_strsel_last = seen = -1; return; }
    int id = g8(cw + 4);
    if (id != seen) { seen = id; since = GetTickCount(); }
    if (id != g_strsel_last && GetTickCount() - since >= 150) strsel_say(0, 0);
}

/* ---------- save and storage questions ----------
 * System messages ask with one "Yes No" picture; 0 is Yes, 1 No. The text
 * reader says the question.
 *  - save / storage messages (packs ebdb339d, 236e7248, picture 33): the
 *    byte at 8243ab0b (No is highlighted first);
 *  - quit messages such as "End your game?" (pack fb8b5c70, picture 3):
 *    a message box block in the system work memory, no pointer to it found:
 *    4012e9a8 its picture pack, 4012e9cc the answer (32 bit). The pack is
 *    checked first, so a different layout just means no answer is read. */
#define SYSQ_CURSOR 0x8243ab0bu
#define QUITQ_PACK  0x4012e9a8u
#define QUITQ_ANS   0x4012e9ccu
static int g_sysq = -1;

int sysq_open(void) {
    return text_visible(0xebdb339du, 33) || text_visible(0x236e7248u, 33) || text_visible(0xfb8b5c70u, 3);
}

static int sysq_cursor(void) {
    if (text_visible(0xfb8b5c70u, 3)) {
        uint32_t pk = guest_ok(QUITQ_PACK, 0x28) ? g32(QUITQ_PACK) : 0;
        if (!pk || !guest_ok(pk, 0x20) || g32(pk + 0x1c) != 0xfb8b5c70u) return -1;
        return (int)g32(QUITQ_ANS);
    }
    return g8(SYSQ_CURSOR);
}

static int sysq_say(int interrupt) {
    if (!sysq_open()) return 0;
    int c = sysq_cursor();
    if (c < 0) return 0;
    speak(c == 0 ? L"Yes" : L"No", interrupt);
    g_sysq = c;
    return 1;
}

/* The first answer waits a moment so the text reader says the question first. */
static void sysq_poll(void) {
    static DWORD since;
    if (!sysq_open()) { g_sysq = -1; since = 0; return; }
    if (!since) since = GetTickCount();
    int c = sysq_cursor();
    if (c < 0) return;
    if (g_sysq < 0) {
        if (GetTickCount() - since < 400) return;
        speak(c == 0 ? L"Selected: Yes" : L"Selected: No", 0); g_sysq = c;
    }
    else if (c != g_sysq) sysq_say(1);
}

/* ---------- Dueling / World Tournament character select ----------
 * One "CHARASEL" task per player picking (without "# STR SELECT #", which
 * the Dragon Universe reader handles); work +4 = character under the cursor,
 * same numbers as there. The name pictures (pack e8989c42) are only spoken
 * when they newly appear, so scrolling back to a name still on screen (the
 * other player's) said nothing: the names now come from the cursor. Picture
 * 63 "Random or locked character" replaces the name when it is shown.
 * Names come from char_name (character number -> Bonus list); if a number
 * is not in it, the name picture on screen that isn't the other player's. */
#define CS_NAMES 0xe8989c42u
#define CS_NAME_FIRST 26          /* name pictures 26..64 (63 is "Random or locked") */
#define CS_NAME_LAST  64
static int g_cs_last[2] = { -1, -1 }, g_cs_seen[2] = { -1, -1 }, g_cs_any;
static uint32_t g_cs_work[2];
static DWORD g_cs_since[2];
static char g_cs_name[2][64];

static const char *cs_name(int k, int id, char *buf, size_t cap) {
    if (char_name(id, buf, cap)) return buf;
    for (int i = CS_NAME_FIRST; i <= CS_NAME_LAST; i++) {
        if (i == 63 || !text_visible(CS_NAMES, (uint32_t)i)) continue;
        const char *t = text_lookup(CS_NAMES, (uint32_t)i);
        if (!t || !strcmp(t, g_cs_name[!k])) continue;      /* the other player's name */
        snprintf(buf, cap, "%s", t);
        return buf;
    }
    return NULL;
}

static void cs_say(int k, int id, int interrupt) {
    char name[64];
    /* drawn this moment, not lingering from the slot just left (it is drawn every frame while shown) */
    /* a cell added by a roster mod has its host's slot number: name it from the wheel */
    const char *added = g_cs_work[k] ? roster_cell_name(g_cs_work[k]) : NULL;
    if (added) { speak_utf8(added, interrupt); snprintf(g_cs_name[k], sizeof g_cs_name[k], "%s", added); return; }
    if (text_visible_within(CS_NAMES, 63, 60)) { speak(L"Random or locked character", interrupt); g_cs_name[k][0] = 0; return; }
    if (!cs_name(k, id, name, sizeof name)) snprintf(name, sizeof name, "Character %d", id);
    speak_utf8(name, interrupt);
    snprintf(g_cs_name[k], sizeof g_cs_name[k], "%s", name);
}

static void cs_poll(void) {
    uint32_t w[2];
    int n = task_running("# STR SELECT #") || task_running("# ARENA CHARASE") ? 0 : task_works("CHARASEL", w, 2);
    g_cs_any = n > 0;
    if (!n) { g_cs_last[0] = g_cs_last[1] = g_cs_seen[0] = g_cs_seen[1] = -1; g_cs_name[0][0] = g_cs_name[1][0] = 0; return; }
    text_mute_pack_except(CS_NAMES, 25);      /* names come from here; "Computer" still read */
    for (int k = 0; k < 2; k++) {
        if (k >= n || !guest_ok(w[k] + 4, 1)) { g_cs_last[k] = g_cs_seen[k] = -1; continue; }
        g_cs_work[k] = w[k];
        /* slot and wheel cell together: new cells share their host's slot */
        int id = g8(w[k] + 4) | (roster_wheel_cell(w[k]) + 1) << 8;
        if (id != g_cs_seen[k]) { g_cs_seen[k] = id; g_cs_since[k] = GetTickCount(); }
        /* speak once the cursor rests: the picture for locked characters follows it */
        if (id != g_cs_last[k] && GetTickCount() - g_cs_since[k] >= 150) {
            cs_say(k, id & 0xff, g_cs_last[k] >= 0);
            g_cs_last[k] = id;
        }
    }
}

/* ---------- New Game / Load Game / Continue (after the title) ----------
 * Task "# DATA LOAD #". No cursor found in its data, but each item has its
 * own comment picture in pack 1447f931 (1 New Game, 2 Load Game, 3 Continue;
 * 0 is the greeting), so the comment on screen names the item. The comments
 * themselves are muted, as on the main menu. */
#define NL_PACK 0x1447f931u
static const char *k_nl[] = { "New Game", "Load Game", "Continue" };
static int g_nl = -1;

/* The item whose comment is the only one on screen, or -1 (none, or the
 * old and new comments both up while it changes). */
static int nl_item(void) {
    int item = -1;
    for (int i = 0; i < 3; i++)
        if (text_visible_within(NL_PACK, (uint32_t)i + 1, 60)) { if (item >= 0) return -1; item = i; }
    return item;
}

static int nl_say(int interrupt) {
    int i = task_running("# DATA LOAD #") ? nl_item() : -1;
    if (i < 0) return 0;
    speakf(interrupt, "%s, %d of 3", k_nl[i], i + 1);
    g_nl = i;
    return 1;
}

static void nl_poll(void) {
    if (!task_running("# DATA LOAD #")) { g_nl = -1; return; }
    text_mute_pack(NL_PACK);
    int i = nl_item();
    if (i >= 0 && i != g_nl) nl_say(g_nl >= 0);
}

/* ---------- Options screen ----------
 * Task "Option Main". Its labels and values are sprite sheets (pack
 * 8d4313d4), so the text reader read every label at once. No cursor found;
 * instead each category and setting has its own description picture (pack
 * 7334e8f2), so the one description on screen names the highlighted item.
 * Settings (found by stepping each one and back):
 *   824e6199 Game Difficulty, 824e619b COM Level (bytes, 0 = lowest)
 *   824e619c Duel Time (16 bit seconds, -1 none), 824e61cb Speaker (1 Stereo)
 *   824e61cc / 824e61d0 Music / Voice and effects volume (float, 1.0 full) */
#define OPT_DESC   0x7334e8f2u
#define OPT_LABELS 0x8d4313d4u
#define OPT_SET    0x824e6100u
static const char *const k_opt_names[29] = {
    "Game Options", "Game Options", "Game Options", "Save/Load", "Controller", "Screen", "Screen", "Sound",
    "Language", "Exit, save changes", "Exit", "Default", "Game Difficulty", "COM Level", "Duel Time", "Duel Time",
    "Duel Time", "Return", "Save", "Load", "Brightness Control", "Screen Display Mode", "Screen Adjust", "Speaker",
    "Music Volume", "Voice and Effects Volume", "Voice Language", "English", "Japanese",
};
static int g_opt_item = -1;
static char g_opt_val[64];

static int opt_item(void) {
    int item = -1;
    for (int i = 0; i < 29; i++)
        if (text_visible_within(OPT_DESC, (uint32_t)i, 60)) { if (item >= 0) return -1; item = i; }
    return item;
}

static const char *opt_value(int item, char *buf, size_t cap) {
    static const char *diff[] = { "Very Easy", "Easy", "Normal", "Hard", "Very Hard" };
    static const char *com[] = { "Very Weak", "Weak", "Average", "Strong", "Very Strong" };
    int v;
    switch (item) {
    case 12: v = g8(OPT_SET + 0x99); return v < 5 ? diff[v] : NULL;
    case 13: v = g8(OPT_SET + 0x9b); return v < 5 ? com[v] : NULL;
    case 14: case 15: case 16:
        v = (int16_t)g16(OPT_SET + 0x9c);
        if (v < 0) return "No time limit";
        snprintf(buf, cap, "%d seconds", v); return buf;
    case 23: return g8(OPT_SET + 0xcb) ? "Stereo" : "Mono";
    case 24: case 25: {
        float f = gf(OPT_SET + (item == 24 ? 0xcc : 0xd0));
        snprintf(buf, cap, "%d percent", (int)(f * 100 + 0.5f)); return buf;
    }
    }
    return NULL;
}

static int opt_say(int interrupt) {
    if (!task_running("Option Main") || g_opt_item < 0) return 0;
    char vb[64]; const char *v = opt_value(g_opt_item, vb, sizeof vb);
    if (v) speakf(interrupt, "%s: %s", k_opt_names[g_opt_item], v);
    else speak_utf8(k_opt_names[g_opt_item], interrupt);
    return 1;
}

static void opt_poll(void) {
    if (!task_running("Option Main")) { g_opt_item = -1; text_mute_pack2(0); return; }
    text_mute_pack2(OPT_DESC);
    text_mute_pack(OPT_LABELS);
    int item = opt_item();
    char vb[64]; const char *v = g_opt_item >= 0 ? opt_value(g_opt_item, vb, sizeof vb) : NULL;
    if (item >= 0 && item != g_opt_item) {
        int first = g_opt_item < 0;
        g_opt_item = item;
        v = opt_value(item, vb, sizeof vb);
        snprintf(g_opt_val, sizeof g_opt_val, "%s", v ? v : "");
        opt_say(!first);
    } else if (v && strcmp(v, g_opt_val)) {          /* value changed with left / right */
        snprintf(g_opt_val, sizeof g_opt_val, "%s", v);
        speak_utf8(v, 1);
    }
}

/* ---------- Controller screen (Options, Controller) ----------
 * Also under "Option Main". Rows named by their description pictures (pack
 * f0e76d35): 0 / 1 the Player 1 / Player 2 tab, 2 / 3 / 4 Config (the
 * description follows the type: Type A, Type B, Custom), 5 Vibration.
 * Settings per player, Player 1 at 824e6182, Player 2 at 824e618c: +0 type
 * (0 Type A, 1 Type B, 2 Custom), +1 vibration (0 On, 1 Off), +2.. the eight
 * buttons LT RT LB RB Y B A X, each a mask of Punch 1, Kick 2, Guard 4, Ki 8.
 * In Custom you hold a button and press the D-pad to change what it does. */
#define CTL_DESC 0xf0e76d35u
static const char *const k_ctl_btn[8] = { "LT", "RT", "LB", "RB", "Y", "B", "A", "X" };
static int g_ctl_row = -1, g_ctl_player;
static uint8_t g_ctl_set[10];

static uint32_t ctl_base(void) { return g_ctl_player ? 0x824e618cu : 0x824e6182u; }

static void ctl_action(int m, char *buf, size_t cap) {
    static const char *w[4] = { "Punch", "Kick", "Guard", "Ki" };
    int n = 0; buf[0] = 0;
    for (int i = 0; i < 4; i++) if (m & (1 << i)) n += snprintf(buf + n, cap - n, "%s%s", n ? " and " : "", w[i]);
    if (!n) snprintf(buf, cap, "nothing");
}

static int ctl_row(void) {
    int r = -1;
    for (int i = 0; i < 6; i++)
        if (text_visible_within(CTL_DESC, (uint32_t)i, 60)) { if (r >= 0) return -1; r = i; }
    return r;
}

static void ctl_layout(char *buf, size_t cap) {
    uint32_t b = ctl_base(); int n = 0; char a[48];
    for (int i = 0; i < 8; i++) { ctl_action(g8(b + 2 + (uint32_t)i), a, sizeof a); n += snprintf(buf + n, cap - n, "%s %s%s", btn_name(k_ctl_btn[i]), a, i < 7 ? ", " : "."); }
}

static int ctl_say(int interrupt, int full) {
    if (g_ctl_row < 0) return 0;
    static const char *types[3] = { "Type A", "Type B", "Custom" };
    uint32_t b = ctl_base(); char buf[600]; int n;
    int type = g8(b), vib = g8(b + 1);
    if (g_ctl_row <= 1) n = snprintf(buf, sizeof buf, "Player %d", g_ctl_row + 1);
    else if (g_ctl_row <= 4) n = snprintf(buf, sizeof buf, "Config: %s%s", type < 3 ? types[type] : "?",
                                         type == 2 ? ". To change a button, hold it and press the D-pad" : "");
    else n = snprintf(buf, sizeof buf, "Vibration: %s", vib ? "Off" : "On");
    if (full) { n += snprintf(buf + n, sizeof buf - n, ". Player %d buttons: ", g_ctl_player + 1); ctl_layout(buf + n, sizeof buf - n); }
    speak_utf8(buf, interrupt);
    return 1;
}

static void ctl_poll(void) {
    int on = task_running("Option Main") && (text_visible(CTL_DESC, 0) || text_visible(CTL_DESC, 1) || text_visible(CTL_DESC, 2)
             || text_visible(CTL_DESC, 3) || text_visible(CTL_DESC, 4) || text_visible(CTL_DESC, 5));
    if (!on) { g_ctl_row = -1; return; }
    text_mute_pack2(CTL_DESC);
    int r = ctl_row();
    if (r == 0 || r == 1) g_ctl_player = r;
    uint32_t b = ctl_base();
    uint8_t now[10];
    for (int i = 0; i < 10; i++) now[i] = g8(b + (uint32_t)i);
    if (g_ctl_row < 0) {                          /* screen just opened: row and the whole layout */
        if (r < 0) return;
        g_ctl_row = r; memcpy(g_ctl_set, now, 10);
        ctl_say(1, 1);
        return;
    }
    int changed = 0, which = -1;
    for (int i = 2; i < 10; i++) if (now[i] != g_ctl_set[i]) { changed++; which = i - 2; }
    if (r >= 0 && r != g_ctl_row) { g_ctl_row = r; ctl_say(1, 0); }
    else if (now[0] != g_ctl_set[0] || now[1] != g_ctl_set[1]) ctl_say(1, 0);
    if (changed == 1) {                           /* one button changed (Custom) */
        char a[48]; ctl_action(now[2 + which], a, sizeof a);
        speakf(1, "%s: %s", k_ctl_btn[which], a);
    } else if (changed > 1) {                     /* a type or Default changed them all */
        char buf[500]; ctl_layout(buf, sizeof buf); speak_utf8(buf, 0);
    }
    memcpy(g_ctl_set, now, 10);
}

/* ---------- World Tournament: course and number of players ----------
 * Task "# TOUR MODE #", work +0xaf course (0 Novice, 1 Adept, 2 Advanced,
 * 3 Cell Games), +0xb0 number of players - 1. The course comes first; the
 * players question shows announcer line 4 of pack d164cbde ("Please select
 * the number of players!"), and line 3 means the course is locked. The
 * course names are one sheet (pack 29f6ab39) and are muted, as are the
 * announcer's lines. */
#define WT_COMMENTS 0xd164cbdeu
#define WT_SHEET    0x29f6ab39u
static const char *const k_wt_course[4] = { "Novice, 3 rounds", "Adept, 4 rounds", "Advanced, 5 rounds", "Cell Games, 5 rounds" };
static int g_wt_step = -1, g_wt_val = -1, g_wt_locked = -1;
static DWORD g_wt_since;

static int wt_say(int interrupt) {
    uint32_t w = task_running("CHARASEL") ? 0 : task_work("# TOUR MODE #");
    if (!w || g_wt_step < 0) return 0;
    if (g_wt_step == 1) speakf(interrupt, "Number of players: %d", g8(w + 0xb0) + 1);
    else {
        int c = g8(w + 0xaf);
        /* Cell Games only once unlocked (capsule 515 "World Tournament-Cell Games") */
        int n = c == 3 || g8(LIVE_G_EARLY + 0x2afe9 + 515) ? 4 : 3;
        speakf(interrupt, "%s%s, %d of %d", c < 4 ? k_wt_course[c] : "Course", text_visible(WT_COMMENTS, 3) ? ", locked" : "", c + 1, n);
    }
    return 1;
}

static void wt_poll(void) {
    if (task_running("CHARASEL")) return;     /* keep the state: no repeat of the course after picking */
    uint32_t w = task_work("# TOUR MODE #");
    if (!w || !guest_ok(w + 0xaf, 2)) { g_wt_step = g_wt_val = g_wt_locked = -1; return; }
    text_mute_pack(WT_SHEET);
    text_mute_pack2(WT_COMMENTS);
    int step = text_visible(WT_COMMENTS, 4) ? 1 : 0;
    int val = g8(w + (step ? 0xb0 : 0xaf));
    int locked = text_visible(WT_COMMENTS, 3);
    if (step != g_wt_step || val != g_wt_val) { g_wt_step = step; g_wt_val = val; g_wt_locked = -1; g_wt_since = GetTickCount(); }
    /* wait for the comment that follows the cursor (locked or not) before speaking */
    if (g_wt_locked < 0 && GetTickCount() - g_wt_since >= 250) { g_wt_locked = locked; wt_say(1); }
}

/* ---------- Shenron's wishes (Dragon Universe, all 7 Dragon Balls) ----------
 * Task "Shenron Msg" shows the choices; object *(8243a260): +0x14 number of
 * wishes, +0x18 capsule ID of each (-1 none), +0x24 cursor. The capsule
 * name pictures are muted while it runs (the text reader would say all of
 * them at once); A picks the wish, and its capsule card follows. */
#define SHENRON_OBJ 0x8243a260u
static int g_wish = -1;

static int wish_say(int interrupt) {
    uint32_t o = g32(SHENRON_OBJ);
    if (!task_running("Shenron Msg") || !o || !guest_ok(o + 0x14, 0x14)) return 0;
    int n = (int)g32(o + 0x14), c = (int)g32(o + 0x24);
    if (n < 1 || n > 3 || c < 0 || c >= n) return 0;
    int id = (int)g32(o + 0x18 + 4 * c);
    speakf(interrupt, "%s, %d of %d", id >= 0 ? capsule_name(id) : "Wish", c + 1, n);
    return 1;
}

static void wish_poll(void) {
    uint32_t o = g32(SHENRON_OBJ);
    if (!task_running("Shenron Msg") || !o || !guest_ok(o + 0x14, 0x14)) { g_wish = -1; return; }
    text_mute_pack(CAPSULE_NAMES);
    int c = (int)g32(o + 0x24);
    if (c == g_wish) return;
    if (g_wish < 0) speakf(0, "Choose a wish, up and down, A to choose.");
    g_wish = c;
    wish_say(0);
}

/* ---------- Training lessons ----------
 * The command a lesson asks for ("-> P+K" at the bottom of the screen) is a
 * row of command icons drawn by [FIX]TRL DRAW from the block at 823ce050:
 * +4 lesson on, +8 command shown, +0x10 up to 16 command codes (32 bit,
 * -1 ends them; same codes as the skill lists). A new command is spoken once
 * it has held for 600 ms, so the teacher's line comes first; the strip
 * hiding while a hit lands does not repeat it. */
#define TRL_BLOCK 0x823ce050u
static void trl_poll(void) {
    static char spoken[300], pending[300];
    static DWORD since;
    if (!task_running("[FIX]TRL DRAW") || !guest_ok(TRL_BLOCK, 0x50)) { spoken[0] = pending[0] = 0; return; }
    if (!g32(TRL_BLOCK + 4) || !g32(TRL_BLOCK + 8)) return;
    char buf[300];
    int n = snprintf(buf, sizeof buf, "Command: "), any = 0;
    for (uint32_t i = 0; i < 16; i++) {
        int c = (int)g32(TRL_BLOCK + 0x10 + 4 * i);
        if (c < 0) break;
        n += snprintf(buf + n, sizeof buf - n, "%s%s", any ? ", then " : "", cmd_word(c));
        any = 1;
    }
    if (!any) return;
    if (strcmp(buf, pending)) { snprintf(pending, sizeof pending, "%s", buf); since = GetTickCount(); }
    if (strcmp(pending, spoken) && GetTickCount() - since >= 600) {
        speak_utf8(pending, 0);
        snprintf(spoken, sizeof spoken, "%s", pending);
    }
}

void menus_poll(void) {
    for (int i = 0; i < NMENUS; i++) {
        Menu *m = &k_menus[i];
        uint32_t w = menu_base(m);
        if (!w || (m->not_task && task_running(m->not_task)) || !guest_ok(w + m->cursor_off, m->size)) {
            if (m->last != -1 || m->on_extra) m->closed_on_extra = m->on_extra;
            m->last = -1; m->on_extra = 0; continue;
        }
        int c = m->size == 4 ? (int)g32(w + m->cursor_off) : g8(w + m->cursor_off);
        /* Port 1.5.5: leaving Online goes back to the HUB, which opens over the main menu
         * (the menu's cursor is on Dueling then): stay quiet as on the HUB row. */
        if (m->last == -1 && m->closed_on_extra && !strcmp(m->task, "# MAIN MENU #")) { m->on_extra = 1; m->moves = g_pad_moves; m->last = -2; }
        m->closed_on_extra = 0;
        /* On a port row the game's cursor stays where it was; imgui.c reads the row.
         * Coming back from it, say the game's item again. */
        if (m->extra) {
            int on = im_row_on() || im_menu_on();      /* a port screen over the menu (ONLINE) too */
            if (on) { if (!m->on_extra) m->moves = g_pad_moves; m->on_extra = 1; continue; }   /* the description fades after the press that leaves */
            /* Inside the HUB screen (its last row was not the main menu's HUB row) the game's
             * cursor sits on 0 underneath and the descriptions blink out between rows; presses
             * there don't count, so backing out waits for the HUB row's description. */
            if (m->on_extra && !strcmp(m->task, "# MAIN MENU #") && strcmp(im_row(), "HUB") && (im_port_recent(800) || im_any_text())) { m->moves = g_pad_moves; continue; }
            /* The row's description is gone: back on the game's rows only after up or down
             * (not while the row's own screen, the HUB or ONLINE, opens over the menu). */
            if (m->on_extra && g_pad_moves == m->moves) continue;
            if (m->on_extra) { m->on_extra = 0; m->last = -2; }
        }
        if (c < 0 || c >= m->count || c == m->last) { m->wait0 = 0; continue; }
        /* Port 1.5.5 sets the game's cursor to 0 on the HUB row too. From the last
         * game row it can only be the HUB (imgui.c says it); otherwise give the row's
         * description a moment to show before saying the first item. */
        int hub = m->extra && !strcmp(m->task, "# MAIN MENU #");
        if (hub && c == 0 && m->last >= 1 && m->last == menu_rows(m) - 1) {
            m->on_extra = 1; m->moves = g_pad_moves; m->last = -2; continue;
        }
        if (hub && c == 0 && m->last != -1) {
            if (!m->wait0) m->wait0 = GetTickCount();
            if (GetTickCount() - m->wait0 < 150) continue;
        }
        m->wait0 = 0;
        say_item(m, c, m->last != -1);
        m->last = c;
    }
    shop_poll(0);
    pause_poll();
    trl_poll();
    stage_poll();
    bonus_poll();
    strsel_poll();
    sysq_poll();
    /* Words these readers speak themselves stay quiet in the text reader. */
    int yes_no = text_visible(PAUSE_TEXTS, 13) && text_visible(PAUSE_TEXTS, 14);   /* its reader says title and answer */
    text_mute_pack(task_running("ESS MAIN") ? CAPSULE_NAMES : (g_pause_row >= 0 || g_layout >= 0 || yes_no) ? PAUSE_TEXTS : 0);
    text_mute_all(g_skill_row >= 0 || g_strsel_last >= 0 || task_running("# MAIN MENU #"));   /* main menu: just the mode, not Goku's comments */   /* story select: its reader says name, level, stats */
    cs_poll();                 /* after the mute above: it mutes the name pictures itself */
    wish_poll();               /* after the mute above too */
    nl_poll();
    opt_poll();
    ctl_poll();                /* after opt_poll: both run under "Option Main" */
    wt_poll();
}

/* F5: say the highlighted item of any open menu. */
int menus_read_current(void) {
    if (im_row_on() || im_menu_on()) return im_read_all(1);
    if (g_wish >= 0) return wish_say(1);
    if (g_stage_last >= 0) return stage_say(1);
    if (g_bonus_last >= 0) return bonus_say(1);
    if (g_nl >= 0) return nl_say(1);
    if (g_ctl_row >= 0) return ctl_say(1, 1);
    if (g_wt_step >= 0 && wt_say(1)) return 1;   /* not during its character select */
    if (g_opt_item >= 0) return opt_say(1);
    if (g_sysq >= 0) return sysq_say(1);
    if (g_strsel_last >= 0) return strsel_say(1, 1);
    if (g_cs_any) {            /* the player whose cursor moved last */
        int k = g_cs_last[1] >= 0 && g_cs_since[1] > g_cs_since[0] ? 1 : 0;
        if (g_cs_last[k] >= 0) { cs_say(k, g_cs_last[k], 1); return 1; }
    }
    if (g_skill_row >= 0) { g_skill_row = -1; return 1; }   /* re-read on the next poll */
    if (g_shop_tab >= 0) { shop_poll(1); return 1; }
    if (g_pause_row >= 0) { pause_say(g_pause_row, 1); return 1; }
    if (g_layout >= 0) { g_lay_row = -1; return 1; }   /* re-announced on the next poll */
    for (int i = 0; i < NMENUS; i++) {
        Menu *m = &k_menus[i];
        if (m->last >= 0) { say_item(m, m->last, 1); return 1; }
    }
    return 0;
}
