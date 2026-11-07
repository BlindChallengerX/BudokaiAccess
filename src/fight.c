/*
 * Fights: the distance indicator and fight announcements.
 *
 * Fighter records are at fixed addresses (one per side), position (x, height,
 * z) at +0x60. The battle work struct (pointer at 823d6cf4) holds the camera:
 * +0x190 camera position, +0x1a0 the point it looks at (between the
 * fighters). From these the mod knows how far apart the fighters are, how
 * high the opponent is, and on which side of the screen the opponent is,
 * which is what forward and back on the stick depend on.
 *
 * During a fight a distance sound plays, chosen with Shift+F6: a subtle hum
 * (default; soft sine, 110 Hz far to 330 Hz touching, louder when closer), a
 * steady tone (as in Tekken 3 Access: 880 Hz touching, 220 Hz far, reedy when
 * the opponent is on the left and smooth on the right), or off. Either is
 * panned to the opponent's side of the screen. A soft chirp every so often
 * slides up when the opponent is above you and down when below. The tone runs from "Fight!" until the round ends and is
 * quiet while the game is paused, the Ultimate minigame or a Dragon Rush.
 *
 * Which side is you: in World Tournament the human is sometimes player 2.
 * Each side's gauge block has the controlling pad at +0x3e (16 bit, -1 for
 * the computer); see my_side().
 */
#include "access.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <mmsystem.h>

#define P1_REC 0x823cea10u
#define P2_REC 0x823d0a40u
#define GAUGE_P1 0x823cc6e4u
#define GAUGE_P2 0x823cc8c4u
#define GAUGE_BASE_PTR 0x82324064u   /* -> side 0 gauge block - 4; blocks are 0x1e0 apart */
#define BTL_WORK_PTR 0x823d6cf4u

volatile int g_beeps_on = 1;   /* Dragon Universe guide beep (Shift+F6 in the field) */

/* Fight distance sound (Shift+F6 in a fight), kept in
 * Budokai3Access\distance_sound.txt: 0 off, 1 subtle hum (default; a soft low
 * sine, as in Soulcalibur II Access), 2 the steady Tekken 3 style tone. Off
 * only silences the distance: the ring edge knock, the Ultimate meter and
 * the Dragon Universe guide keep playing. */
#define DIST_OFF 0
#define DIST_SUBTLE 1
#define DIST_STEADY 2
static volatile int g_dist_mode = DIST_SUBTLE;
static void dist_path(wchar_t *p) { swprintf(p, MAX_PATH, L"%lsdistance_sound.txt", g_mod_dir); }
static void dist_load(void) {
    wchar_t p[MAX_PATH]; dist_path(p);
    FILE *f = _wfopen(p, L"r");
    if (!f) return;
    char w[32] = "";
    if (fscanf(f, "%31s", w) == 1)
        g_dist_mode = !strcmp(w, "off") ? DIST_OFF : !strcmp(w, "steady") ? DIST_STEADY : DIST_SUBTLE;
    fclose(f);
}

/* Volume of all the mod's sounds, in percent (Page Up / Page Down), kept in
 * Budokai3Access\sound_volume.txt. */
static volatile int g_vol = 100;
static void vol_path(wchar_t *p) { swprintf(p, MAX_PATH, L"%lssound_volume.txt", g_mod_dir); }
static void vol_load(void) {
    wchar_t p[MAX_PATH]; vol_path(p);
    FILE *f = _wfopen(p, L"r");
    if (!f) return;
    int v; if (fscanf(f, "%d", &v) == 1 && v >= 10 && v <= 200) g_vol = v;
    fclose(f);
}
void fight_volume(int dir) {
    int v = g_vol + 10 * dir;
    if (v < 10) v = 10;
    if (v > 200) v = 200;
    g_vol = v;
    wchar_t p[MAX_PATH]; vol_path(p);
    FILE *f = _wfopen(p, L"w");
    if (f) { fprintf(f, "%d\n", v); fclose(f); }
    speakf(1, "Sound volume %d percent", v);
}

typedef struct { float x, y, z; } V3;
static V3 rdv(uint32_t a) { V3 v = { gf(a), gf(a + 4), gf(a + 8) }; return v; }

/* Fight geometry, filled by fight_poll for the audio thread and F6. */
static CRITICAL_SECTION g_lock;
static int s_active;
static int s_round;                  /* between "Fight!" and the end of the round */
static int s_quiet;                  /* no tone: outside a round, paused, or the Ultimate minigame */
static float s_dist, s_side, s_dy;   /* horizontal distance, screen side (-1 left .. 1 right), height of opponent minus you */
static DWORD s_update;
static volatile int s_ult_on;        /* Ultimate minigame tone (below) */
static volatile float s_ult_level;

static int in_battle(void) { return task_running("[BTL] MAIN") && task_running("[PLY] MAIN"); }

/* 0 when you are player 1, 1 when player 2. The game keeps the pad that
 * drives each side at gauge block +0x3e (-1 = computer): Practice has 0 / -1,
 * a World Tournament match can have -1 / 0. With both or neither human,
 * player 1 counts as you. */
static int g_side;
static int my_side(void) {
    uint32_t b = g32(GAUGE_BASE_PTR);
    if (!b || !guest_ok(b, 0x1e0 * 2)) return 0;
    int pad0 = (int16_t)g16(b + 0x3e), pad1 = (int16_t)g16(b + 0x1e0 + 0x3e);
    return pad0 == -1 && pad1 != -1;
}
static uint32_t my_rec(void) { return g_side ? P2_REC : P1_REC; }
static uint32_t foe_rec(void) { return g_side ? P1_REC : P2_REC; }
static uint32_t my_gauge(void) { return g_side ? GAUGE_P2 : GAUGE_P1; }
static uint32_t foe_gauge(void) { return g_side ? GAUGE_P1 : GAUGE_P2; }

static int geometry(float *dist, float *side, float *dy) {
    uint32_t bw = g32(BTL_WORK_PTR);
    if (!bw || !guest_ok(bw + 0x190, 0x20)) return 0;
    V3 me = rdv(my_rec() + 0x60), foe = rdv(foe_rec() + 0x60);
    V3 cam = rdv(bw + 0x190), look = rdv(bw + 0x1a0);
    float fx = look.x - cam.x, fz = look.z - cam.z;
    float fl = sqrtf(fx * fx + fz * fz);
    float dx = foe.x - me.x, dz = foe.z - me.z;
    *dist = sqrtf(dx * dx + dz * dz);
    *dy = foe.y - me.y;
    if (fl < 1e-3f || *dist < 1e-3f) { *side = 0; return 1; }
    /* camera right = (-fz, 0, fx) / |f| */
    float sx = (dx * -fz + dz * fx) / (fl * *dist);
    *side = sx;
    return 1;
}

/* ---------- ring edge ----------
 * The arena is a square around (0, 0). Measured on the World Tournament
 * stage from real ring outs: fighters leave the floor at about 260 units
 * from the centre on either axis (and are held at 280 while falling). */
#define EDGE_HALF  260.0f
/* Only the World Tournament stage has this ring. Stage number: 823a3dc0
 * (the stage code loads file stage * 128 + 6 from it); 0 = World Tournament
 * Stage, the same numbers as stage select. Other stages have walls, or (Cell
 * Ring) a ring of a size never measured, so no edge sound there. */
#define STAGE_NUM  0x823a3dc0u
/* The World Tournament's Cell Games course leaves 823a3dc0 at 0 but has the
 * stage in the first word of the same block (8 = Cell Ring, walls at about
 * 185 units, no ring out); a normal tournament and duels keep both at 0. */
#define STAGE_NUM2 0x823a3d80u
static int ring_stage(void) { return (int)g32(STAGE_NUM) == 0 && (int)g32(STAGE_NUM2) == 0; }
#define EDGE_SOUND 150.0f      /* thump starts this close: a knockback can throw a fighter over 100 units */
static float s_edge_dist = 1e9f, s_edge_pan;        /* your nearest edge, for the audio thread */

/* Distance from p to the nearest edge, and that edge's outward direction. */
static int edge_info(V3 p, float *dist, float *nx, float *nz) {
    float half = EDGE_HALF;
    float ex = half - fabsf(p.x), ez = half - fabsf(p.z);
    if (ex < ez) { *dist = ex; *nx = p.x >= 0 ? 1.0f : -1.0f; *nz = 0; }
    else { *dist = ez; *nx = 0; *nz = p.z >= 0 ? 1.0f : -1.0f; }
    return 1;
}

/* Edge direction relative to where you face (towards the opponent). */
static const char *edge_words(float nx, float nz, V3 me, V3 foe) {
    float fx = foe.x - me.x, fz = foe.z - me.z, fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-3f) return "near you";
    float d = (nx * fx + nz * fz) / fl;
    return d < -0.5f ? "behind you" : d > 0.5f ? "ahead of you" : "to your side";
}

static void edge_poll(int active) {
    float d, nx, nz;
    V3 me = rdv(my_rec() + 0x60);
    static int logged = -1;
    if (active && logged != (int)(g32(STAGE_NUM) | g32(STAGE_NUM2) << 8)) {
        logged = (int)(g32(STAGE_NUM) | g32(STAGE_NUM2) << 8);
        log_line("fight on stage %d (block start %d)", (int)g32(STAGE_NUM), (int)g32(STAGE_NUM2));
    }
    if (!active) logged = -1;
    if (!active || !ring_stage() || !edge_info(me, &d, &nx, &nz)) {
        EnterCriticalSection(&g_lock); s_edge_dist = 1e9f; LeaveCriticalSection(&g_lock);
        return;
    }
    /* screen side of the edge, for panning */
    float pan = 0;
    uint32_t bw = g32(BTL_WORK_PTR);
    if (bw) {
        V3 cam = rdv(bw + 0x190), look = rdv(bw + 0x1a0);
        float fx = look.x - cam.x, fz = look.z - cam.z, fl = sqrtf(fx * fx + fz * fz);
        if (fl > 1e-3f) pan = (nx * -fz + nz * fx) / fl;
    }
    EnterCriticalSection(&g_lock); s_edge_dist = d; s_edge_pan = pan; LeaveCriticalSection(&g_lock);
    /* No spoken warnings during a match: the thump carries it. F6 tells the distance. */
}

void fight_poll(void) {
    float d, s, dy;
    int battle = in_battle();
    if (battle) {
        int side = my_side();
        if (side != g_side) log_line("you are player %d", side + 1);
        g_side = side;
    }
    int active = battle && geometry(&d, &s, &dy);
    EnterCriticalSection(&g_lock);
    s_active = active;
    s_quiet = !s_round || im_tactics_on() || task_running("[FIX]PSDRW") || s_ult_on || task_running("[FIX]DRUSH")
              || task_running("[PLY] SCR INPUT") || task_running("HM CHARGE LINE")   /* scripted rush scenes, Hyper Mode dash */
              || task_running("[FIX]ULT") || task_running("[FIX]ULT DRAW");   /* the whole Ultimate, not just the presses */
    if (active) { s_dist = d; s_side = s; s_dy = dy; s_update = GetTickCount(); }
    LeaveCriticalSection(&g_lock);
    edge_poll(active);
}

/* F6: the distance in words. */
void fight_describe(void) {
    float d, s, dy;
    if (!in_battle() || !geometry(&d, &s, &dy)) { speak(L"Not in a fight", 1); return; }
    const char *how = d < 15 ? "close" : d < 40 ? "mid range" : "far";
    const char *side = s > 0.25f ? "to your right" : s < -0.25f ? "to your left" : "straight ahead";
    char buf[260];
    int n = snprintf(buf, sizeof buf, "%s, %s, %d units%s", how, side, (int)(d + 0.5f),
             dy > 4 ? ", above you" : dy < -4 ? ", below you" : "");
    float ed, nx, nz;
    V3 me = rdv(my_rec() + 0x60), foe = rdv(foe_rec() + 0x60);
    if (ring_stage() && edge_info(me, &ed, &nx, &nz))
        n += snprintf(buf + n, sizeof buf - n, ". Edge %s, %d units", edge_words(nx, nz, me, foe), (int)(ed + 0.5f));
    if (ring_stage() && edge_info(foe, &ed, &nx, &nz) && ed < EDGE_SOUND)
        snprintf(buf + n, sizeof buf - n, ". Opponent %d units from the edge", (int)(ed + 0.5f));
    speak_utf8(buf, 1);
}

void fight_toggle_beeps(void) {
    g_beeps_on = !g_beeps_on;
    speak(g_beeps_on ? L"Guide beep on" : L"Guide beep off", 1);
}

/* Shift+F6 in a fight or menu: subtle hum, steady tone, off, and round. */
void fight_cycle_distance(void) {
    g_dist_mode = g_dist_mode == DIST_SUBTLE ? DIST_STEADY : g_dist_mode == DIST_STEADY ? DIST_OFF : DIST_SUBTLE;
    static const char *const names[] = { "off", "subtle", "steady" };
    wchar_t p[MAX_PATH]; dist_path(p);
    FILE *f = _wfopen(p, L"w");
    if (f) { fprintf(f, "%s\n", names[g_dist_mode]); fclose(f); }
    speak(g_dist_mode == DIST_SUBTLE ? L"Distance sound: subtle hum"
          : g_dist_mode == DIST_STEADY ? L"Distance sound: steady tone" : L"Distance sound off", 1);
}

/* ---------- health and ki ----------
 * Gauge blocks, one per side, 0x1e0 apart: +0 max health, +4 health,
 * +8 max ki (7000 = 7 bars), +0xc ki. Found by matching health loss against
 * the combo damage counter (fighter record +0x1a2c). */
typedef struct { int hp, hpmax, ki, kimax; } Gauge;
static int gauge(uint32_t a, Gauge *g) {
    g->hpmax = (int)g32(a); g->hp = (int)g32(a + 4); g->kimax = (int)g32(a + 8); g->ki = (int)g32(a + 12);
    return g->hpmax > 0 && g->kimax > 0 && g->hp >= 0 && g->hp <= g->hpmax * 2;
}
static int pct(int v, int max) { return max > 0 ? (v * 100 + max / 2) / max : 0; }

/* F8: health and ki of both fighters. */
void fight_status(void) {
    Gauge a, b;
    if (!in_battle() || !gauge(my_gauge(), &a) || !gauge(foe_gauge(), &b)) { speak(L"Not in a fight", 1); return; }
    char buf[200];
    snprintf(buf, sizeof buf, "You: health %d percent, ki %.1f bars. Opponent: health %d percent, ki %.1f bars.",
             pct(a.hp, a.hpmax), a.ki / 1000.0, pct(b.hp, b.hpmax), b.ki / 1000.0);
    speak_utf8(buf, 1);
}

/* Spoken warnings when your health crosses half and a quarter. */
static int g_hp_band = 2;
static void health_poll(void) {
    Gauge a;
    if (!in_battle() || !gauge(my_gauge(), &a)) { g_hp_band = 2; return; }
    if (!s_round) return;            /* only while a round is on, not on the screens after it */
    int p = pct(a.hp, a.hpmax);
    int band = p > 50 ? 2 : p > 25 ? 1 : 0;
    if (band < g_hp_band) speakf(0, band == 1 ? "Health below half" : "Health low, %d percent", p);
    g_hp_band = band;
}

/* ---------- sound ---------- */
#define RATE   44100
#define FRAMES 256
#define NBUF   8          /* 46 ms queued: 4 (23 ms) could run dry while the game is busy and crackle */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct { float freq, freq_end, pos, len, gain, pan, phase; } Voice;

static void voice_start(Voice *v, float f0, float f1, float seconds, float gain, float pan) {
    v->freq = f0; v->freq_end = f1; v->pos = 0; v->len = seconds * RATE;
    v->gain = gain; v->pan = pan; v->phase = 0;
}

static void voice_mix(Voice *v, float *l, float *r) {
    if (v->pos >= v->len) return;
    float t = v->pos / v->len;
    float f = v->freq + (v->freq_end - v->freq) * t;
    float env = t < 0.08f ? t / 0.08f : (1.0f - t) * (1.0f - t) / (0.92f * 0.92f);
    float smp = sinf(v->phase) * env * v->gain;
    v->phase += 6.2831853f * f / RATE;
    if (v->phase > 6.2831853f) v->phase -= 6.2831853f;
    float a = (v->pan + 1.0f) * 0.7853982f;
    *l += smp * cosf(a);
    *r += smp * sinf(a);
    v->pos += 1;
}

/* Ultimate meter tone: a continuous tone whose pitch follows a 0..100 meter. */
static float s_ult_phase, s_ult_freq, s_ult_gain;

static void ult_mix(float *l, float *r) {
    float target_gain = s_ult_on ? 0.16f : 0.0f;
    float target_freq = 220.0f * powf(2.0f, s_ult_level / 100.0f * 2.0f);   /* two octaves over the sweep */
    s_ult_gain += (target_gain - s_ult_gain) * 0.002f;
    /* follow the pitch quickly, but snap down when the meter wraps */
    if (target_freq < s_ult_freq * 0.6f) s_ult_freq = target_freq;
    else s_ult_freq += (target_freq - s_ult_freq) * 0.02f;
    if (s_ult_gain < 1e-4f) return;
    float smp = (sinf(s_ult_phase) + 0.3f * sinf(2 * s_ult_phase)) * s_ult_gain;
    s_ult_phase += 6.2831853f * s_ult_freq / RATE;
    if (s_ult_phase > 6.2831853f) s_ult_phase -= 6.2831853f;
    *l += smp; *r += smp;
}

/* Extra tones other modules can play (meters), mixed with the beeps. */
static Voice g_extra[4];
void fight_tone(int slot, float f0, float f1, float seconds, float gain, float pan) {
    if (slot < 0 || slot > 3) return;
    EnterCriticalSection(&g_lock);
    voice_start(&g_extra[slot], f0, f1, seconds, gain, pan);
    LeaveCriticalSection(&g_lock);
}

static volatile int g_audio_stop;
static HANDLE g_audio_thread;

static DWORD WINAPI audio_thread(LPVOID unused) {
    (void)unused;
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = RATE * 4;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        log_line("audio: could not open the sound device; distance beeps are off");
        return 0;
    }
    static short bufs[NBUF][FRAMES * 2];
    WAVEHDR hdr[NBUF];
    memset(hdr, 0, sizeof hdr);
    for (int i = 0; i < NBUF; i++) {
        memset(bufs[i], 0, sizeof bufs[i]);
        hdr[i].lpData = (LPSTR)bufs[i]; hdr[i].dwBufferLength = sizeof bufs[i];
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    log_line("audio: distance indicator ready");
    Voice chirp = { 0 }, thump = { 0 };
    double until_chirp = 0, until_thump = 0;
    float t_phase = 0, t_freq = 440, t_gain = 0, t_pan = 0, t_reed = 0;
    int t_left = 0, was_tone = 0;
    while (!g_audio_stop) {
        WaitForSingleObject(ev, 100);
        for (int i = 0; i < NBUF; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            int active, live, tone, mode = g_dist_mode; float dist, side, dy, edge, epan;
            EnterCriticalSection(&g_lock);
            active = s_active && game_has_focus() && GetTickCount() - s_update < 500;
            live = active && !s_quiet;               /* a round is on: edge knock */
            tone = live && mode != DIST_OFF;         /* distance hum or tone, height chirp */
            dist = s_dist; side = s_side; dy = s_dy; edge = s_edge_dist; epan = s_edge_pan;
            float d = clampf(dist, 8.0f, 130.0f), close = 1.0f - (d - 8.0f) / 122.0f;
            float want_freq, want_gain;
            if (mode == DIST_STEADY) {
                /* steady tone (Tekken 3 Access): 880 Hz touching (8 units),
                 * two octaves down to 220 Hz far (130 units) */
                want_freq = 880.0f * powf(2.0f, -2.0f * (d - 8.0f) / 122.0f);
                want_gain = tone ? 0.13f : 0.0f;
            } else {
                /* subtle hum (Soulcalibur II Access): a soft sine, 110 Hz far
                 * up to 330 Hz touching, and quieter the farther away */
                want_freq = 110.0f * powf(3.0f, close);
                want_gain = tone ? 0.025f + 0.055f * close : 0.0f;
            }
            int smooth_only = mode != DIST_STEADY;   /* the hum has no reedy left side; pan says it */
            if (tone != was_tone) { was_tone = tone; log_line("distance tone %s", tone ? "on" : "off"); }
            float want_pan = dist < 3.0f ? 0.0f : clampf(side * 0.6f, -0.6f, 0.6f);
            if (side < -0.15f) t_left = 1; else if (side > 0.15f) t_left = 0;   /* timbre side, with some slack */
            short *out = bufs[i];
            for (int n = 0; n < FRAMES; n++) {
                float l = 0, r = 0;
                t_freq += (want_freq - t_freq) * 0.0015f;
                t_gain += (want_gain - t_gain) * 0.0008f;
                t_pan += (want_pan - t_pan) * 0.0015f;
                t_reed += ((t_left && !smooth_only ? 1.0f : 0.0f) - t_reed) * 0.0005f;   /* blend the timbres: a hard switch clicked */
                if (t_gain > 1e-4f) {
                    float ph = t_phase;
                    float reedy = (sinf(ph) + sinf(3 * ph) / 3 + sinf(5 * ph) / 5 + sinf(7 * ph) / 7) * 0.8f;  /* odd harmonics */
                    float smooth = sinf(ph) + 0.15f * sinf(2 * ph);
                    float smp = (reedy * t_reed + smooth * (1.0f - t_reed)) * t_gain;
                    float pa = (t_pan + 1.0f) * 0.7853982f;
                    l += smp * cosf(pa); r += smp * sinf(pa);
                }
                t_phase += 6.2831853f * t_freq / RATE;
                if (t_phase > 6.2831853f) t_phase -= 6.2831853f;
                if (active) {
                    /* height: a soft chirp from the opponent's side, sliding up
                     * when they are above you, down when below */
                    if (tone && (dy > 4.0f || dy < -4.0f) && until_chirp <= 0) {
                        float f0 = mode == DIST_STEADY ? want_freq * 1.5f : want_freq * 2.0f;
                        voice_start(&chirp, f0, dy > 0 ? f0 * 1.5f : f0 / 1.5f, 0.09f,
                                    mode == DIST_STEADY ? 0.08f : 0.045f, want_pan);
                        until_chirp = 0.7 * RATE;
                    }
                    /* ring edge: a short, loud knock from the edge's side (a quick pitch
                     * drop), about 2 a second at the outer range up to 14 a second at
                     * the very edge, and louder the closer you are */
                    if (live && edge < EDGE_SOUND && until_thump <= 0) {
                        float closeness = 1.0f - clampf(edge / EDGE_SOUND, 0, 1);
                        voice_start(&thump, 240.0f + 80.0f * closeness, 110.0f, 0.045f, 0.60f + 0.30f * closeness, clampf(epan, -1, 1) * 0.9f);
                        until_thump = (0.07 + 0.43 * (1.0f - closeness)) * RATE;
                    }
                } else { until_chirp = 0; until_thump = 0; }
                until_chirp -= 1; until_thump -= 1;
                voice_mix(&chirp, &l, &r);
                voice_mix(&thump, &l, &r);
                for (int k = 0; k < 4; k++) voice_mix(&g_extra[k], &l, &r);
                ult_mix(&l, &r);
                float vol = g_vol / 100.0f;
                out[2 * n] = (short)(clampf(l * vol, -1, 1) * 32000);
                out[2 * n + 1] = (short)(clampf(r * vol, -1, 1) * 32000);
            }
            LeaveCriticalSection(&g_lock);
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
        }
    }
    waveOutReset(wo);
    for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo);
    CloseHandle(ev);
    return 0;
}

/* ---------- ultimate attack timing game ----------
 * Task "[FIX]ULT DRAW" work data while an Ultimate is being decided:
 *   +0x04 phase: 0 intro, 1 pressing window (4 seconds), 2 result
 *   +0x20 / +0x24 player 1 / player 2 meter, sweeping 0..100 and wrapping
 *   +0x28 / +0x2c score (float): sum of the meter values at each press
 *   +0x30 / +0x32 presses used (16 bit); each side gets 3
 * The best press is just before the meter wraps back to 0. The meter does
 * not stop for presses; it sweeps 0..100 in about 0.57 s (176 a second,
 * measured), so a cue at the top comes too late to react to: hearing it,
 * pressing and the game seeing the press take about a quarter second, some
 * 45 points of meter. The mod plays a tone that rises with your meter and
 * a soft beat each time it starts over. Pressing as the tone tops out works
 * best (user: 290-299 in Practice); an early "press now" tick was tried and
 * dropped, as timing the rhythm beat reacting to a tick. */
static int g_ult_phase = -1, g_ult_presses;
static float g_ult_prev, g_ult_score;   /* score before the latest press, for the log */

static void ult_poll(void) {
    uint32_t w = task_work("[FIX]ULT DRAW");
    if (!w || !guest_ok(w, 0x40)) {
        if (g_ult_phase >= 0) { s_ult_on = 0; g_ult_phase = -1; }
        return;
    }
    int phase = (int)g32(w + 4);
    float me = gf(w + (g_side ? 0x24 : 0x20));
    int presses = g16(w + (g_side ? 0x32 : 0x30));
    if (phase != g_ult_phase) {
        if (phase == 0 && g_ult_phase < 0) speak(L"Ultimate! Press as the tone tops out, 3 presses", 1);
        if (phase == 1) { g_ult_presses = 0; g_ult_prev = 0; g_ult_score = 0; }
        if (phase == 2) {
            s_ult_on = 0;
            int a = (int)(gf(w + (g_side ? 0x2c : 0x28)) + 0.5f), b = (int)(gf(w + (g_side ? 0x28 : 0x2c)) + 0.5f);
            speakf(1, "You %d, opponent %d. %s", a, b, a > b ? "You win the ultimate" : a < b ? "Opponent wins the ultimate" : "Tie");
        }
        g_ult_phase = phase;
    }
    if (phase != 1) return;
    if (presses != g_ult_presses) {
        float score = gf(w + (g_side ? 0x2c : 0x28));
        if (presses > 0) log_line("ultimate press %d: %.0f points", presses, score - g_ult_score);
        g_ult_score = score;
        g_ult_presses = presses;
        fight_tone(1, 660, 660, 0.03f, 0.18f, 0);       /* press registered */
    }
    int active = presses < 3;   /* plays whatever the distance sound setting */
    s_ult_on = active;
    s_ult_level = me;
    if (active && me < g_ult_prev - 30)
        fight_tone(2, 330, 330, 0.06f, 0.18f, 0);         /* meter started over */
    g_ult_prev = me;
}

/* Dragon Rush, Fusion and Potara picks are actions, not buttons, so they are
 * named as actions: short to say, and right on any controller, layout or
 * keyboard (user request). */
static const char *action_button(int mask) {
    return mask == 1 ? "Punch" : mask == 2 ? "Kick" : mask == 4 ? "Guard" : mask == 8 ? "Ki" : "?";
}

/* ---------- Dragon Rush ----------
 * Task "[FIX]DRUSH" keeps its data inline in the task record from +0x30
 * (game code 820efbf8, picks in 820eea00). Offsets from there:
 *   +0x04 phase: 0 start, 1 choosing (about 3 seconds), 5 reveal, 2/3/4 on
 *         to the next exchange, 6 over
 *   +0x18 buttons the attacker has used (16 bit mask); nobody may pick them
 *   +0x1a who has picked this exchange (1 attacker, 2 defender)
 *   +0x1c / +0x1e attacker's / defender's pick as an action mask (Punch 1,
 *         Kick 2, Guard 4, Ki 8: X, Y, A, B on the default layout)
 *   +0x20 / +0x22 attacker's / defender's player number
 *   +0x24.. attacker's picks so far, +0x2a.. defender's, +0x32 exchange (0..2)
 * The defender stops the rush by picking the same button as the attacker. */
static const char *dr_button(int m) { return m ? action_button(m) : "nothing"; }
static int g_dr_phase = -1, g_dr_mine;

static void drush_poll(void) {
    uint32_t t = task_rec("[FIX]DRUSH");
    if (!t || !guest_ok(t + 0x30, 0x40)) { g_dr_phase = -1; return; }
    uint32_t w = t + 0x30, b = g32(GAUGE_BASE_PTR);
    int phase = (int)g32(w + 4);
    int me_id = b && guest_ok(b + 0xf20, 4) ? (int8_t)g8(b + 0xf20 + (uint32_t)g_side) : g_side;
    int attack = (int16_t)g16(w + 0x20) == me_id;
    int my_slot = attack ? 0 : 1;
    if (g_dr_phase < 0) speak(attack ? L"Dragon Rush! You attack" : L"Dragon Rush! You defend", 1);
    if (phase != g_dr_phase) {
        if (phase == 1) {
            int used = g16(w + 0x18), round = (int16_t)g16(w + 0x32);
            char buf[120];
            int n = snprintf(buf, sizeof buf, "%s %d:", attack ? "Attack" : "Guess", round + 1);
            for (int m = 1; m <= 8; m <<= 1)
                if (!(used & m)) n += snprintf(buf + n, sizeof buf - n, " %s", dr_button(m));
            speak_utf8(buf, 1);
            g_dr_mine = 0;
        } else if (phase == 5) {
            int a = g16(w + 0x1c), d = g16(w + 0x1e);
            int mine = attack ? a : d, theirs = attack ? d : a;
            const char *res = a == d ? (attack ? "Blocked!" : "Blocked it!") : (attack ? "Hit!" : "Hit, you guessed wrong");
            speakf(1, "You %s, opponent %s. %s", dr_button(mine), dr_button(theirs), res);
        } else if (phase == 6) {
            /* over: the last reveal said how it ended */
        }
        g_dr_phase = phase;
    }
    if (phase == 1 && !g_dr_mine && (g16(w + 0x1a) & (1 << my_slot))) {
        g_dr_mine = 1;
        fight_tone(1, 880, 880, 0.04f, 0.2f, 0);     /* your pick registered */
    }
}

/* ---------- Fusion command ----------
 * A fusion skill (Fusion <Gotenks>, <Gogeta>...) used in Hyper Mode shows a
 * row of five buttons to press in order before a timer runs out (tasks
 * "[FIX]CMD" / "[FIX]CMDDRAW"; the row is a picture the text reader cannot
 * read). Data is the work of [FIX]CMDDRAW:
 *   +0x04 phase: 0 intro (about 1 s), 1 pressing (3 s), 2 result, 3 over
 *   +0x08 frame counter of the phase (float), +0x0c / +0x10 intro / window length
 *   +0x14.. five buttons, s32 each, as action masks: Punch 1, Kick 2, Guard 4, Ki 8
 *   +0x28 1 = failed (wrong button or too slow), +0x2c buttons pressed right
 * The sequence is random each time and is set from the start of the intro;
 * presses during the intro are ignored (neither counted nor a mistake).
 * Success gives the fused fighter, failure a weak one (fat Gotenks).
 * "[MDG]SKL 1P" / "2P" (the skill name banner) says whose fusion it is. */
static int g_cmd_phase = -1, g_cmd_mine, g_cmd_done;
static float g_cmd_t;   /* frames into the pressing window */

static void fusion_poll(void) {
    uint32_t w = task_work("[FIX]CMDDRAW");
    if (!w || !guest_ok(w, 0x30)) { g_cmd_phase = -1; return; }
    int phase = (int)g32(w + 4);
    if (g_cmd_phase < 0) {
        if (!g32(w + 0x14)) return;     /* sequence not set yet */
        int owner = task_running("[MDG]SKL 2P") ? 1 : task_running("[MDG]SKL 1P") ? 0 : g_side;
        g_cmd_mine = owner == g_side;
        g_cmd_done = 0;
        if (g_cmd_mine) {
            char buf[160]; int n = snprintf(buf, sizeof buf, "Fusion! Press");
            for (int i = 0; i < 5 && g32(w + 0x14 + 4 * (uint32_t)i); i++)
                n += snprintf(buf + n, sizeof buf - n, "%s %s", i ? "," : "", action_button((int)g32(w + 0x14 + 4 * (uint32_t)i)));
            speak_utf8(buf, 1);
        } else {
            speak(L"Opponent is fusing", 1);
        }
    }
    int done = (int)g32(w + 0x2c);
    if (phase == 1) g_cmd_t = gf(w + 8);
    if (g_cmd_mine && phase == 1 && g_cmd_phase == 0)
        fight_tone(2, 392, 392, 0.08f, 0.22f, 0);      /* go: presses during the intro are ignored */
    if (g_cmd_mine && (phase == 1 || phase == 2) && done > g_cmd_done && done <= 5) {
        float f = 523.25f * powf(1.12246f, (float)(done - 1));    /* a step up the scale per right button */
        fight_tone(1, f, f, 0.05f, 0.2f, 0);
    }
    g_cmd_done = done;
    if (phase != g_cmd_phase && phase == 2 && g_cmd_phase >= 0) {
        int failed = (int)g32(w + 0x28) != 0;
        if (!g_cmd_mine) speak(failed ? L"Opponent's fusion failed" : L"Opponent fused", 1);
        else if (!failed) speak(L"Fusion success!", 1);
        else if (g_cmd_t >= gf(w + 0x10) - 2) speakf(1, "Too slow, fusion failed. %d of 5", done);
        else speakf(1, "Wrong button, fusion failed. %d of 5", done);
    }
    g_cmd_phase = phase;
}

/* ---------- Potara ----------
 * Potara skills (Vegito, Kibitoshin) used in Hyper Mode start a guessing
 * game like a Dragon Rush exchange: the fusing fighter picks a button, the
 * other side tries to pick the same one. Tasks "[FIX]RLTCMD" / "[FIX]RCM DRAW";
 * data is the work of [FIX]RCM DRAW (game code 820f09e0):
 *   +0x04 phase: 0 intro (80 frames, picks ignored), 1 picking (120 frames),
 *         2 both picks shown, 3 result
 *   +0x1a who has picked (1 the fuser, 2 the guesser), +0x1c / +0x1e the
 *   fuser's / guesser's pick (action masks), +0x20 / +0x22 their player numbers
 *   +0x34 set to 1 when the guess stops the fusion: same button, or the
 *         fuser picked nothing and the guesser did; nobody picking = fused. */
static int g_pt_phase = -1, g_pt_mine, g_pt_fuser;

static void potara_poll(void) {
    uint32_t w = task_work("[FIX]RCM DRAW");
    if (!w || !guest_ok(w, 0x40)) { g_pt_phase = -1; return; }
    uint32_t b = g32(GAUGE_BASE_PTR);
    int phase = (int)g32(w + 4);
    int me_id = b && guest_ok(b + 0xf20, 4) ? (int8_t)g8(b + 0xf20 + (uint32_t)g_side) : g_side;
    int fuser = (int16_t)g16(w + 0x20) == me_id;
    if (g_pt_phase < 0) {
        g_pt_fuser = fuser; g_pt_mine = 0;
        /* short: the whole game is about 3 seconds; picks count from the beep */
        speak(fuser ? L"Potara! Pick a button, opponent guesses" : L"Opponent's Potara! Guess their button", 1);
    }
    if (phase == 1 && g_pt_phase == 0) fight_tone(2, 392, 392, 0.08f, 0.22f, 0);    /* go */
    if (phase == 1 && !g_pt_mine && (g16(w + 0x1a) & (fuser ? 1 : 2))) {
        g_pt_mine = 1;
        fight_tone(1, 880, 880, 0.04f, 0.2f, 0);     /* your pick registered */
    }
    if (phase != g_pt_phase && phase == 2 && g_pt_phase >= 0) {
        int a = g16(w + 0x1c), d = g16(w + 0x1e), stopped = g16(w + 0x34) != 0;
        int mine = fuser ? a : d, theirs = fuser ? d : a;
        const char *res = fuser ? (stopped ? "Guessed, Potara failed" : "Potara success!")
                                : (stopped ? "You stopped the Potara!" : "Opponent fused");
        speakf(1, "You %s, opponent %s. %s", dr_button(mine), dr_button(theirs), res);
    }
    g_pt_phase = phase;
}

/* ---------- fight announcements ---------- */
/* 820deac8 creates the on-screen fight message; its task is named
 * "[MSG]<name>". The names below are what is spoken. */
static const struct { const char *task, *say; } k_msgs[] = {
    { "[MSG]Ready", "Ready" }, { "[MSG]Fight", "Fight!" }, { "[MSG]KO", "K O" },
    { "[MSG]DoubleKO", "Double K O" }, { "[MSG]Perfect", "Perfect" }, { "[MSG]Finish", "Finish" },
    { "[MSG]TimeUp", "Time up" }, { "[MSG]Victory", "Victory" }, { "[MSG]RingOut", "Ring out" },
    { "[MSG]Win00", "Player 1 wins" }, { "[MSG]Win01", "Player 2 wins" }, { "[MSG]Lose", "You lose" },
    { "[MSG]Draw", "Draw" }, { "[MSG]Miss", "Miss" }, { "[MSG]Gameset00", "Game set" },
    { "[MSG]Gameset01", "Game set" }, { "[MSG]Success00", "Success" }, { "[MSG]Success01", "Success" },
    { "[MSG]Clear00", "Clear" }, { "[MSG]Clear01", "Clear" },
    { "[MSG]GBREAK 1P", "Guard break on you" }, { "[MSG]GBREAK 2P", "Guard break" },
    { "[MSG]CNTR 1P", "Counter" }, { "[MSG]CNTR 2P", "Opponent counters" },
    { "[MSG]FATK 1P", "First attack" }, { "[MSG]FATK 2P", "Opponent first attack" },
};
static int g_msg_seen[sizeof k_msgs / sizeof k_msgs[0]];

/* Messages that end a round (the tone stops). */
static const char *const k_round_end[] = {
    "[MSG]KO", "[MSG]DoubleKO", "[MSG]Finish", "[MSG]TimeUp", "[MSG]RingOut", "[MSG]Victory",
    "[MSG]Win00", "[MSG]Win01", "[MSG]Lose", "[MSG]Draw", "[MSG]Ready",
};

static void announce_poll(void) {
    for (unsigned i = 0; i < sizeof k_msgs / sizeof k_msgs[0]; i++) {
        int on = task_running(k_msgs[i].task);
        if (on && !g_msg_seen[i] && !im_tactics_on()) {   /* Dragon Tactics' hidden fight says nothing */
            /* "... 1P" / "... 2P" messages are about a side; the texts are
             * written for you being player 1, so take the partner's when
             * you are player 2 (the pairs sit next to each other). */
            unsigned j = i;
            size_t n = strlen(k_msgs[i].task);
            if (g_side && n > 2 && k_msgs[i].task[n - 1] == 'P') j = i ^ 1;
            speak_utf8(k_msgs[j].say, 1);
            if (!strcmp(k_msgs[i].task, "[MSG]Fight")) s_round = 1;
            for (unsigned k = 0; k < sizeof k_round_end / sizeof k_round_end[0]; k++)
                if (!strcmp(k_msgs[i].task, k_round_end[k])) s_round = 0;
        }
        g_msg_seen[i] = on;
    }
    if (!in_battle()) s_round = 0;
}

void fight_init(void) {
    InitializeCriticalSection(&g_lock);
    vol_load();
    dist_load();
    g_audio_stop = 0;
    g_audio_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
}

void fight_shutdown(void) {
    g_audio_stop = 1;
    WaitForSingleObject(g_audio_thread, 2000);
    CloseHandle(g_audio_thread);
    DeleteCriticalSection(&g_lock);
}

void fight_poll_all(void) {
    fight_poll();
    health_poll();
    ult_poll();
    drush_poll();
    fusion_poll();
    potara_poll();
    announce_poll();
}
