#include "usb_hid.h"
#include "key_bindings.h"
#include "led.h"
#include "debug_log.h"

#include "tusb.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "nes_pio.pio.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

#ifndef NES_TAP_STRETCH_US
#define NES_TAP_STRETCH_US 17000u
#endif

#ifndef NES_TRACE_SELECT
#define NES_TRACE_SELECT 0
#endif

#ifndef NES_LOG_KEY_EDGES
#define NES_LOG_KEY_EDGES 0
#endif

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

volatile uint8_t current_nes_state = 0xFF; // all unpressed

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

// Raw keyboard-derived button state before optional tap stretch is applied.
static uint8_t s_keyboard_nes_state = 0xFF;
static uint8_t s_last_latched_state = 0xFF;
static uint32_t s_press_started_us[NES_NUM_BUTTONS] = {0};
static uint32_t s_release_hold_until_us[NES_NUM_BUTTONS] = {0};

// Poll summary: 256-bit set (32 bytes) tracking every keycode pressed since
// the last call to usb_hid_print_poll_summary().
static uint8_t s_poll_keys[32] = {0};  // bit N set = keycode N was pressed

typedef struct {
    uint32_t hid_reports;
    uint32_t exposed_state_changes;
    uint32_t pio_publish_count;
    uint32_t pio_overwrite_words;
    uint32_t pio_prime_count;
    uint32_t tx_fifo_max_depth;
    uint32_t nes_latches;
    uint32_t hid_error_reports;
    uint32_t usb_press_edges[NES_NUM_BUTTONS];
    uint32_t nes_visible_press_edges[NES_NUM_BUTTONS];
    uint32_t last_hid_report_us;
    uint32_t last_state_change_us;
    uint32_t last_pio_publish_us;
    uint32_t last_latch_us;
} latency_stats_t;

static latency_stats_t s_latency = {0};

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

static const char *button_name_from_index(int btn) {
    switch (btn) {
        case NES_BTN_A:      return "A";
        case NES_BTN_B:      return "B";
        case NES_BTN_SELECT: return "Select";
        case NES_BTN_START:  return "Start";
        case NES_BTN_UP:     return "Up";
        case NES_BTN_DOWN:   return "Down";
        case NES_BTN_LEFT:   return "Left";
        case NES_BTN_RIGHT:  return "Right";
        default:             return "?";
    }
}

static void maybe_log_press_edges(uint8_t prev_state, uint8_t new_state) {
#if NES_LOG_KEY_EDGES
    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        if (!bit_is_pressed(prev_state, btn) && bit_is_pressed(new_state, btn)) {
            NES_LOG("key edge -> NES %s\n", button_name_from_index(btn));
        }
    }
#else
    (void)prev_state;
    (void)new_state;
#endif
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline uint32_t now_us(void) {
    return time_us_32();
}

static inline bool bit_is_pressed(uint8_t state, int btn) {
    return (state & (uint8_t)(1u << btn)) == 0;
}

#if NES_TRACE_SELECT
static void trace_select(const char *stage, uint8_t state, uint32_t now) {
    NES_LOG("[trace][Select] %s t=%luus pressed=%u state=0x%02X"
            " hid_dt=%ldus state_dt=%ldus pio_dt=%ldus latch_dt=%ldus\n",
            stage,
            (unsigned long)now,
            bit_is_pressed(state, NES_BTN_SELECT) ? 1u : 0u,
            state,
            (long)(now - s_latency.last_hid_report_us),
            (long)(now - s_latency.last_state_change_us),
            (long)(now - s_latency.last_pio_publish_us),
            (long)(now - s_latency.last_latch_us));
}
#else
static void trace_select(const char *stage, uint8_t state, uint32_t now) {
    (void)stage;
    (void)state;
    (void)now;
}
#endif

static uint8_t compute_exposed_state(uint32_t now) {
    uint8_t state = s_keyboard_nes_state;

    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        const uint8_t mask = (uint8_t)(1u << btn);
        if ((state & mask) == 0) {
            s_release_hold_until_us[btn] = 0;
            continue;
        }

        uint32_t hold_until = s_release_hold_until_us[btn];
        if (hold_until != 0 && (int32_t)(hold_until - now) > 0) {
            state &= (uint8_t)~mask;
        } else {
            s_release_hold_until_us[btn] = 0;
        }
    }

    return state;
}

static void publish_current_state(uint32_t now, bool prime_only) {
    if (s_pio == NULL) return;

    uint tx_depth = pio_sm_get_tx_fifo_level(s_pio, s_sm);
    if (tx_depth > s_latency.tx_fifo_max_depth) {
        s_latency.tx_fifo_max_depth = tx_depth;
    }

    if (prime_only && tx_depth != 0) {
        return;
    }

    if (tx_depth != 0) {
        s_latency.pio_overwrite_words += tx_depth;
        pio_sm_clear_fifos(s_pio, s_sm);
    }

    // GPIO_DATA drives a 2N7002 pull-down enable, so invert the active-low
    // NES state into active-high gate control bits.
    //   state bit=0 pressed     -> PIO bit=1 -> MOSFET on  -> NES DATA low
    //   state bit=1 not pressed -> PIO bit=0 -> MOSFET off -> NES DATA high
    pio_sm_put(s_pio, s_sm, (uint32_t)((uint8_t)~current_nes_state));
    s_latency.pio_publish_count++;
    if (prime_only) {
        s_latency.pio_prime_count++;
    }
    s_latency.last_pio_publish_us = now;
    trace_select(prime_only ? "pio-prime" : "pio-update", current_nes_state, now);
}

static void update_exposed_state(uint32_t now) {
    uint8_t new_state = compute_exposed_state(now);
    if (new_state == current_nes_state) {
        return;
    }

    current_nes_state = new_state;
    s_latency.exposed_state_changes++;
    s_latency.last_state_change_us = now;
    trace_select("state-change", current_nes_state, now);
    publish_current_state(now, false);
}

// Update keyboard-derived state from a 6-byte boot report keycode array
static void process_boot_report(const uint8_t keycodes[6]) {
    uint8_t new_state = 0xFF; // start with all unpressed
    uint32_t now = now_us();
    uint8_t prev_keyboard_state = s_keyboard_nes_state;

    s_latency.hid_reports++;
    s_latency.last_hid_report_us = now;

    for (int k = 0; k < 6; k++) {
        if (keycodes[k] > 0x00 && keycodes[k] < 0x04) {
            s_latency.hid_error_reports++;
            NES_LOG("[hid] ignoring keyboard error report: %02X %02X %02X %02X %02X %02X\n",
                    keycodes[0], keycodes[1], keycodes[2],
                    keycodes[3], keycodes[4], keycodes[5]);
            return;
        }
    }

    // Check each pressed key against our bindings
    for (int k = 0; k < 6; k++) {
        uint8_t kc = keycodes[k];
        if (kc == 0x00 || kc >= 0x04) {
            if (kc == 0x00) continue; // no key in this slot
        } else {
            continue; // error codes (1=rollover, 2=post fail, 3=undefined)
        }

        // Poll summary + remap tracking
        {
            poll_keys_set(kc);

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

    maybe_log_press_edges(prev_keyboard_state, new_state);

    // Update held keys tracking
    memcpy(s_held_keys, keycodes, 6);

    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        bool was_pressed = bit_is_pressed(prev_keyboard_state, btn);
        bool now_pressed = bit_is_pressed(new_state, btn);

        if (was_pressed == now_pressed) {
            continue;
        }

        if (now_pressed) {
            s_press_started_us[btn] = now;
            s_release_hold_until_us[btn] = 0;
            s_latency.usb_press_edges[btn]++;
            if (btn == NES_BTN_SELECT) {
                trace_select("usb-press", new_state, now);
            }
        } else {
            uint32_t pressed_at = s_press_started_us[btn];
            uint32_t min_release_time = pressed_at + NES_TAP_STRETCH_US;
            if (pressed_at != 0 && (int32_t)(min_release_time - now) > 0) {
                s_release_hold_until_us[btn] = min_release_time;
            } else {
                s_release_hold_until_us[btn] = 0;
            }
            if (btn == NES_BTN_SELECT) {
                trace_select("usb-release", new_state, now);
            }
        }
    }

    s_keyboard_nes_state = new_state;

    // Dip LED briefly on any keypress so we know reports are arriving
    if (new_state != 0xFF) {
        led_dip();
    }

    update_exposed_state(now);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void usb_hid_set_pio(PIO pio, uint sm) {
    s_pio = pio;
    s_sm  = sm;
    publish_current_state(now_us(), false);
}

void usb_hid_task(void) {
    uint32_t now = now_us();

    update_exposed_state(now);

    if (s_pio != NULL && pio_sm_is_tx_fifo_empty(s_pio, s_sm)) {
        publish_current_state(now, true);
    }
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
        NES_LOG("[poll] no keys pressed in the last 5 seconds\n");
        return;
    }

    NES_LOG("[poll] keys pressed in the last 5 seconds (%d):", count);
    for (int i = 0; i < count; i++) {
        const char *btn = nes_button_name(keys[i]);
        if (btn) {
            NES_LOG("  0x%02X(%s)", keys[i], btn);
        } else {
            NES_LOG("  0x%02X", keys[i]);
        }
    }
    NES_LOG("\n");
}

void usb_hid_note_nes_latch_irq(void) {
    uint32_t now = now_us();
    uint8_t latched_state = current_nes_state;

    for (int btn = 0; btn < NES_NUM_BUTTONS; btn++) {
        if (!bit_is_pressed(s_last_latched_state, btn) &&
            bit_is_pressed(latched_state, btn)) {
            s_latency.nes_visible_press_edges[btn]++;
        }
    }

    s_last_latched_state = latched_state;
    s_latency.nes_latches++;
    s_latency.last_latch_us = now;
    trace_select("nes-latch", latched_state, now);
}

void usb_hid_print_latency_summary(void) {
    static const int buttons_to_report[] = {
        NES_BTN_A, NES_BTN_B, NES_BTN_SELECT, NES_BTN_START
    };
    latency_stats_t snapshot;
    uint32_t irq_state = save_and_disable_interrupts();

    snapshot = s_latency;
    memset(&s_latency, 0, sizeof(s_latency));

    restore_interrupts(irq_state);

    NES_LOG("[latency] hid_reports=%lu error_reports=%lu state_changes=%lu "
            "pio_put=%lu primes=%lu overwrite_words=%lu tx_depth_max=%lu "
            "latches=%lu last_us{hid=%lu,state=%lu,pio=%lu,latch=%lu}\n",
            (unsigned long)snapshot.hid_reports,
            (unsigned long)snapshot.hid_error_reports,
            (unsigned long)snapshot.exposed_state_changes,
            (unsigned long)snapshot.pio_publish_count,
            (unsigned long)snapshot.pio_prime_count,
            (unsigned long)snapshot.pio_overwrite_words,
            (unsigned long)snapshot.tx_fifo_max_depth,
            (unsigned long)snapshot.nes_latches,
            (unsigned long)snapshot.last_hid_report_us,
            (unsigned long)snapshot.last_state_change_us,
            (unsigned long)snapshot.last_pio_publish_us,
            (unsigned long)snapshot.last_latch_us);

    for (size_t i = 0; i < sizeof(buttons_to_report) / sizeof(buttons_to_report[0]); i++) {
        int btn = buttons_to_report[i];
        NES_LOG("[latency] %-6s usb_presses=%lu nes_visible=%lu\n",
                button_name_from_index(btn),
                (unsigned long)snapshot.usb_press_edges[btn],
                (unsigned long)snapshot.nes_visible_press_edges[btn]);
    }
}

// ---------------------------------------------------------------------------
// TinyUSB device-level callbacks (fire for any USB device, before HID)
// ---------------------------------------------------------------------------

void tuh_mount_cb(uint8_t dev_addr) {
    NES_LOG("[usb] device mounted: addr=%u\n", dev_addr);
    // 1 flash = USB device detected (fires before HID enumeration)
    led_flash(1);
}

void tuh_umount_cb(uint8_t dev_addr) {
    NES_LOG("[usb] device unmounted: addr=%u\n", dev_addr);
}

// ---------------------------------------------------------------------------
// TinyUSB HID host callbacks
// ---------------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    NES_LOG("[hid] mount: addr=%u instance=%u protocol=%u (%s)\n",
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

        NES_LOG("[hid] boot-protocol keyboard accepted\n");
        tuh_hid_receive_report(dev_addr, instance);

    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE && desc_report != NULL) {
        // Generic HID interface: parse the report descriptor to check if
        // this is a keyboard (usage page 0x01 Generic Desktop, usage 0x06 Keyboard)
        tuh_hid_report_info_t info[8];
        uint8_t n = tuh_hid_parse_report_descriptor(info, 8, desc_report, desc_len);

        for (uint8_t i = 0; i < n; i++) {
            NES_LOG("[hid] report descriptor entry %u: usage_page=0x%02X usage=0x%02X report_id=%u\n",
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

                NES_LOG("[hid] report-protocol keyboard accepted, report_id=%u\n",
                        info[i].report_id);
                tuh_hid_receive_report(dev_addr, instance);
                break;
            }
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    NES_LOG("[hid] umount: addr=%u instance=%u\n", dev_addr, instance);

    if ((size_t)instance < CFG_TUH_HID && s_dev_mounted[instance]) {
        s_dev_mounted[instance]   = false;
        s_is_report_kbd[instance] = false;
        s_report_kbd_id[instance] = 0;
        s_keyboard_count--;
        if (s_keyboard_count < 0) s_keyboard_count = 0;
    }

    if (s_keyboard_count == 0) {
        // All keyboards disconnected - release all buttons
        s_keyboard_nes_state = 0xFF;
        memset(s_press_started_us, 0, sizeof(s_press_started_us));
        memset(s_release_hold_until_us, 0, sizeof(s_release_hold_until_us));
        current_nes_state = 0xFF;
        publish_current_state(now_us(), false);
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
            NES_LOG("[hid] short boot report: len=%u (expected >=8)\n", len);
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
            NES_LOG("[hid] short report-protocol report: klen=%u\n", klen);
        }
    }

    // Continue receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}
