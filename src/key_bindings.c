#include "key_bindings.h"
#include "led.h"
#include "usb_hid.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "tusb.h"

#include <string.h>

// Default bindings: index = NES button, value = HID keycode
static const uint8_t k_default_keycodes[NES_NUM_BUTTONS] = {
    [NES_BTN_A]      = HID_KEY_L,
    [NES_BTN_B]      = HID_KEY_K,
    [NES_BTN_SELECT] = HID_KEY_BRACKET_LEFT,
    [NES_BTN_START]  = HID_KEY_BRACKET_RIGHT,
    [NES_BTN_UP]     = HID_KEY_W,
    [NES_BTN_DOWN]   = HID_KEY_S,
    [NES_BTN_LEFT]   = HID_KEY_A,
    [NES_BTN_RIGHT]  = HID_KEY_D,
};

static const uint8_t k_mister_nes_reset_order[NES_NUM_BUTTONS] = {
    NES_BTN_RIGHT,
    NES_BTN_LEFT,
    NES_BTN_DOWN,
    NES_BTN_UP,
    NES_BTN_A,
    NES_BTN_B,
    NES_BTN_SELECT,
    NES_BTN_START,
};

uint8_t g_keycodes[NES_NUM_BUTTONS];

static bool s_reset_mapping_active = false;
static uint8_t s_reset_mapping_step = 0;

// Layout of the flash sector:
//   byte 0:   magic (FLASH_MAGIC_VALUE)
//   bytes 1-8: keycodes[0..7]
typedef struct {
    uint8_t magic;
    uint8_t keycodes[NES_NUM_BUTTONS];
    uint8_t _pad[FLASH_PAGE_SIZE - 1 - NES_NUM_BUTTONS]; // pad to page size
} __attribute__((packed)) flash_binding_page_t;

_Static_assert(sizeof(flash_binding_page_t) == FLASH_PAGE_SIZE,
               "flash_binding_page_t must be exactly FLASH_PAGE_SIZE bytes");

void key_bindings_load(void) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + FLASH_BINDINGS_OFFSET);
    if (flash_ptr[0] == FLASH_MAGIC_VALUE) {
        memcpy(g_keycodes, flash_ptr + 1, NES_NUM_BUTTONS);
    } else {
        memcpy(g_keycodes, k_default_keycodes, NES_NUM_BUTTONS);
    }
}

bool key_bindings_save(void) {
    // Build page buffer (256 bytes)
    uint8_t page_buf[FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    page_buf[0] = FLASH_MAGIC_VALUE;
    memcpy(page_buf + 1, g_keycodes, NES_NUM_BUTTONS);

    // Disable interrupts while programming flash
    uint32_t saved = save_and_disable_interrupts();

    flash_range_erase(FLASH_BINDINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_BINDINGS_OFFSET, page_buf, FLASH_PAGE_SIZE);

    restore_interrupts(saved);

    // Verify
    const uint8_t *readback = (const uint8_t *)(XIP_BASE + FLASH_BINDINGS_OFFSET);
    if (readback[0] != FLASH_MAGIC_VALUE) return false;
    for (int i = 0; i < NES_NUM_BUTTONS; i++) {
        if (readback[i + 1] != g_keycodes[i]) return false;
    }
    return true;
}

void key_bindings_start_reset_mapping(void) {
    s_reset_mapping_active = true;
    s_reset_mapping_step = 0;
    led_set_mode(LED_MODE_FAST_BLINK);
}

bool key_bindings_reset_mapping_active(void) {
    return s_reset_mapping_active;
}

bool key_bindings_reset_mapping_capture(uint8_t keycode) {
    if (!s_reset_mapping_active || keycode == 0) {
        return false;
    }

    uint8_t btn = k_mister_nes_reset_order[s_reset_mapping_step];
    g_keycodes[btn] = keycode;
    led_flash((uint8_t)(s_reset_mapping_step + 1));

    s_reset_mapping_step++;
    if (s_reset_mapping_step < NES_NUM_BUTTONS) {
        return true;
    }

    bool ok = key_bindings_save();
    s_reset_mapping_active = false;
    s_reset_mapping_step = 0;
    led_set_mode(LED_MODE_SOLID_ON);
    if (ok) {
        led_flash(3);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Remap mode
// ---------------------------------------------------------------------------

static const char *const k_button_names[NES_NUM_BUTTONS] = {
    "A", "B", "Select", "Start", "Up", "Down", "Left", "Right"
};

// Wait for a key press + release, with a global timeout from entry.
// Returns the HID keycode of the key pressed, or 0 on timeout.
static uint8_t wait_for_key(uint32_t timeout_ms) {
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;

    // Wait for a key to be pressed
    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        tuh_task();
        led_task();

        uint8_t kc = usb_hid_get_last_pressed_key();
        if (kc != 0) {
            // Wait for release
            uint32_t rel_deadline = to_ms_since_boot(get_absolute_time()) + 2000;
            while (to_ms_since_boot(get_absolute_time()) < rel_deadline) {
                tuh_task();
                led_task();
                if (!usb_hid_is_key_held(kc)) break;
            }
            usb_hid_clear_last_pressed_key();
            return kc;
        }
    }
    return 0;
}

bool key_bindings_remap(void) {
    led_set_mode(LED_MODE_FAST_BLINK);

    // Wait up to 5 seconds for a keyboard to connect
    uint32_t kb_deadline = to_ms_since_boot(get_absolute_time()) + 5000;
    while (!usb_hid_keyboard_connected()) {
        tuh_task();
        led_task();
        if (to_ms_since_boot(get_absolute_time()) >= kb_deadline) {
            // Timed out waiting for keyboard
            return false;
        }
    }

    uint8_t new_keycodes[NES_NUM_BUTTONS];

    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        // Blink LED (btn+1) times to indicate which button
        led_flash((uint8_t)(btn + 1));
        // Wait for flash to complete (each flash is 40ms, plus gaps)
        uint32_t flash_wait = to_ms_since_boot(get_absolute_time()) +
                              (uint32_t)(btn + 1) * 60 + 100;
        while (to_ms_since_boot(get_absolute_time()) < flash_wait) {
            tuh_task();
            led_task();
        }

        // Wait for key press (30 second per-button timeout)
        uint8_t kc = wait_for_key(30000);
        if (kc == 0) {
            // Timed out - abort remap
            return false;
        }
        new_keycodes[btn] = kc;
    }

    // All captured - save to flash
    memcpy(g_keycodes, new_keycodes, NES_NUM_BUTTONS);
    bool ok = key_bindings_save();
    if (ok) {
        led_flash(3); // 3 quick flashes = success
        uint32_t wait = to_ms_since_boot(get_absolute_time()) + 500;
        while (to_ms_since_boot(get_absolute_time()) < wait) {
            tuh_task();
            led_task();
        }
    }
    return ok;
}
