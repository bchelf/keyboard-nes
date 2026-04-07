#include "usb_hid.h"
#include "key_bindings.h"
#include "led.h"

#include "tusb.h"
#include "hardware/pio.h"
#include "nes_pio.pio.h"

#include <stdio.h>
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

// Report-protocol keyboard tracking (for HID_ITF_PROTOCOL_NONE devices)
// Some keyboards (e.g. Keychron V2) enumerate with protocol=NONE and use
// HID report protocol instead of the simpler boot protocol.
static bool    s_is_report_kbd[CFG_TUH_HID] = {false};
static uint8_t s_report_kbd_id[CFG_TUH_HID] = {0};   // report_id (0 = no IDs)

// Remap mode key tracking
static uint8_t s_last_pressed_key = 0;
static uint8_t s_held_keys[6]     = {0};  // boot report has 6 key slots

// Poll summary: 256-bit set (32 bytes) tracking every keycode pressed since
// the last call to usb_hid_print_poll_summary().
static uint8_t s_poll_keys[32] = {0};  // bit N set = keycode N was pressed

static void poll_keys_set(uint8_t kc) {
    s_poll_keys[kc >> 3] |= (uint8_t)(1u << (kc & 7));
}

static const char *nes_button_name(uint8_t kc) {
    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        if (kc == g_keycodes[btn]) {
            switch (btn) {
                case NES_BTN_A:      return "A";
                case NES_BTN_B:      return "B";
                case NES_BTN_SELECT: return "Select";
                case NES_BTN_START:  return "Start";
                case NES_BTN_UP:     return "Up";
                case NES_BTN_DOWN:   return "Down";
                case NES_BTN_LEFT:   return "Left";
                case NES_BTN_RIGHT:  return "Right";
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void push_to_pio(uint8_t state) {
    if (s_pio == NULL) return;
    // PIO uses push-pull with OUT PINS (not PINDIRS).
    // nes_button_state is active-low (0=pressed, 1=not pressed) — send as-is.
    //   PIO bit=0 -> pin LOW  -> NES sees 0 (pressed)
    //   PIO bit=1 -> pin HIGH -> NES sees 1 (not pressed)
    pio_sm_put(s_pio, s_sm, (uint32_t)state);
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

        // Log every key that is currently pressed
        {
            const char *btn = nes_button_name(kc);
            if (btn) {
                printf("key pressed: 0x%02X -> NES %s\n", kc, btn);
            } else {
                printf("key pressed: 0x%02X\n", kc);
            }
            poll_keys_set(kc);

            // Remap mode tracking: only record the first press of each key
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

    // Dip LED briefly on any keypress so we know reports are arriving
    if (new_state != 0xFF) {
        led_dip();
    }

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

void usb_hid_print_poll_summary(void) {
    // Collect all keycodes set since last call
    uint8_t keys[256];
    int count = 0;
    for (int kc = 0; kc < 256; kc++) {
        if (s_poll_keys[kc >> 3] & (1u << (kc & 7))) {
            keys[count++] = (uint8_t)kc;
        }
    }
    memset(s_poll_keys, 0, sizeof(s_poll_keys));

    if (count == 0) {
        printf("[poll] no keys pressed in the last 5 seconds\n");
        return;
    }

    printf("[poll] keys pressed in the last 5 seconds (%d):", count);
    for (int i = 0; i < count; i++) {
        const char *btn = nes_button_name(keys[i]);
        if (btn) {
            printf("  0x%02X(%s)", keys[i], btn);
        } else {
            printf("  0x%02X", keys[i]);
        }
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// TinyUSB device-level callbacks (fire for any USB device, before HID)
// ---------------------------------------------------------------------------

void tuh_mount_cb(uint8_t dev_addr) {
    printf("[usb] device mounted: addr=%u\n", dev_addr);
    // 1 flash = USB device detected (fires before HID enumeration)
    led_flash(1);
}

void tuh_umount_cb(uint8_t dev_addr) {
    printf("[usb] device unmounted: addr=%u\n", dev_addr);
}

// ---------------------------------------------------------------------------
// TinyUSB HID host callbacks
// ---------------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    printf("[hid] mount: addr=%u instance=%u protocol=%u (%s)\n",
           dev_addr, instance, itf_protocol,
           itf_protocol == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" :
           itf_protocol == HID_ITF_PROTOCOL_MOUSE    ? "mouse"    : "other");

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        // Boot-protocol keyboard: request boot protocol for fixed 8-byte reports
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

        if ((size_t)instance < CFG_TUH_HID) {
            s_dev_mounted[instance] = true;
        }
        s_keyboard_count++;
        led_set_mode(LED_MODE_SOLID_ON);
        led_flash(2);  // 2 flashes → solid = boot-protocol keyboard accepted

        printf("[hid] boot-protocol keyboard accepted\n");
        tuh_hid_receive_report(dev_addr, instance);

    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE && desc_report != NULL) {
        // Generic HID interface: parse the report descriptor to check if
        // this is a keyboard (usage page 0x01 Generic Desktop, usage 0x06 Keyboard)
        tuh_hid_report_info_t info[8];
        uint8_t n = tuh_hid_parse_report_descriptor(info, 8, desc_report, desc_len);

        for (uint8_t i = 0; i < n; i++) {
            printf("[hid] report descriptor entry %u: usage_page=0x%02X usage=0x%02X report_id=%u\n",
                   i, info[i].usage_page, info[i].usage, info[i].report_id);

            if (info[i].usage_page == HID_USAGE_PAGE_DESKTOP &&
                info[i].usage      == HID_USAGE_DESKTOP_KEYBOARD) {

                if ((size_t)instance < CFG_TUH_HID) {
                    s_dev_mounted[instance]    = true;
                    s_is_report_kbd[instance]  = true;
                    s_report_kbd_id[instance]  = info[i].report_id;
                }
                s_keyboard_count++;
                led_set_mode(LED_MODE_SOLID_ON);
                led_flash(2);  // 2 flashes → solid = report-protocol keyboard accepted

                printf("[hid] report-protocol keyboard accepted, report_id=%u\n",
                       info[i].report_id);
                tuh_hid_receive_report(dev_addr, instance);
                break;
            }
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("[hid] umount: addr=%u instance=%u\n", dev_addr, instance);

    if ((size_t)instance < CFG_TUH_HID && s_dev_mounted[instance]) {
        s_dev_mounted[instance]   = false;
        s_is_report_kbd[instance] = false;
        s_report_kbd_id[instance] = 0;
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
        // Boot protocol: fixed 8-byte report [modifier][reserved][key0..key5]
        if (len >= 8) {
            process_boot_report(report + 2);
        } else {
            printf("[hid] short boot report: len=%u (expected >=8)\n", len);
        }
    } else if ((size_t)instance < CFG_TUH_HID && s_is_report_kbd[instance]) {
        // Report protocol keyboard: same logical layout but may be prefixed
        // with a report ID byte when the device uses multiple report IDs.
        const uint8_t *kbd = report;
        uint16_t       klen = len;

        if (s_report_kbd_id[instance] != 0) {
            // First byte is the report ID; skip it if it matches ours,
            // ignore the report entirely if it belongs to a different report.
            if (klen < 1 || kbd[0] != s_report_kbd_id[instance]) {
                tuh_hid_receive_report(dev_addr, instance);
                return;
            }
            kbd++;
            klen--;
        }

        // Remaining bytes: [modifier][reserved][key0..key5]
        if (klen >= 8) {
            process_boot_report(kbd + 2);
        } else {
            printf("[hid] short report-protocol report: klen=%u\n", klen);
        }
    }

    // Continue receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}
