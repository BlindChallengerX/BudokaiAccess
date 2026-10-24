/* Shared declarations for Budokai3Access. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define MOD_VERSION "1.1.2-nightly"   /* nightly branch: port 1.5 support, may break; stable is main */

extern wchar_t g_game_dir[MAX_PATH];   /* folder of dbz3.exe, with trailing backslash */
extern wchar_t g_mod_dir[MAX_PATH];    /* g_game_dir + "Budokai3Access\" */

void log_line(const char *fmt, ...);
char *to_utf8(const wchar_t *w);       /* malloc'd; caller frees */
wchar_t *from_utf8(const char *s);     /* malloc'd; caller frees */
int game_has_focus(void);

/* Game threads currently inside mod hooks; reload waits for zero.
 * Hooks call the original function through CALL_ORIG. */
extern volatile LONG g_inflight;
#define CALL_ORIG(o, c, b) do { InterlockedIncrement(&g_inflight); (o)(c, b); InterlockedDecrement(&g_inflight); } while (0)

/* speech.c */
void speech_init(void);
void speech_shutdown(void);
void speak(const wchar_t *text, int interrupt);
void speak_utf8(const char *text, int interrupt);
/* buttons.c: Xbox or PlayStation button names (Shift+F12) */
extern volatile int g_ps_buttons;
void buttons_load(void);
void buttons_toggle(void);
const char *btn_name(const char *xbox);
wchar_t *buttons_rewrite(const wchar_t *in);
void speakf(int interrupt, const char *fmt, ...);
void speak_repeat(void);
void speech_stop(void);

/* guest.c: the Xbox 360 memory and the recompiled game code.
 *
 * The port maps guest memory at a fixed host address (logged by the game as
 * "Guest memory arena mapped: virtual base 0x100000000"). Guest data is
 * big-endian. Recompiled functions are looked up through the table the
 * runtime keeps right after the image: 8-byte host pointers indexed by
 * (guest address - CODE_BASE) / 4. */
#define GUEST_BASE   0x100000000ULL
#define CODE_BASE    0x82080000u
#define CODE_END     0x8230EC00u
#define FTABLE       0x826D0000u

/* Register file of a recompiled function (first argument of every host
 * function). Worked out from the generated code: r3 is first, then r0, r1,
 * r2, then r4..r31 in order. */
typedef struct {
    uint64_t r3, r0, r1, r2, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15,
             r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31;
} PPCContext;
typedef void (*guest_fn)(PPCContext *ctx, uint8_t *base);

static inline uint8_t *gptr(uint32_t a) { return (uint8_t *)(GUEST_BASE + a + (a >= 0xE0000000u ? 0x1000 : 0)); }
int  guest_ok(uint32_t a, uint32_t len);           /* readable? */
uint32_t g32(uint32_t a);                           /* big-endian reads; 0 if unreadable */
uint16_t g16(uint32_t a);
uint8_t  g8(uint32_t a);
float    gf(uint32_t a);
void     w32(uint32_t a, uint32_t v);
void    *guest_host_fn(uint32_t guest);             /* host code for a guest function, or NULL */
uint32_t guest_fn_of_host(uintptr_t rip);           /* guest function containing host address, 0 if none */
int  guest_ready(void);                             /* function table filled in */
int  guest_hook(uint32_t guest, void *detour, void **orig);
void guest_hooks_apply(void);
void tools_init(void);
void tools_shutdown(void);
void tools_poll(void);                              /* RE commands from Budokai3Access\cmd.txt */

/* pad.c */
void pad_init(void);
void pad_shutdown(void);
void pad_inject(const char *names, int ms);
void pad_pin(uint32_t addr, uint32_t val, int ms);
extern volatile uint16_t g_pad_buttons;
extern volatile LONG g_pad_moves;
extern volatile uint8_t g_pad_lt, g_pad_rt;
extern volatile int16_t g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;

/* text.c */
void text_init(void);
void text_poll(void);
void text_read_all(void);
const char *text_lookup(uint32_t sum, uint32_t idx);
void text_mute_pack(uint32_t sum);
void text_mute_all(int on);
int text_visible(uint32_t sum, uint32_t idx);
#define PLACE_NAMES_EARTH 0x26da3539u
#define PLACE_NAMES_NAMEK 0xbfd36483u
extern volatile uint32_t g_place_pack;     /* place names of the planet you are on */
extern volatile DWORD g_place_pack_t;      /* when a place name was last drawn */
int text_visible_within(uint32_t sum, uint32_t idx, DWORD ms);
int skill_rows(void);
int skill_row_text(int i, char *out, int cap);
const char *cmd_word(int c);          /* skill / training command code -> words */

/* tasks.c */
void tasks_poll(void);
void tasks_dump(void);
uint32_t task_work(const char *name);
int task_running(const char *name);
uint32_t task_rec(const char *name);
int task_works(const char *name, uint32_t *out, int max);
void text_mute_pack_except(uint32_t sum, uint32_t keep_idx);
void text_mute_pack2(uint32_t sum);
void text_mute_list(uint32_t sum);
int sysq_open(void);           /* menus.c: a save or storage Yes/No question is up */

/* imgui.c: text of the port's own ImGui screens (port 1.5 and later) */
#define IM_MAX 384
#define IM_TEXT 400
typedef struct { char s[IM_TEXT]; float x, y, size; uint32_t col; void *list; } ImText;
void im_init(void);
void im_shutdown(void);
int im_ready(void);
int im_frame(ImText *out, int max, LONG *frame_no);
void im_dump(void);
void im_poll(void);
void im_mute(int on);
const char *im_row(void);
int im_row_on(void);
int im_menu_on(void);
int im_port_recent(DWORD ms);
int im_any_text(void);
const char *im_find(const char *prefix);
void im_dialog_poll(void);   /* the port's ISO file dialog: bring it forward */
int im_read_all(int interrupt);

/* roster.c: characters and capsules added by roster mods (mods\_rosteroster.toml) */
const char *roster_char_name(int id);
const char *roster_capsule_name(int id);
int roster_wheel_cell(uint32_t work);
const char *roster_cell_name(uint32_t work);

/* menus.c */
void menus_poll(void);
int menus_read_current(void);
int shop_open(void);
void shop_say_money(void);

/* fight.c */
void fight_init(void);
void fight_shutdown(void);
void fight_poll_all(void);
void fight_describe(void);
void fight_status(void);
void fight_toggle_beeps(void);      /* Dragon Universe guide beep on / off */
void fight_cycle_distance(void);    /* fight distance sound: subtle hum, steady tone, off */
void fight_tone(int slot, float f0, float f1, float seconds, float gain, float pan);
void fight_volume(int dir);   /* +1 / -1: all mod sounds 10 percent louder / quieter */

/* du.c: Dragon Universe map */
int du_in_field(void);
void du_poll(void);
void du_cycle(int dir);
void du_describe(void);
void du_status(void);
int du_menu_read(void);

/* health.c (selfcheck_*): self checks against port changes */
void selfcheck_log_versions(void);
void selfcheck_fail(const char *what);
int selfcheck_failed(void);
void selfcheck_poll(void);
int guest_hooks_bad(void);

/* game modules */
void game_shutdown(void);
void game_init(void);
void game_poll(void);
void game_key(int vk, int shift, int ctrl);
