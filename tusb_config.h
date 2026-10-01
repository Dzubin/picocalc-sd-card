#pragma once

/*
 * tusb_config.h - PicoCalc-SD-Drive (the one file besides the program's own .c and .h;
 * TinyUSB insists on a settings file with exactly this name)
 *
 * TinyUSB configuration: one device-mode mass storage (USB drive) interface on
 * the RP2040/RP2350's native USB controller. Nothing else is switched on.
 *
 * CFG_TUSB_MCU is defined by the Pico SDK's own build when linking the
 * tinyusb_device target, so it is not set here.
 *
 * Author: Thomas Dzubin
 */

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (expected to come from the Pico SDK tinyusb_device target)
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

/* Device mode only: the PicoCalc never acts as a USB host. */
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

/* Class drivers: only mass storage. */
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

/* Mass storage transfer buffer. It must be a whole number of 512 byte blocks;
 * bigger means fewer, longer card transfers per PC request. 2 KiB is four
 * blocks, a compromise with the RAM it takes. */
#define CFG_TUD_MSC_EP_BUFSIZE 2048

#ifdef __cplusplus
}
#endif
