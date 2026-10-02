#pragma once

/*
 * PicoCalc-SD-Drive.h - the one header for PicoCalc-SD-Drive
 *
 * Everything that is a constant, a type or a setting lives here, and
 * PicoCalc-SD-Drive.c holds only code. (TinyUSB insists on its own settings file,
 * tusb_config.h, so that one is separate.)
 *
 * Sections:
 *   1. Version
 *   2. Drive and SD card settings
 *   3. USB identity and layout
 *   4. SCSI codes reported to the PC
 *   5. Status screen layout and colours
 *   6. Types shared by the code
 *
 * Author: Thomas Dzubin
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tusb.h"  /* USB descriptor length macros */
#include "lcd.h"   /* the starter's LCD driver: RGB(), WIDTH, ROWS (by Blair Leduc) */
#include "keyboard.h" /* the starter's keyboard driver: KEY_ESC (by Blair Leduc) */

/* ---- 1. Version -------------------------------------------------------- */

/* The one place the version number lives. It is shown on the PicoCalc's screen,
 * reported to the PC in the USB product revision, and is the version in
 * CHANGELOG.md. Bump it here when behaviour changes. */
#define SD_DRIVE_VERSION        "V0.02B"

/* The same version without the leading V, for the 4 character USB revision field. */
#define SD_DRIVE_USB_REVISION   "0.02"

/* ---- 2. Drive and SD card settings ------------------------------------- */

/* Size of one block on the card and on the USB drive, in bytes. */
#define DRIVE_BLOCK_SIZE        512

/* How often (ms) the main loop looks at the card-detect switch. The switch
 * bounces for a few milliseconds, so this is also its debounce. */
#define MEDIA_POLL_MS           250

/* After a failed attempt to start a card, wait this long (ms) before trying again. */
#define MEDIA_RETRY_MS          1000

/* How long (ms) to wait for the card's answer when reading its size. */
#define MEDIA_CSD_TIMEOUT_MS    100

/* The most times to read the card's size register before giving up on the card.
 * A read is only believed if its checksum is right and the read after it gives
 * exactly the same bytes, so at least two reads are always made. */
#define MEDIA_CSD_TRIES         6

/* A new speed measurement starts when the drive has been idle this long (ms). */
#define SPEED_BURST_GAP_MS      1000

/* How long (ms) an activity indicator stays lit after the last read or write. */
#define ACTIVITY_HOLD_MS        200

/* SD card timing (ms). The SD specification allows a card up to 250 ms to finish
 * writing a block, so the busy wait is twice that. The starter's own driver
 * waits only a few milliseconds, which is why it is used only to start the card. */
#define SD_COMMAND_TIMEOUT_MS   100
#define SD_TOKEN_TIMEOUT_MS     200
#define SD_BUSY_TIMEOUT_MS      500

/* After a failed transfer the card is restarted and the transfer tried once more.
 * After this many failed transfers in a row the drive reports "not ready" to the
 * PC and the card is restarted from scratch instead of failing for ever. */
#define MEDIA_FAIL_LIMIT        3

/* Writes of at least this many blocks first tell the card how many are coming (ACMD23),
 * so it can erase them ahead of time. It is only a hint; short writes do not need it. */
#define SD_PREERASE_MIN_BLOCKS  4

/* The card's answer to a write data block: the low 5 bits are 00101 when accepted. */
#define SD_DATA_RESPONSE_MASK     0x1F
#define SD_DATA_RESPONSE_ACCEPTED 0x05

/* SD card command framing in SPI mode (SD Physical Layer Specification). */
#define SD_COMMAND_START_BITS     0x40 /* the top two bits of a command byte are 01 */
#define SD_COMMAND_DUMMY_CRC      0xFF /* the checksum is not checked in SPI mode */
#define SD_IDLE_BYTE              0xFF /* what the card sends, and we send, when nothing is happening */
#define SD_REPLY_NOT_YET_BIT      0x80 /* an answer byte has this bit clear; set means no answer yet */
#define SD_REPLY_TRIES            16   /* bytes to look through for a command's answer */
#define SD_CRC7_POLYNOMIAL        0x09 /* x^7 + x^3 + 1 without the top term */
#define CSD_BYTES                 16   /* the card-specific data register */

/* Leaving the program for the PicoCalc UF2 Loader (pelrun/uf2loader). The loader
 * has no call for an app to use, but its own menu hands commands to its start-up
 * code through the chip's watchdog scratch registers, which survive a watchdog
 * reboot: scratch 0 holds a magic number, 1 the boot mode, 2 an argument. Asking
 * for boot mode "SD" then rebooting makes the loader show its menu again. */
#define LOADER_COMMAND_MAGIC        0xE98CC638u /* PICOCALC_BL_MAGIC in the loader's proginfo.h */
#define LOADER_BOOT_MODE_SD         1           /* BOOT_SD: load the menu from the SD card */
#define LOADER_SCRATCH_MAGIC        0
#define LOADER_SCRATCH_MODE         1
#define LOADER_SCRATCH_ARGUMENT     2

/* Before rebooting, the USB connection is dropped and this long (ms) is given for
 * the PC to notice, so Windows sees the drive removed instead of the bus resetting. */
#define EXIT_USB_DISCONNECT_MS      100

/* The watchdog reboot happens this many ms after it is requested. */
#define EXIT_REBOOT_DELAY_MS        10

/* ---- 3. USB identity and layout ---------------------------------------- */

/* Text the PC sees when it asks what the drive is (SCSI INQUIRY).
 * Vendor is at most 8 characters, product at most 16. */
#define USB_VENDOR_TEXT         "PicoCalc"
#define USB_PRODUCT_TEXT        "SD Drive"

/* The vendor/product ID is an unregistered hobbyist pair, not an official one. */
#define USB_VID                 0xF00F
#define USB_PID                 0xD15C

#define USB_MANUFACTURER_TEXT   "Thomas Dzubin"
#define USB_DEVICE_NAME_TEXT    "PicoCalc SD Drive"

enum
{
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

#define EPNUM_MSC_OUT           0x01
#define EPNUM_MSC_IN            0x81

#define CONFIG_TOTAL_LEN        (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

#define USB_BCD_DEVICE          0x0100
#define USB_LANGID_ENGLISH_US   0x0409

/* USB string descriptors hold at most this many characters (we keep one buffer). */
#define USB_STRING_MAX_CHARS    31

/* Positions in the USB string table. */
#define STRING_SERIAL_INDEX     3
#define STRING_MSC_INDEX        4

/* ---- 4. SCSI codes reported to the PC ---------------------------------- */

/* The "additional sense codes" the drive reports along with an error (SCSI
 * Primary Commands). The PC uses them to tell the user what went wrong. */
#define ASC_MEDIUM_NOT_PRESENT      0x3A
#define ASC_MEDIA_CHANGED           0x28
#define ASC_WRITE_PROTECTED         0x27
#define ASC_UNRECOVERED_READ_ERROR  0x11
#define ASC_WRITE_ERROR             0x0C
#define ASC_INVALID_COMMAND         0x20

/* ---- 5. Status screen layout and colours ------------------------------- */

/* How often (ms) the screen checks whether anything changed. */
#define UI_REFRESH_MS           100

/* Bright, saturated colours only: dim grey text is hard to read on the real LCD. */
#define UI_COLOR_BACKGROUND     RGB(0, 0, 0)
#define UI_COLOR_TITLE          RGB(255, 255, 0)
#define UI_COLOR_LABEL          RGB(255, 255, 255)
#define UI_COLOR_GOOD           RGB(0, 255, 0)
#define UI_COLOR_WARNING        RGB(255, 160, 0)
#define UI_COLOR_BAD            RGB(255, 80, 80)
#define UI_COLOR_READ           RGB(0, 255, 255)
#define UI_COLOR_WRITE          RGB(255, 0, 255)

/* The text grid: rows and columns of 8x10 pixel characters. */
#define UI_ROWS                 ROWS
#define UI_COLUMNS              (WIDTH / 8)

/* Screen rows. */
#define UI_ROW_TITLE            0
#define UI_ROW_PC               2
#define UI_ROW_CARD             3
#define UI_ROW_DRIVE            4
#define UI_ROW_WRITE            5
#define UI_ROW_READ_COUNT       7
#define UI_ROW_WRITE_COUNT      8
#define UI_ROW_ERRORS           9
#define UI_ROW_ACTIVITY         11
#define UI_ROW_SPEED            13
#define UI_ROW_CSD              15
#define UI_ROW_CSD_STATUS       16
#define UI_ROW_KEYS             27 /* the key reminder rows start here, one per key */

/* Keys that act on the drive. Q (either case) and Escape both leave the program. */
#define UI_KEY_EJECT            'e'
#define UI_KEY_WRITE_PROTECT    'w'
#define UI_KEY_QUIT             'q'
#define UI_KEY_ESCAPE           0x1B /* plain ASCII escape, in case a keyboard sends it */
#define UI_KEY_ESCAPE_PICOCALC  KEY_ESC /* what the PicoCalc's keyboard sends (starter's keyboard.h) */
#define UI_KEY_BOOTSEL          '~'

/* Longest line the screen builds, in characters (the grid is 40 wide). */
#define UI_LINE_MAX             41

/* Room for the size text ("14.8 GB") and the CSD as hex (32 characters and a NUL). */
#define UI_SIZE_TEXT_MAX        16
#define UI_CSD_TEXT_MAX         (2 * CSD_BYTES + 1)

/* ---- 6. Types shared by the code --------------------------------------- */

typedef enum
{
    MEDIA_NONE,  /* no card in the slot */
    MEDIA_READY, /* card started and its size known */
    MEDIA_ERROR  /* a card is in but could not be started */
} media_state_t;

/* What the drive is doing: shared by the USB code, which changes it as the PC
 * reads, writes and ejects, and the screen, which shows it. */
typedef struct
{
    bool pc_connected;       /* the PC has configured the USB connection */
    bool ejected;            /* the PC or the user has ejected the drive */
    bool write_protect;      /* writes from the PC are refused */
    uint32_t blocks_read;    /* blocks sent to the PC since power up */
    uint32_t blocks_written; /* blocks received from the PC since power up */
    uint32_t errors;         /* reads and writes that failed */
    uint32_t last_read_ms;   /* time of the most recent read, 0 if none yet */
    uint32_t last_write_ms;  /* time of the most recent write, 0 if none yet */
    uint32_t epoch;          /* goes up each time the user or PC ejects or re-mounts */
    uint32_t read_kbps;      /* average read speed of the current burst of transfers, KB/s */
    uint32_t write_kbps;     /* average write speed of the current burst of transfers, KB/s */
} drive_state_t;
