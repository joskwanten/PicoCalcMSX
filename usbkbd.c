#include "usbkbd.h"
#include "machine.h" // machine_keydown / machine_keyup
#include "menu.h"     // menu_input_t

#include "tusb.h"

// --- menu-mode navigation event queue ---
static volatile bool g_menu_mode = false;
#define MENU_Q 8
static volatile int g_mq[MENU_Q];
static volatile int g_mq_head = 0, g_mq_tail = 0;

static void menu_push(int ev)
{
    int nh = (g_mq_head + 1) % MENU_Q;
    if (nh != g_mq_tail) { g_mq[g_mq_head] = ev; g_mq_head = nh; }
}

static void menu_key(uint8_t hid)
{
    int ev = -1;
    switch (hid) {
    case 0x52: ev = MENU_UP; break;                 // Up
    case 0x51: ev = MENU_DOWN; break;               // Down
    case 0x28: case 0x58: case 0x2C: ev = MENU_ENTER; break; // Enter, KP-Enter, Space
    case 0x29: case 0x2A: ev = MENU_BACK; break;    // Esc, Backspace
    default: break;
    }
    if (ev >= 0) menu_push(ev);
}

void usbkbd_menu_mode(bool on) { g_menu_mode = on; }

int usbkbd_menu_poll(void)
{
    if (g_mq_tail == g_mq_head) return -1;
    int ev = g_mq[g_mq_tail];
    g_mq_tail = (g_mq_tail + 1) % MENU_Q;
    return ev;
}

// --- MSX key-matrix indices (row*8 + col) ---
#define MSX_SHIFT 48
#define MSX_CTRL  49
#define MSX_GRAPH 50
#define MSX_ESC   58
#define MSX_TAB   59
#define MSX_BS    61
#define MSX_ENTER 63
#define MSX_SPACE 64
#define MSX_DEL   67
#define MSX_LEFT  68
#define MSX_UP    69
#define MSX_DOWN  70
#define MSX_RIGHT 71

// USB HID keycode -> (MSX matrix index + 1); 0 = unmapped. Physical-key mapping:
// modifiers (shift/ctrl/graph) are handled separately, so the same physical key
// maps regardless of shift, exactly like a real MSX keyboard.
static uint8_t hid_to_msx[256];

static void build_map(void)
{
    for (int i = 0; i < 26; i++) hid_to_msx[0x04 + i] = (uint8_t)(22 + i) + 1; // a-z -> 22..47
    for (int i = 0; i < 9; i++)  hid_to_msx[0x1E + i] = (uint8_t)(1 + i) + 1;  // 1-9 -> 1..9
    hid_to_msx[0x27] = 0 + 1;                                                  // 0 -> 0

    hid_to_msx[0x28] = MSX_ENTER + 1; // Enter
    hid_to_msx[0x29] = MSX_ESC + 1;   // Escape
    hid_to_msx[0x2A] = MSX_BS + 1;    // Backspace
    hid_to_msx[0x2B] = MSX_TAB + 1;   // Tab
    hid_to_msx[0x2C] = MSX_SPACE + 1; // Space
    hid_to_msx[0x4C] = MSX_DEL + 1;   // Delete Forward

    // Symbols (best effort; the MSX layout differs from a US PC keyboard, refine after test)
    hid_to_msx[0x2D] = 10 + 1; // -
    hid_to_msx[0x33] = 15 + 1; // ;
    hid_to_msx[0x36] = 18 + 1; // ,
    hid_to_msx[0x37] = 19 + 1; // .
    hid_to_msx[0x38] = 20 + 1; // /

    for (int i = 0; i < 5; i++) hid_to_msx[0x3A + i] = (uint8_t)(53 + i) + 1; // F1-F5 -> 53..57

    hid_to_msx[0x4F] = MSX_RIGHT + 1;
    hid_to_msx[0x50] = MSX_LEFT + 1;
    hid_to_msx[0x51] = MSX_DOWN + 1;
    hid_to_msx[0x52] = MSX_UP + 1;
}

static void set_key(uint8_t hid, bool down)
{
    uint8_t m = hid_to_msx[hid];
    if (!m) return;
    if (down) machine_keydown(m - 1);
    else machine_keyup(m - 1);
}

static void diff_mod(uint8_t oldm, uint8_t newm, uint8_t mask, uint32_t msx)
{
    bool o = (oldm & mask) != 0, n = (newm & mask) != 0;
    if (n && !o) machine_keydown(msx);
    else if (!n && o) machine_keyup(msx);
}

static bool contains(const uint8_t *kc, uint8_t v)
{
    if (!v) return false;
    for (int i = 0; i < 6; i++)
        if (kc[i] == v) return true;
    return false;
}

static void process_report(const hid_keyboard_report_t *r)
{
    static uint8_t prev_kc[6];
    static uint8_t prev_mod;

    if (g_menu_mode) {
        // Only newly-pressed keys generate menu events.
        for (int i = 0; i < 6; i++) {
            uint8_t k = r->keycode[i];
            if (k && !contains(prev_kc, k)) menu_key(k);
        }
        for (int i = 0; i < 6; i++) prev_kc[i] = r->keycode[i];
        prev_mod = r->modifier;
        return;
    }

    // Modifiers (either left or right) -> MSX Shift / Ctrl / Graph.
    diff_mod(prev_mod, r->modifier,
             KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT, MSX_SHIFT);
    diff_mod(prev_mod, r->modifier,
             KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL, MSX_CTRL);
    diff_mod(prev_mod, r->modifier,
             KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT, MSX_GRAPH);

    // Releases: keys in the previous report that are gone now.
    for (int i = 0; i < 6; i++) {
        uint8_t k = prev_kc[i];
        if (k && !contains(r->keycode, k)) set_key(k, false);
    }
    // Presses: keys new in this report.
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keycode[i];
        if (k && !contains(prev_kc, k)) set_key(k, true);
    }

    for (int i = 0; i < 6; i++) prev_kc[i] = r->keycode[i];
    prev_mod = r->modifier;
}

// --- TinyUSB host callbacks ---

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT); // forceer boot-report (8 bytes)
        tuh_hid_receive_report(dev_addr, instance);                  // start the report stream
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr;
    (void)instance;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len)
{
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
        process_report((const hid_keyboard_report_t *)report);
    tuh_hid_receive_report(dev_addr, instance); // ask for the next one
}

void usbkbd_init(void)
{
    build_map();
    tuh_init(0); // native USB, root-hub port 0
}

void usbkbd_task(void)
{
    tuh_task();
}
