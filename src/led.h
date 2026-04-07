#ifndef LED_H
#define LED_H

#include <stdint.h>
#include <stdbool.h>

// LED state machine modes
typedef enum {
    LED_MODE_SOLID_ON,      // keyboard connected
    LED_MODE_SLOW_BLINK,    // no keyboard (500ms period)
    LED_MODE_FAST_BLINK,    // remap mode (100ms period)
    LED_MODE_FLASH,         // N quick flashes then done
} led_mode_t;

void led_init(void);

// Set continuous blink/solid mode
void led_set_mode(led_mode_t mode);

// Trigger N quick flashes (20ms on/off), then return to current mode
void led_flash(uint8_t count);

// Briefly turn LED off for 100ms then return to current mode.
// No-op if a flash sequence is already running.
void led_dip(void);

// Call every loop iteration (non-blocking)
void led_task(void);

#endif // LED_H
