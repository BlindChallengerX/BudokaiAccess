/*
 * Characters and capsules added by roster mods (port 1.5: Nova's Mods
 * characters such as Janemba, the Ginyu Force or the Infinite World pack).
 *
 * The port merges every character mod into mods\_roster\roster.toml when the
 * player presses PLAY. Each [[personaje]] block starts with a comment
 * "# Display name (mod folder)" and has id, celda = true (its own cell on the
 * character wheel) and tras_casilla (the slot of the cell it comes after;
 * without it, the donor's slot). [[capsula]] blocks are new capsules (ID 596
 * and up), also named by the comment above their id.
 *
 * The wheel (select_ext.cpp in the port): the per-player select state (task
 * "CHARASEL" work) +40 wheel object, its +48 wheel data: +12 s8 number of
 * cells, +40 current cell pointer, +44 cells of 28 bytes (+2 s8 slot). A new
 * cell has its host's slot, so the game's slot byte can't tell it apart;
 * new cells sit right after the original cell with their tras_casilla slot,
 * in roster.toml order, which is how they are told apart here.
 */
#include "access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXNEW 64
typedef struct { char name[64]; int id, cell, after, donor, alias; } NewChar;
static NewChar g_new[MAXNEW];
static int g_nnew = -1;
#define MAXCAPS 256
static struct { int id; char name[64]; } g_caps[MAXCAPS];
static int g_ncaps;

#define ID_TO_SLOT 0x82020668u    /* u16 per character ID */

/* English names from the Nova's Mods catalog for mods whose roster name is
 * Spanish (roster.toml takes the mod's own name); keyed by mod folder. */
static const struct { const char *mod, *name; } k_english[] = {
    { "vis_b2_tenshinhan", "Tien (Budokai 2)" },
    { "vis_b2_yamhan", "Yamhan (Budokai 2 fusion)" },
    { "vis_b2_yamhan_fallida", "Yamhan (failed fusion)" },
    { "vis_b2_fat_gotenks", "Fat Gotenks" },
    { "vis_b2_ghost_gotenks", "Ghost Gotenks" },
    { "vis_iw_vegeta_saiyan", "Vegeta (Saiyan Saga)" },
    { "vis_iw_yamcha_amarillo", "Yamcha (Yellow Suit)" },
    { "vis_sdbh_roshi", "Master Roshi" },
    { "imp_iw_gran_saiyaman2", "Great Saiyaman 2" },
    { "cut_android19", "Android 19" },
};

static void load(void) {
    g_nnew = 0; g_ncaps = 0;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsmods\\_roster\\.disabled", g_game_dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    swprintf(path, MAX_PATH, L"%lsmods\\_roster\\roster.toml", g_game_dir);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    char line[1024], comment[64] = "";
    enum { NONE, CHAR, CAP } block = NONE;
    NewChar *c = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (!strcmp(t, "[[personaje]]")) {
            block = g_nnew < MAXNEW ? CHAR : NONE;
            c = block == CHAR ? &g_new[g_nnew++] : NULL;
            if (c) { memset(c, 0, sizeof *c); c->id = c->after = c->donor = c->alias = -1; }
            comment[0] = 0;
            continue;
        }
        if (!strcmp(t, "[[capsula]]")) { block = CAP; comment[0] = 0; continue; }
        if (*t == '[') { block = NONE; continue; }
        if (*t == '#') {                      /* "# Name (mod folder)" or "# Name" */
            snprintf(comment, sizeof comment, "%s", t + 1 + (t[1] == ' '));
            char *mod = strstr(comment, " (mod ");
            if (mod) {
                *mod = 0;
                char folder[64]; snprintf(folder, sizeof folder, "%s", mod + 6);
                folder[strcspn(folder, ")")] = 0;
                for (size_t i = 0; i < sizeof k_english / sizeof k_english[0]; i++)
                    if (!strcmp(folder, k_english[i].mod)) snprintf(comment, sizeof comment, "%s", k_english[i].name);
            }
            if (c && block == CHAR && !c->name[0]) snprintf(c->name, sizeof c->name, "%s", comment);
            continue;
        }
        char *eq = strchr(t, '=');
        if (!eq) continue;
        char key[32]; int kn = (int)(eq - t);
        while (kn > 0 && t[kn - 1] == ' ') kn--;
        snprintf(key, sizeof key, "%.*s", kn, t);
        char *v = eq + 1; while (*v == ' ') v++;
        if (block == CHAR && c) {
            if (!strcmp(key, "id")) c->id = atoi(v);
            else if (!strcmp(key, "celda")) c->cell = !strncmp(v, "true", 4);
            else if (!strcmp(key, "tras_casilla")) c->after = atoi(v);
            else if (!strcmp(key, "donante")) c->donor = atoi(v);
            else if (!strcmp(key, "alias")) c->alias = atoi(v);
        } else if (block == CAP && !strcmp(key, "id") && comment[0] && g_ncaps < MAXCAPS) {
            g_caps[g_ncaps].id = atoi(v);
            snprintf(g_caps[g_ncaps].name, sizeof g_caps[g_ncaps].name, "%s", comment);
            g_ncaps++;
        }
    }
    fclose(f);
    log_line("roster: %d new characters, %d new capsules", g_nnew, g_ncaps);
}

static void ensure(void) { if (g_nnew < 0) load(); }

/* Name of a character added by a roster mod, by character ID, or NULL. */
const char *roster_char_name(int id) {
    ensure();
    for (int i = 0; i < g_nnew; i++) if (g_new[i].id == id && g_new[i].name[0]) return g_new[i].name;
    return NULL;
}

/* Name of a capsule added by a roster mod (ID 596 and up), or NULL. */
const char *roster_capsule_name(int id) {
    ensure();
    for (int i = 0; i < g_ncaps; i++) if (g_caps[i].id == id) return g_caps[i].name;
    return NULL;
}

static int donor_slot(const NewChar *c) {
    return c->donor >= 0 && c->donor < 64 ? (int16_t)g16(ID_TO_SLOT + 2 * (uint32_t)c->donor) : -1;
}

/* The slot a new cell's own data is kept under (its host): alias, else the donor's slot. */
static int host_slot(const NewChar *c) { return c->alias >= 0 ? c->alias : donor_slot(c); }

/* The slot a new cell comes after. */
static int after_slot(const NewChar *c) {
    if (c->after >= 0) return c->after;
    return host_slot(c);
}

/* The wheel cell under the cursor of this select state, or -1. */
int roster_wheel_cell(uint32_t work) {
    uint32_t wobj = g32(work + 40);
    if (!wobj || !guest_ok(wobj + 48, 4)) return -1;
    uint32_t d = g32(wobj + 48);
    if (!d || !guest_ok(d, 44 + 28 * 64)) return -1;
    uint32_t cur = g32(d + 40);
    if (cur < d + 44) return -1;
    uint32_t k = (cur - d - 44) / 28;
    int n = (int8_t)g8(d + 12);
    return k < (uint32_t)n && k < 64 ? (int)k : -1;
}

/* If the cursor of this select state is on a new cell, the character's name; else NULL. */
const char *roster_cell_name(uint32_t work) {
    ensure();
    if (!g_nnew) return NULL;
    int k = roster_wheel_cell(work);
    if (k < 0) return NULL;
    uint32_t d = g32(g32(work + 40) + 48);
    /* Walk the wheel: each original cell is followed by the new cells that come after its slot. */
    int pending[MAXNEW], np = 0, pi = 0;
    for (int i = 0; i <= k; i++) {
        int slot = (int8_t)g8(d + 44 + 28 * (uint32_t)i + 2);
        if (pi < np && slot == host_slot(&g_new[pending[pi]])) {   /* a new cell */
            if (i == k) return g_new[pending[pi]].name;
            pi++;
            continue;
        }
        if (i == k) return NULL;          /* an original cell */
        np = pi = 0;
        for (int j = 0; j < g_nnew; j++)
            if (g_new[j].cell && after_slot(&g_new[j]) == slot) pending[np++] = j;
    }
    return NULL;
}
