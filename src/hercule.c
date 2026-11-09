/*
 * Legend of Hercule (port 1.5.6, HUB > Legend of Hercule, from the player's
 * Budokai 1 disc). The port draws the mode itself (pasarela.cpp) from disc
 * pictures: the title, the difficulty board, Hercule's talk before each
 * challenger and the rule of the fight are images, so the only ImGui text is
 * the button legend. What the mod has instead is the port's log
 * ("dbz3 pasarela: ..." lines, followed by ship.c): each step of the mode
 * ("paso k/n (presentacion|charla|combate ronda r): pantalla|combate|perdido"),
 * START on the title, the difficulty picked, each fight's set-up and end, the
 * points, and the end of the legend. The difficulty cursor itself is not
 * logged: imgui.c finds it from where the port draws the three option
 * pictures (the chosen one is drawn last).
 * The talk text below is the USA Budokai 1 text of those pictures (DATA_US.AFS
 * bank 669), two per challenger from the 2nd: what Hercule says and thinks, and
 * the rule line.
 */
#include "access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int g_silent;
static void say(int interrupt, const char *fmt, ...) {
    if (g_silent) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    speak_utf8(buf, interrupt);
}

static const char *const k_rivals[11] = { "Yamcha", "Tien", "Krillin", "Android 16", "Trunks", "Vegeta",
                                          "Piccolo", "Cell Jr.", "Goku", "Gohan", "Cell" };
static const char *const k_ord[11] = { "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th", "10th", "11th" };
/* challengers 2..11: { says, thinks, rule } */
static const char *const k_talk[10][3] = {
    { "Hmph! That was a wimp, heh! You two sick bald turkeys. I'll deal with both of you at once!",
      "Jeez, he's got three eyes. What shape are his glasses?",
      "Fight both Tien and Krillin, but your Health won't recover!" },
    { "Wimp, spineless jellyfish. Yer a pushover too then? My remainin' power'll be plenty for you. Come on then!",
      "Then again I'm a bit pooped, heh. Maybe... maybe I shouldn't have said both at once...",
      "Fight Krillin without recovering Health! Can your body take it?!" },
    { "Cheater! That's against the rules. What's with the armor? Aw, still, that's not gonna help against Hercule... bah hah hah hah hah hah!",
      "Ooh, tough looking armor, stinking c-coward.",
      "You can't hurt Android 16 easily! Don't give it a chance to attack!" },
    { "So, yer next? But yer still a boy! I'll go easy on ya. Give it yer best shot.",
      "Ow, fighting that useless robot must've smashed up my arms and legs!",
      "Your hands and legs are injured! Use your Ki to attack!!" },
    { "You lookin' at me? You got somethin' to say? And what's with the weird hair? Get outta here!",
      "Ahh, oh, I was talkin' the talk but I'm... suddenly gettin' a pain in my gut. Ohh gotta hang... on... oh.",
      "Defeat Vegeta within the time limit to make it to the bathroom!" },
    { "Hey you! Yer lookin' pale. Don't think I'm gonna go easy on ya. The world champion ain't like that, bah hah hah hah!",
      "What?! That looks just like the Evil King Piccolo! Whoa, I'm in it now.",
      "Piccolo the Evil King?! Fight even though it's tough to gather Ki!" },
    { "I got rid of them... What's this? Who are you?! So you gotta gang up on me now, huh? Bring it on!",
      "Ooh, he's full of monsters! Why doesn't anybody tell me anything?!",
      "Five Cell Jrs. appeared! Focus yourself and take them out!" },
    { "Ya, all right, lemme tell ya something. Yer outta yer league! Hey! Blond dude! I'll deal with YOU!",
      "Ohhh, I'm a goner, he... he might be just a bit too powerful...",
      "A battle-weary Hercule faces Goku in a match!" },
    { "You're just a kid! Go and do your homework, okay? What?! Respect your elders!!",
      "Huh? Well, this looks like that blond freak I just fought. Ohh, I'm too young to die...",
      "An exhausted Hercule has to challenge Gohan! Good luck!" },
    { "Did I keep you waiting Cell? Well the laughter stops right here! I'll wipe the floor with you and protect the whole world! So let's rumble!",
      "Ter-riffic!",
      "Defeat Cell, the Source of Evil! Can you become a legend?" },
};
/* the port's difficulty order (0x5E9C10 / kDificultad: Novato, Avanzado, Experto) and the board's words */
static const char *const k_dif[3] = { "Novice", "Adept", "Advanced" };

static int g_active, g_board;          /* in the mode; on the difficulty board */
static int g_dif;
static int g_round = -1;
static int g_points;
static int g_scene;                   /* a cut-scene fight is running */
static DWORD g_scene_end;
static int g_pics = 1;                 /* the port found the disc's pictures (not on the USA disc: other file names) */
static int g_final;                    /* on the ending's points and password screen */
static DWORD g_left_at;                /* the main menu under the HUB shows for a moment as the mode ends */

static int dif_index(const char *s) {
    if (!strncmp(s, "Novato", 6)) return 0;
    if (!strncmp(s, "Avanzado", 8)) return 1;
    if (!strncmp(s, "Experto", 7)) return 2;
    return -1;
}

static const char *after(const char *line, const char *key) {
    const char *p = strstr(line, key);
    return p ? p + strlen(key) : NULL;
}

void hercule_line(const char *line, int silent) {
    const char *p;
    g_silent = silent;
    if (strstr(line, "dbz3 pasarela: ") && strstr(line, " leida en ")) {
        g_pics = strstr(line, "charlas - ") == NULL;
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: la leyenda de Mr. Satan ("))) {
        const char *d = strstr(p, "desde el paso ");
        const char *pt = strstr(p, " (");
        g_active = 1; g_board = 0; g_round = -1; g_scene = 0; g_final = 0;
        g_points = 0;
        if (d && (pt = strrchr(d, '('))) g_points = atoi(pt + 1);
        if (d && (d = strchr(d, ',')) && dif_index(d + 2) >= 0) g_dif = dif_index(d + 2);
        log_line("hercule: entered, difficulty %d", g_dif);
        goto out;
    }
    if (strstr(line, "dbz3 pasarela: fuera del modo") || strstr(line, "dbz3 pasarela: leyenda completada")) {
        g_active = 0; g_board = 0; g_scene = 0;
        g_left_at = GetTickCount();
        log_line("hercule: left");
        goto out;
    }
    if (!g_active) goto out;
    /* the round 1 intro, the round intros and the ending run as fights the player doesn't play */
    if ((p = after(line, "dbz3 pasarela: escena "))) {
        if (strstr(p, "acabada") || strstr(p, "terminada")) { if (g_scene) g_scene_end = GetTickCount(); g_scene = 0; }
        else g_scene = 1;
        goto out;
    }
    if (strstr(line, "dbz3 pasarela: titulo: START")) {
        g_board = 1;
        say(1, "Pick the difficulty: %s, %d of 3. Up and down to change, A to start.", k_dif[g_dif], g_dif + 1);
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: dificultad "))) {
        int d = dif_index(p);
        if (d >= 0) g_dif = d;
        g_board = 0;
        say(1, "%s", k_dif[g_dif]);
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: paso "))) {   /* "k/n (TYPE ronda r[ vez v]): PHASE" */
        const char *t = strchr(p, '('), *ph = strstr(p, "): ");
        const char *rn = strstr(p, " ronda ");
        if (!t || !ph || !rn) goto out;
        int r = atoi(rn + 7) - 1;
        ph += 3;
        if (r < 0 || r > 10) goto out;
        g_round = r;
        if (!strncmp(t + 1, "presentacion", 12) && !strncmp(ph, "pantalla", 8)) {
            g_board = 0;
            /* without the disc's pictures the port has no title screen: the difficulty rows come
             * straight away as text (imgui.c reads them) */
            if (g_pics) say(1, "Legend of Hercule. Hercule takes on 11 challengers at the Cell Games. Press START.");
            else say(1, "Legend of Hercule. Hercule takes on 11 challengers at the Cell Games. Pick the difficulty with up and down, then A.");
        } else if (!strncmp(t + 1, "charla", 6) && !strncmp(ph, "pantalla", 8) && r >= 1) {
            const char *const *k = k_talk[r - 1];
            say(1, "Challenger %d of 11: %s. Hercule: %s Thinking: %s %s You have %d points. A for next, B to quit.",
                r + 1, k_rivals[r], k[0], k[1], k[2], g_points);
        } else if (!strncmp(ph, "perdido", 7)) {
            say(1, "Hercule is down! A to retry, B to quit.");
        }
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: ronda "))) {   /* "r (v/n): id A contra id B ..." */
        int r = 0, v = 0, n = 0;
        if (sscanf(p, "%d (%d/%d)", &r, &v, &n) == 3 && r >= 1 && r <= 11) {
            if (g_scene) g_scene_end = GetTickCount();
            g_scene = 0;
            if (n > 1) say(1, "%s challenger: Hercule versus %s, %d of %d", k_ord[r - 1], k_rivals[r - 1], v, n);
            else say(1, "%s challenger: Hercule versus %s", k_ord[r - 1], k_rivals[r - 1]);
        }
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: fin del combate (paso "))) {
        const char *g = strstr(p, "gana el lado ");
        if (g && atoi(g + 13) != 1) say(1, "You lost");
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: puntos del combate "))) {   /* "P (...), ronda R, total T" */
        const char *rd = strstr(p, "ronda "), *tt = strstr(p, "total ");
        if (tt) g_points = atoi(tt + 6);
        if (rd && tt) say(1, "You won! %d points this challenger. Total %d.", atoi(rd + 6), atoi(tt + 6));
        goto out;
    }
    if ((p = after(line, "dbz3 pasarela: final de la leyenda ("))) {
        const char *pt = strstr(p, "pasos, ");
        g_final = 1;
        say(1, "Legend complete! Hercule beat all 11 challengers. Total %d points. "
               "The password below was for a 2002 website that no longer exists.", pt ? atoi(pt + 7) : 0);
        goto out;
    }
out:
    g_silent = 0;
}

void hercule_poll(void) {
    if (!g_active || !g_board) return;
    int d = im_b1_dif();
    if (d < 0 || d == g_dif) return;
    g_dif = d;
    say(1, "%s, %d of 3", k_dif[g_dif], g_dif + 1);
}

int hercule_busy(void) { return g_active || (g_left_at && GetTickCount() - g_left_at < 1500); }
/* B1's password is drawn in its own symbol font: as text it is noise */
int hercule_skip_text(const char *s) {
    if (!g_active || !g_final) return 0;
    for (; *s; s++) if (strchr("`\\_&", *s)) return 1;
    return 0;
}

int hercule_scene(void) { return g_active && (g_scene || GetTickCount() - g_scene_end < 2000); }
