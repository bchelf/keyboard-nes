#ifndef KEY_BINDINGS_H
#define KEY_BINDINGS_H

#include <stdint.h>
#include <stdbool.h>

// NES button indices (bit positions in nes_button_state)
#define NES_BTN_A       0
#define NES_BTN_B       1
#define NES_BTN_SELECT  2
#define NES_BTN_START   3
#define NES_BTN_UP      4
#define NES_BTN_DOWN    5
#define NES_BTN_LEFT    6
#define NES_BTN_RIGHT   7

#define NES_NUM_BUTTONS 8

// HID keycodes for default bindings
#define HID_KEY_A              0x04
#define HID_KEY_B              0x05
#define HID_KEY_D              0x07
#define HID_KEY_K              0x0E
#define HID_KEY_L              0x0F
#define HID_KEY_S              0x16
#define HID_KEY_W              0x1A
#define HID_KEY_BRACKET_LEFT   0x2F
#define HID_KEY_BRACKET_RIGHT  0x30

// Flash storage for key bindings
// Last 4K sector of 2MB flash = offset 0x1FF000
#define FLASH_BINDINGS_OFFSET  0x1FF000
#define FLASH_BINDINGS_MAGIC   0xNE   // validated as 0xAE below

// actual magic value (0xNE is not valid hex; use 0xAE as the sentinel)
#define FLASH_MAGIC_VALUE      0xAE

// Current active bindings (keycodes[i] = HID keycode for NES button i)
extern uint8_t g_keycodes[NES_NUM_BUTTONS];

// Load bindings from flash (or use defaults if invalid)
void key_bindings_load(void);

// Save current g_keycodes[] to flash; returns true on success
bool key_bindings_save(void);

// Enter remap mode: interactively capture new bindings from keyboard
// Returns true if remap completed, false if timed out / aborted
bool key_bindings_remap(void);

#endif // KEY_BINDINGS_H
