#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

// Shared NES button state (active low: 0=pressed, 1=not pressed)
// Bit order: bit0=A, bit1=B, bit2=Select, bit3=Start,
//            bit4=Up, bit5=Down, bit6=Left, bit7=Right
extern volatile uint8_t nes_button_state;

// Set PIO SM reference (called from main after PIO init)
void usb_hid_set_pio(PIO pio, uint sm);

// Initialize USB HID host subsystem (unused; TinyUSB init via tusb_init)
void usb_hid_init(void);

// Returns true if at least one keyboard is currently connected
bool usb_hid_keyboard_connected(void);

// Remap mode helpers:
// Returns the HID keycode of the most recently pressed key (0 = none)
uint8_t usb_hid_get_last_pressed_key(void);

// Returns true if the given keycode is currently held down
bool usb_hid_is_key_held(uint8_t keycode);

// Clear the last-pressed key record
void usb_hid_clear_last_pressed_key(void);

#endif // USB_HID_H
