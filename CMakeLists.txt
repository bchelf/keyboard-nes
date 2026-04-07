cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(nes_kbd_adapter C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(nes_kbd_adapter
    src/main.c
    src/usb_hid.c
    src/key_bindings.c
    src/led.c
)

# ---------------------------------------------------------------------------
# Generate PIO header from the .pio file (must be after add_executable)
# ---------------------------------------------------------------------------
pico_generate_pio_header(nes_kbd_adapter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nes_pio.pio
)

# Include directories
target_include_directories(nes_kbd_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # for tusb_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src      # for generated nes_pio.pio.h
    ${CMAKE_CURRENT_BINARY_DIR}          # for generated nes_pio.pio.h (pioasm output)
)

# TinyUSB configuration: must be set before linking tinyusb_host
target_compile_definitions(nes_kbd_adapter PRIVATE
    CFG_TUSB_MCU=OPT_MCU_RP2040
    CFG_TUH_ENABLED=1
    CFG_TUH_HID=4
    CFG_TUH_HUB=0
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)

# Link against required libraries
target_link_libraries(nes_kbd_adapter
    pico_stdlib
    pico_unique_id
    hardware_pio
    hardware_flash
    hardware_sync
    hardware_watchdog
    tinyusb_host
    tinyusb_board
)

# Enable USB host, disable USB device (we use TinyUSB in host mode)
# The Pico SDK sets up the USB hardware; tinyusb_host pulls in the host stack.

# Generate UF2, bin, hex outputs
pico_add_extra_outputs(nes_kbd_adapter)

# Optional: enable UART stdio for debugging (disable for production)
pico_enable_stdio_uart(nes_kbd_adapter 1)
pico_enable_stdio_usb(nes_kbd_adapter 0)
