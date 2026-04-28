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
#define GPIO_HOST_VBUS_EN  10  // active high: enables USB host VBUS power switch
#define GPIO_FAULT_N       11  // active low fault output from USB host power switch
#define GPIO_CONFIG        15  // active low, internal pullup
#define GPIO_LATCH         17
#define GPIO_CLOCK         18
#define GPIO_DATA          19
#define GPIO_LED           25

static PIO  s_pio = pio0;
static uint s_sm  = 0;
static uint s_pio_offset = 0;

// Signal monitors: incremented in GPIO IRQs, checked in the main loop.
static volatile uint32_t s_latch_count = 0;  // LATCH rising edges
static volatile uint32_t s_clock_count = 0;  // CLK falling edges

static void nes_signal_irq_handler(uint gpio, uint32_t events) {
    (void)events;
    if (gpio == GPIO_LATCH) {
        s_latch_count++;
        usb_hid_note_nes_latch_irq();
    }
    if (gpio == GPIO_CLOCK) s_clock_count++;
}

static void pio_init(void) {
    s_pio_offset = pio_add_program(s_pio, &nes_controller_program);
    nes_controller_program_init(s_pio, s_sm, s_pio_offset,
                                GPIO_LATCH, GPIO_CLOCK, GPIO_DATA);

    // Attach a rising-edge GPIO interrupt to LATCH so the main loop
    // can tell whether the NES is actually sending latch pulses.
    // GPIO interrupts fire on the raw pin state and work alongside PIO ownership.
    // GPIO_IRQ_EDGE_RISE on LATCH, GPIO_IRQ_EDGE_FALL on CLK.
    // Both share one callback; the SDK allows only one callback per core.
    gpio_set_irq_enabled_with_callback(GPIO_LATCH, GPIO_IRQ_EDGE_RISE,
                                       true, nes_signal_irq_handler);
    gpio_set_irq_enabled(GPIO_CLOCK, GPIO_IRQ_EDGE_FALL, true);
}

static void host_power_init(void) {
    gpio_init(GPIO_FAULT_N);
    gpio_set_dir(GPIO_FAULT_N, GPIO_IN);
    gpio_pull_up(GPIO_FAULT_N);

    gpio_init(GPIO_HOST_VBUS_EN);
    gpio_put(GPIO_HOST_VBUS_EN, 1);
    gpio_set_dir(GPIO_HOST_VBUS_EN, GPIO_OUT);
}

int main(void) {
    // Initialize clocks and stdio (UART)
    stdio_init_all();

    // Initialize LED
    led_init();
    led_set_mode(LED_MODE_SLOW_BLINK);

    // Enable host-side VBUS before starting the USB host stack.
    host_power_init();

    // Initialize PIO for NES protocol
    pio_init();

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

    printf("nes_kbd_adapter: boot complete, waiting for USB keyboard\n");

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
    uint32_t last_poll        = to_ms_since_boot(get_absolute_time());
    uint32_t last_latch_check = last_poll;
    uint32_t last_latch_seen  = s_latch_count;
    uint32_t last_clock_seen  = s_clock_count;
    bool last_fault_asserted  = !gpio_get(GPIO_FAULT_N);

    while (true) {
        // TinyUSB host task (must be called every iteration)
        tuh_task();

        // Service HID-to-NES state propagation every loop.
        usb_hid_task();

        // LED state machine
        led_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 5-second poll summary
        if (now - last_poll >= 5000) {
            last_poll = now;
            usb_hid_print_poll_summary();
            usb_hid_print_latency_summary();
        }

        bool fault_asserted = !gpio_get(GPIO_FAULT_N);
        if (fault_asserted != last_fault_asserted) {
            last_fault_asserted = fault_asserted;
            printf("[usb-host-power] FAULT_N %s\n",
                   fault_asserted ? "asserted" : "released");
        }

        // Latch monitor: once per second, check whether the NES has been
        // sending latch pulses. A single quick flash = NES is polling.
        // No flash = LATCH never reaching the configured GPIO
        // (wiring or level-shifter issue).
        if (now - last_latch_check >= 1000) {
            last_latch_check = now;
            uint32_t latches = s_latch_count - last_latch_seen;
            uint32_t clocks  = s_clock_count - last_clock_seen;
            last_latch_seen  = s_latch_count;
            last_clock_seen  = s_clock_count;

            printf("[nes] last second: %lu LATCH pulses, %lu CLK edges\n",
                   (unsigned long)latches, (unsigned long)clocks);

            if (clocks >= 400) {
                led_flash(1);  // CLK arriving correctly (~480 expected at 60Hz x 8)
            }
        }
    }

    return 0; // unreachable
}
