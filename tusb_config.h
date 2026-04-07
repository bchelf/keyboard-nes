#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// -------------------------------------------------------
// Board & port
// -------------------------------------------------------
#ifndef CFG_TUSB_MCU
#  define CFG_TUSB_MCU   OPT_MCU_RP2040
#endif

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_HOST

// -------------------------------------------------------
// OS
// -------------------------------------------------------
// CFG_TUSB_OS is set by the Pico SDK build system via -D flag;
// only define it here if not already defined.
#ifndef CFG_TUSB_OS
#  define CFG_TUSB_OS            OPT_OS_NONE
#endif
#define CFG_TUSB_OS_INC_PATH     // not used with OPT_OS_NONE

// -------------------------------------------------------
// Memory
// -------------------------------------------------------
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN       __attribute__((aligned(4)))

// -------------------------------------------------------
// Host mode
// -------------------------------------------------------
#define CFG_TUH_ENABLED          1

// Hub support (disabled for v1)
#define CFG_TUH_HUB              0

// HID: support up to 4 HID interfaces (composite keyboards)
#define CFG_TUH_HID              4

// Each HID instance can have at most this many reports in queue
#define CFG_TUH_HID_EPIN_BUFSIZE 64

// -------------------------------------------------------
// Debug
// -------------------------------------------------------
// #define CFG_TUSB_DEBUG        2

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
