/*
 * Controller state seen by the game. The port gives the recompiled game
 * Xbox pad state through __imp__XamInputGetState (r3 user, r4 flags,
 * r5 guest XINPUT_STATE*). The mod wraps that import to read the buttons the
 * player holds (for mod hotkeys) and, for automated testing, to add buttons
 * (Budokai3Access\cmd.txt: "pad up 200" holds D-pad up for 200 ms).
 */
#include "access.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

static guest_fn o_get_state;
static volatile int g_inject_on; static volatile DWORD g_inject_until;
static volatile uint16_t g_inject;
static volatile LONG g_pkt_bias;        /* added to the packet number so the game sees each change */
static volatile LONG g_pkt_bias;        /* added to the packet number so the game sees each change */
static volatile int16_t g_inject_lx, g_inject_ly, g_inject_rx, g_inject_ry;
static volatile uint8_t g_inject_lt, g_inject_rt;
volatile uint16_t g_pad_buttons;        /* player 1 buttons in the last read (after injection) */
volatile uint8_t g_pad_lt, g_pad_rt;
volatile int16_t g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;

/* Testing: keep a guest value pinned for a while, written on the game thread
 * every time it reads the pad ("pin ADDR VALUE MS" in cmd.txt). */
static volatile uint32_t g_pin_addr, g_pin_val; static volatile DWORD g_pin_until;
void pad_pin(uint32_t addr, uint32_t val, int ms) { g_pin_addr = addr; g_pin_val = val; g_pin_until = GetTickCount() + (DWORD)ms; }

static void h_get_state(PPCContext *c, uint8_t *b) {
    if (g_pin_addr && GetTickCount() < g_pin_until) w32(g_pin_addr, g_pin_val);
    uint32_t user = (uint32_t)c->r3, st = (uint32_t)c->r5;
    CALL_ORIG(o_get_state, c, b);
    if (user != 0 || (uint32_t)c->r3 != 0 || !st) return;
    { /* frame-rate log: the game reads player 1 once per frame */
        static DWORD t0; static int frames; frames++;
        DWORD now = GetTickCount();
        if (!t0) t0 = now;
        if (now - t0 >= 5000) { log_line("fps %.1f", frames * 1000.0 / (now - t0)); t0 = now; frames = 0; }
    }
    uint16_t btn = g16(st + 4);
    /* While a port screen is up the player's pad is kept from the game: test presses too. */
    if (g_inject_on && GetTickCount() < g_inject_until && !im_menu_on()) {
        btn |= g_inject;
        *(uint16_t *)gptr(st + 4) = _byteswap_ushort(btn);
        if (g_inject_lx) *(uint16_t *)gptr(st + 8) = _byteswap_ushort((uint16_t)g_inject_lx);
        if (g_inject_ly) *(uint16_t *)gptr(st + 10) = _byteswap_ushort((uint16_t)g_inject_ly);
        if (g_inject_lt) *gptr(st + 6) = g_inject_lt;
        if (g_inject_rt) *gptr(st + 7) = g_inject_rt;
        if (g_inject_rx) *(uint16_t *)gptr(st + 12) = _byteswap_ushort((uint16_t)g_inject_rx);
        if (g_inject_ry) *(uint16_t *)gptr(st + 14) = _byteswap_ushort((uint16_t)g_inject_ry);
    } else if (g_inject_on) { g_inject_on = 0; InterlockedIncrement(&g_pkt_bias); }
    if (g_pkt_bias) w32(st, g32(st) + (uint32_t)g_pkt_bias);
    g_pad_buttons = btn;
    g_pad_lt = g8(st + 6); g_pad_rt = g8(st + 7);
    g_pad_lx = (int16_t)g16(st + 8); g_pad_ly = (int16_t)g16(st + 10);
    g_pad_rx = (int16_t)g16(st + 12); g_pad_ry = (int16_t)g16(st + 14);
}

static const struct { const char *name; uint16_t bit; } k_btn[] = {
    { "up", 0x1 }, { "down", 0x2 }, { "left", 0x4 }, { "right", 0x8 }, { "start", 0x10 }, { "back", 0x20 },
    { "l3", 0x40 }, { "r3", 0x80 }, { "lb", 0x100 }, { "rb", 0x200 }, { "a", 0x1000 }, { "b", 0x2000 },
    { "x", 0x4000 }, { "y", 0x8000 },
};

void pad_inject(const char *names, int ms) {
    uint16_t m = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; uint8_t lt = 0, rt = 0;
    char buf[128]; strncpy(buf, names, 127); buf[127] = 0;
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        for (unsigned i = 0; i < sizeof k_btn / sizeof k_btn[0]; i++) if (!strcmp(t, k_btn[i].name)) m |= k_btn[i].bit;
        if (!strcmp(t, "lsup")) ly = 32000; else if (!strcmp(t, "lsdown")) ly = -32000;
        else if (!strcmp(t, "lsleft")) lx = -32000; else if (!strcmp(t, "lsright")) lx = 32000;
        else if (!strcmp(t, "lt")) lt = 255; else if (!strcmp(t, "rt")) rt = 255;
        else if (!strcmp(t, "rsup")) ry = 32000; else if (!strcmp(t, "rsdown")) ry = -32000;
        else if (!strcmp(t, "rsleft")) rx = -32000; else if (!strcmp(t, "rsright")) rx = 32000;
    }
    g_inject_lt = lt; g_inject_rt = rt;
    g_inject = m; g_inject_lx = lx; g_inject_ly = ly; g_inject_rx = rx; g_inject_ry = ry;
    g_inject_until = GetTickCount() + (DWORD)ms;
    g_inject_on = 1;
    InterlockedIncrement(&g_pkt_bias);
    log_line("pad inject %s (%04x) for %d ms", names, m, ms);
}

/* The port's own screens (port 1.5: ONLINE, Tag Team...) read the pad through
 * rex::input::InputSystem::GetStateForUI, not the game's import; injected
 * buttons go there too. Its X_INPUT_STATE is the guest's (big-endian): u32 packet, u16 buttons. */
typedef uint32_t (*ui_state_fn)(void *self, uint32_t user, void *state);
static ui_state_fn o_ui_state;
static void *g_ui_state_target;
static uint32_t h_ui_state(void *self, uint32_t user, void *state) {
    InterlockedIncrement(&g_inflight);
    uint32_t r = o_ui_state(self, user, state);
    if (user == 0 && state && g_inject && GetTickCount() < g_inject_until) {
        if (r != 0) { memset(state, 0, 16); r = 0; }    /* testing with no pad plugged in */
        *(uint16_t *)((uint8_t *)state + 4) |= _byteswap_ushort(g_inject);
    }
    InterlockedDecrement(&g_inflight);
    return r;
}

static void ui_pad_init(void) {
    HMODULE rt = GetModuleHandleW(L"rexruntime.dll");
    g_ui_state_target = rt ? (void *)GetProcAddress(rt, "?GetStateForUI@InputSystem@input@rex@@QEAAIIPEAUX_INPUT_STATE@23@@Z") : NULL;
    if (!g_ui_state_target) return;
    if (MH_CreateHook(g_ui_state_target, (void *)h_ui_state, (void **)&o_ui_state) == MH_OK && MH_EnableHook(g_ui_state_target) == MH_OK)
        log_line("pad: port screens' pad wrapped");
    else g_ui_state_target = NULL;
}

static ULONG_PTR *g_iat_slot;

void pad_shutdown(void) {
    if (g_ui_state_target) { MH_DisableHook(g_ui_state_target); MH_RemoveHook(g_ui_state_target); g_ui_state_target = NULL; }
    if (!g_iat_slot) return;
    DWORD old;
    VirtualProtect(g_iat_slot, sizeof(void *), PAGE_READWRITE, &old);
    *g_iat_slot = (ULONG_PTR)o_get_state;
    VirtualProtect(g_iat_slot, sizeof(void *), old, &old);
    g_iat_slot = NULL;
}

void pad_init(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((uint8_t *)exe + dd.VirtualAddress);
    for (; imp->Name; imp++) {
        if (_stricmp((char *)exe + imp->Name, "rexruntime.dll")) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)((uint8_t *)exe + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)((uint8_t *)exe + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)((uint8_t *)exe + names->u1.AddressOfData);
            if (strcmp((char *)n->Name, "__imp__XamInputGetState")) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            o_get_state = (guest_fn)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)h_get_state;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            g_iat_slot = &iat->u1.Function;
            log_line("pad: XamInputGetState wrapped");
            ui_pad_init();
            return;
        }
    }
    log_line("pad: XamInputGetState import not found");
}
