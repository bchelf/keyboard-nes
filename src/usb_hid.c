#include "usb_hid.h"
#include "key_bindings.h"
#include "led.h"

#include "tusb.h"
#include "hardware/pio.h"
#include "nes_pio.pio.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

volatile uint8_t nes_button_state = 0xFF; // all unpressed

// PIO reference set by main after SM init
static PIO  s_pio = NULL;
static uint s_sm  = 0;

// Keyboard connection tracking
static int  s_keyboard_count = 0;  // number of connected HID keyboards
static bool s_dev_mounted[CFG_TUH_HID] = {false};

// Remap mode key tracking
static uint8_t s_last_pressed_key = 0;
static uint8_t s_held_keys[6]     = {0};  // boot report has 6 key slots

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void push_to_pio(uint8_t state) {
    if (s_pio == NULL) return;
    // The PIO uses OUT PINDIRS to implement open-drain:
    //   PIO bit=1 -> pindir=output -> drives GP4 low -> NES sees 0 (pressed)
    //   PIO bit=0 -> pindir=input  -> GP4 hi-Z       -> NES sees 1 (not pressed)
    //
    // nes_button_state uses active-low (0=pressed, 1=not pressed).
    // We need to invert before pushing so that pressed(0)->PIO(1).
    uint8_t pio_byte = ~state;

    // Write non-blocking (drop if FIFO full - stale data is fine)
    pio_sm_put(s_pio, s_sm, (uint32_t)pio_byte);
}

// Update nes_button_state from a 6-byte boot report keycode array
static void process_boot_report(const uint8_t keycodes[6]) {
    uint8_t new_state = 0xFF; // start with all unpressed

    // Check each pressed key against our bindings
    for (int k = 0; k < 6; k++) {
        uint8_t kc = keycodes[k];
        if (kc == 0x00 || kc >= 0x04) {
            if (kc == 0x00) continue; // no key in this slot
        } else {
            continue; // error codes (1=rollover, 2=post fail, 3=undefined)
        }

        // Update remap tracking
        if (kc != 0x00) {
            // Check if this key just appeared (not in previous held)
            bool was_held = false;
            for (int j = 0; j < 6; j++) {
                if (s_held_keys[j] == kc) { was_held = true; break; }
            }
            if (!was_held && s_last_pressed_key == 0) {
                s_last_pressed_key = kc;
            }
        }

        // Map to NES buttons
        for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
            if (kc == g_keycodes[btn]) {
                new_state &= ~(uint8_t)(1u << btn); // clear bit = pressed
            }
        }
    }

    // Update held keys tracking
    memcpy(s_held_keys, keycodes, 6);

    nes_button_state = new_state;
    push_to_pio(new_state);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void usb_hid_set_pio(PIO pio, uint sm) {
    s_pio = pio;
    s_sm  = sm;
}

bool usb_hid_keyboard_connected(void) {
    return s_keyboard_count > 0;
}

uint8_t usb_hid_get_last_pressed_key(void) {
    return s_last_pressed_key;
}

bool usb_hid_is_key_held(uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (s_held_keys[i] == keycode) return true;
    }
    return false;
}

void usb_hid_clear_last_pressed_key(void) {
    s_last_pressed_key = 0;
}

// ---------------------------------------------------------------------------
// TinyUSB HID host callbacks
// ---------------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    // Check HID interface class: we only care about keyboards
    // Usage page 0x01 (Generic Desktop), Usage 0x06 (Keyboard)
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        // Request boot protocol for reliable 8-byte reports
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

        if ((size_t)instance < CFG_TUH_HID) {
            s_dev_mounted[instance] = true;
        }
        s_keyboard_count++;
        led_set_mode(LED_MODE_SOLID_ON);

        // Start receiving reports
        tuh_hid_receive_report(dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;

    if ((size_t)instance < CFG_TUH_HID && s_dev_mounted[instance]) {
        s_dev_mounted[instance] = false;
        s_keyboard_count--;
        if (s_keyboard_count < 0) s_keyboard_count = 0;
    }

    if (s_keyboard_count == 0) {
        // All keyboards disconnected - release all buttons
        nes_button_state = 0xFF;
        push_to_pio(0xFF);
        memset(s_held_keys, 0, sizeof(s_held_keys));
        s_last_pressed_key = 0;
        led_set_mode(LED_MODE_SLOW_BLINK);
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        // Boot protocol keyboard report format (8 bytes):
        // [0] modifier keys
        // [1] reserved
        // [2..7] up to 6 simultaneous keycodes
        if (len >= 8) {
            process_boot_report(report + 2);
        }
    }

    // Continue receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}
