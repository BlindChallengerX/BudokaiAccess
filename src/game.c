/* Game-specific logic for Budokai3Access. */
#include "access.h"

/* 820deac8: creates a fight announcement (Ready, Fight, KO, ...); r3 = message ID. */
static guest_fn o_fight_msg;
static void h_fight_msg(PPCContext *c, uint8_t *b) {
    log_line("fight message %d", (int)c->r3);
    CALL_ORIG(o_fight_msg, c, b);
}

void game_init(void) {
    fight_init();
    text_init();
    guest_hook(0x820deac8, (void *)h_fight_msg, (void **)&o_fight_msg);
}

/* Controller: right stick click does F5 (with LB held: Shift+F5), left
 * stick click does F6. */
static void pad_keys(void) {
    static uint16_t prev;
    uint16_t b = g_pad_buttons, down = b & ~prev;
    prev = b;
    if (!game_has_focus()) return;
    if (down & 0x80) game_key(VK_F5, (b & 0x100) != 0, 0);
    if (down & 0x40) game_key(VK_F6, 0, 0);
}

void game_poll(void) {
    pad_keys();
    tasks_poll();
    text_poll();
    menus_poll();
    fight_poll_all();
    du_poll();
}

void game_shutdown(void) {
    fight_shutdown();
}

void game_key(int vk, int shift, int ctrl) {
    (void)shift; (void)ctrl;
    if (vk == VK_F2) text_read_all();
    else if (vk == VK_F5 && du_in_field()) du_cycle(shift ? -1 : 1);
    else if (vk == VK_F5) { if (!du_menu_read() && !menus_read_current()) text_read_all(); }
    else if (vk == VK_F6 && shift) { if (du_in_field()) fight_toggle_beeps(); else fight_cycle_distance(); }
    else if (vk == VK_F6) { if (du_in_field()) du_describe(); else fight_describe(); }
    else if (vk == VK_F8) { if (shop_open()) shop_say_money(); else if (du_in_field()) du_status(); else fight_status(); }
    else if (vk == VK_F11) tasks_dump();
    else if (vk == VK_F12 && shift) buttons_toggle();
    else if (vk == VK_F12) speak_repeat();
    else if (vk == VK_PRIOR) fight_volume(1);      /* Page Up */
    else if (vk == VK_NEXT) fight_volume(-1);      /* Page Down */
}
