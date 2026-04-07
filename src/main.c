#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "nes_pio.pio.h"
#include "usb_hid.h"
#include "key_bindings.h"
#include "led.h"

// GPIO assignments
#define GPIO_LATCH   2
#define GPIO_CLOCK   3
#define GPIO_DATA    4
#define GPIO_CONFIG  15  // active low, internal pullup
#define GPIO_LED     25

// Heartbeat: push 0xFF to PIO every ~16ms when no keyboard is connected
// so the NES always sees "no buttons" rather than a stale pressed state.
#define HEARTBEAT_MS  16

static PIO  s_pio = pio0;
static uint s_sm  = 0;
static uint s_pio_offset = 0;

static void pio_init(void) {
    s_pio_offset = pio_add_program(s_pio, &nes_controller_program);
    nes_controller_program_init(s_pio, s_sm, s_pio_offset,
                                GPIO_LATCH, GPIO_CLOCK, GPIO_DATA);
}

int main(void) {
    // Initialize clocks and stdio (UART)
    stdio_init_all();

    // Initialize LED
    led_init();
    led_set_mode(LED_MODE_SLOW_BLINK);

    // Initialize PIO for NES protocol
    pio_init();

    // Push initial "all buttons released" state
    pio_sm_put(s_pio, s_sm, (uint32_t)0x00); // inverted: 0=not pressed in PIO domain

    // Load key bindings from flash (or defaults)
    key_bindings_load();

    // Set PIO reference in USB HID module
    // (usb_hid.c needs access to the PIO SM to push button states)
    usb_hid_set_pio(s_pio, s_sm);

    // Check config button (GP15) - active low
    gpio_init(GPIO_CONFIG);
    gpio_set_dir(GPIO_CONFIG, GPIO_IN);
    gpio_pull_up(GPIO_CONFIG);

    // Small settle time for pullup
    sleep_ms(10);

    bool remap_mode = !gpio_get(GPIO_CONFIG);

    // Initialize TinyUSB host
    tusb_init();

    if (remap_mode) {
        // Enter remap mode - blocks until complete or timeout
        bool success = key_bindings_remap();
        (void)success;
        // After remap (success or fail), reboot into normal mode
        // A software reset via watchdog
        watchdog_enable(1, false); // 1ms timeout, no pause on debug
        while (1) { /* wait for watchdog */ }
    }

    // Normal operation loop
    uint32_t last_heartbeat = to_ms_since_boot(get_absolute_time());

    while (true) {
        // TinyUSB host task (must be called every iteration)
        tuh_task();

        // LED state machine
        led_task();

        // Heartbeat: keep pushing current state to PIO FIFO so the NES
        // always gets a fresh value even when no keys change.
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_heartbeat >= HEARTBEAT_MS) {
            last_heartbeat = now;
            // Push inverted state (PIO expects active-high = pressed)
            uint8_t inv = ~nes_button_state;
            pio_sm_put(s_pio, s_sm, (uint32_t)inv);
        }
    }

    return 0; // unreachable
}
