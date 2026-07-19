#ifndef USBKBD_H
#define USBKBD_H

#include <stdint.h>
#include <stdbool.h>

// USB keyboard input via TinyUSB host on the RP2350 native USB port.
// Translates USB HID keycodes into the MSX key matrix (machine_keydown/keyup).

void usbkbd_init(void); // initialise the TinyUSB host stack
void usbkbd_task(void); // pump the host stack; call often (e.g. every frame)

// Menu mode: while on, keypresses are queued as menu navigation events
// (menu_input_t) instead of driving the MSX matrix.
void usbkbd_menu_mode(bool on);
int usbkbd_menu_poll(void); // next menu event (menu_input_t) or -1 if none

#endif // USBKBD_H
