#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// TinyUSB configuration for USB HID keyboard input (host mode on the RP2350
// native USB port). CFG_TUSB_MCU / CFG_TUSB_OS are provided by the pico-sdk.

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

// --- Host mode on the native USB (not PIO-USB) ---
#define CFG_TUH_ENABLED     1
#define CFG_TUH_RPI_PIO_USB 0
#define CFG_TUH_MAX_SPEED   OPT_MODE_FULL_SPEED

#define CFG_TUH_HUB         1                        // allow a keyboard behind a hub
#define CFG_TUH_HID         4                        // HID interfaces (kbd + mouse combos)
#define CFG_TUH_DEVICE_MAX  (3 * CFG_TUH_HUB + 1)

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

// Place USB DMA-visible data in a section the controller can reach.
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#endif // _TUSB_CONFIG_H_
