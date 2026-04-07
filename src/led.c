#include "led.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#define LED_PIN  25

static led_mode_t  s_mode          = LED_MODE_SLOW_BLINK;
static bool        s_led_on        = false;
static uint32_t    s_last_toggle   = 0;

// Flash state
static uint8_t  s_flash_count    = 0;   // remaining flashes
static uint8_t  s_flash_phase    = 0;   // 0=on,1=off per flash
static bool     s_flashing       = false;
static uint32_t s_flash_deadline = 0;

// Dip state (brief LED-off while in solid mode)
static bool     s_dipping        = false;
static uint32_t s_dip_deadline   = 0;

#define DIP_MS 100

#define FLASH_ON_MS  20
#define FLASH_OFF_MS 20

void led_init(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
}

void led_set_mode(led_mode_t mode) {
    s_mode    = mode;
    s_flashing = false;   // cancel any in-progress flash sequence
    s_last_toggle = to_ms_since_boot(get_absolute_time());
}

void led_flash(uint8_t count) {
    s_flash_count    = count;
    s_flash_phase    = 0;
    s_flashing       = true;
    s_flash_deadline = to_ms_since_boot(get_absolute_time()) + FLASH_ON_MS;
    gpio_put(LED_PIN, 1);
    s_led_on = true;
}

void led_dip(void) {
    if (s_flashing) return;  // don't interrupt a flash sequence
    s_dipping      = true;
    s_dip_deadline = to_ms_since_boot(get_absolute_time()) + DIP_MS;
    gpio_put(LED_PIN, 0);
}

void led_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Handle flash sequence first (takes priority)
    if (s_flashing) {
        if (now >= s_flash_deadline) {
            if (s_flash_phase == 0) {
                // End of ON phase
                gpio_put(LED_PIN, 0);
                s_led_on     = false;
                s_flash_phase = 1;
                s_flash_deadline = now + FLASH_OFF_MS;
            } else {
                // End of OFF phase
                s_flash_count--;
                if (s_flash_count == 0) {
                    // Done flashing, return to normal mode
                    s_flashing = false;
                    s_last_toggle = now;
                } else {
                    // Next flash
                    gpio_put(LED_PIN, 1);
                    s_led_on      = true;
                    s_flash_phase = 0;
                    s_flash_deadline = now + FLASH_ON_MS;
                }
            }
        }
        return;
    }

    // Handle dip (brief off)
    if (s_dipping) {
        if (now >= s_dip_deadline) {
            s_dipping = false;
            s_last_toggle = now;
        }
        return;
    }

    // Normal mode handling
    uint32_t period_ms;
    switch (s_mode) {
        case LED_MODE_SOLID_ON:
            gpio_put(LED_PIN, 1);
            return;

        case LED_MODE_SLOW_BLINK:
            period_ms = 500;
            break;

        case LED_MODE_FAST_BLINK:
            period_ms = 100;
            break;

        default:
            return;
    }

    if (now - s_last_toggle >= period_ms) {
        s_led_on = !s_led_on;
        gpio_put(LED_PIN, s_led_on ? 1 : 0);
        s_last_toggle = now;
    }
}
