/*
 * PicoCalc-SD-Drive.c - the whole program
 *
 * Makes the PicoCalc look like a USB drive to a PC, using the SD card in the
 * PicoCalc as the disc. The PC sees an ordinary removable drive; the PicoCalc
 * shows a status screen and has keys to eject the drive and to write-protect it.
 * While plugged in, the PC owns the card: the PicoCalc does not use it itself.
 *
 * Reading guide. main() is first, then the program in sections that build on
 * each other, each of which only uses the ones above it:
 *
 *   1. SD card blocks     our own block reads and writes (the starter's routines give
 *                         up waiting for a busy card after a few milliseconds)
 *   2. The media          the card in the slot: insert and remove, its size, retry
 *   3. Drive state        what the drive is doing: ejected, write protect, counters
 *   4. USB descriptors    the identity the PC sees (one mass storage interface)
 *   5. USB mass storage   answers the PC's requests from the card and the drive state
 *   6. Status screen      what the PicoCalc's LCD shows
 *
 * All constants, settings and shared types are in PicoCalc-SD-Drive.h. The
 * picocalc-text-starter drivers (by Blair Leduc) are used for the LCD, the
 * keyboard and to start the card; they are vendored, read-only, in
 * picocalc-text-starter-main/.
 *
 * Author: Thomas Dzubin
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "keyboard.h"
#include "sdcard.h"

#include "PicoCalc-SD-Drive.h"

static void media_init(void);
static void media_poll(void);
static void usb_msc_init(void);
static void usb_msc_task(void);
static void ui_init(void);
static void ui_update(void);
static void handle_keys(void);
static void drive_toggle_eject(void);
static void drive_toggle_write_protect(void);

/* What the drive is doing; see drive_state_t in the header. */
static drive_state_t drive;

/* keyboard.c (by Blair Leduc) sets this on its own interrupt key; this program
 * does not use it, but the driver refers to it, so it needs a definition. */
volatile bool user_interrupt = false;

int main(void)
{
    /* The keyboard comes first so the '~' reboot-to-BOOTSEL key works even if
     * something later were to hang. */
    keyboard_init();
    keyboard_set_background_poll(true);

    media_init();
    ui_init();
    usb_msc_init();

    while (1)
    {
        usb_msc_task();  /* keep the PC's requests flowing */
        media_poll();    /* card inserted or removed? */
        handle_keys();
        ui_update();
    }
}

/* The PicoCalc's own keys: eject, write protect, and reboot to BOOTSEL. */
static void handle_keys(void)
{
    while (keyboard_key_available())
    {
        int key = (unsigned char)keyboard_get_key();

        if (key == UI_KEY_BOOTSEL)
            reset_usb_boot(0, 0); /* does not return */
        else if (tolower(key) == UI_KEY_EJECT)
            drive_toggle_eject();
        else if (tolower(key) == UI_KEY_WRITE_PROTECT)
            drive_toggle_write_protect();
    }
}

/* ======================================================================== */
/* 1. SD card blocks                                                         */
/* ======================================================================== */

/*
 * Our own block transfer routines for an SD card in SPI mode, on the SPI bus and
 * chip select from the starter's sdcard.h, after the starter's sd_card_init() has
 * started the card. The starter's block routines wait only a few milliseconds
 * for a card to finish a write and carry on regardless, which sends commands to a
 * card that is still busy and makes every later transfer fail. These wait as long
 * as the SD specification allows, and move several blocks per command, which is
 * also faster.
 */

_Static_assert(DRIVE_BLOCK_SIZE == SD_BLOCK_SIZE, "the drive and the starter's SD driver must agree on the block size");

static uint8_t sd_read_byte(void)
{
    uint8_t byte = SD_IDLE_BYTE;

    spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &byte, 1);
    return byte;
}

static void sd_write_byte(uint8_t byte)
{
    spi_write_blocking(SD_SPI, &byte, 1);
}

/* Lets go of the card: chip select high, then a few clocks so the card releases the data line. */
static void sd_deselect(void)
{
    gpio_put(SD_CS, 1);
    (void)sd_read_byte();
}

/* Waits for the card to stop being busy (it holds the data line low while it is). */
static bool sd_wait_not_busy(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (sd_read_byte() != SD_IDLE_BYTE)
    {
        if (time_reached(deadline))
            return false;
    }
    return true;
}

/* Sends a command packet and reads the card's one byte answer (R1), or
 * SD_IDLE_BYTE if it gave none. Chip select must already be low. `skip_stuff_byte`
 * is for the stop command, after which the card sends one stuff byte before its
 * answer. */
static uint8_t sd_send_packet(uint8_t command, uint32_t argument, bool skip_stuff_byte)
{
    uint8_t packet[6];
    uint8_t reply = SD_IDLE_BYTE;

    packet[0] = SD_COMMAND_START_BITS | command;
    packet[1] = (uint8_t)(argument >> 24);
    packet[2] = (uint8_t)(argument >> 16);
    packet[3] = (uint8_t)(argument >> 8);
    packet[4] = (uint8_t)argument;
    packet[5] = SD_COMMAND_DUMMY_CRC;
    spi_write_blocking(SD_SPI, packet, sizeof(packet));

    if (skip_stuff_byte)
        (void)sd_read_byte();

    /* The answer comes within a few bytes: a byte with the top bit clear. */
    for (int i = 0; i < SD_REPLY_TRIES; i++)
    {
        reply = sd_read_byte();
        if ((reply & SD_REPLY_NOT_YET_BIT) == 0)
            break;
    }
    return reply;
}

/* Sends a command to an idle card, with chip select already low, and returns the
 * card's answer (R1), or SD_IDLE_BYTE if it was busy or gave none. */
static uint8_t sd_send_command(uint8_t command, uint32_t argument)
{
    if (!sd_wait_not_busy(SD_COMMAND_TIMEOUT_MS))
        return SD_IDLE_BYTE;
    return sd_send_packet(command, argument, false);
}

/* Stops a multiple block transfer with CMD12. The card is not idle at that point
 * (a read is still streaming the next block), so no busy wait comes first; the
 * answer to a stop command is delayed by one stuff byte. Returns once the card has
 * let go of the bus, or false if it stays busy too long. */
static bool sd_stop_transmission(void)
{
    (void)sd_send_packet(SD_CMD12, 0, true);
    return sd_wait_not_busy(SD_BUSY_TIMEOUT_MS);
}

/* Waits for the start of a data block coming from the card. */
static bool sd_wait_data_token(void)
{
    absolute_time_t deadline = make_timeout_time_ms(SD_TOKEN_TIMEOUT_MS);
    uint8_t token;

    do
    {
        token = sd_read_byte();
        if (token == SD_DATA_START_BLOCK)
            return true;
        if (token != SD_IDLE_BYTE)
            return false; /* an error token: the card could not read the block */
    } while (!time_reached(deadline));
    return false;
}

static uint32_t sd_address_of(uint32_t lba, bool sdhc)
{
    return sdhc ? lba : lba * DRIVE_BLOCK_SIZE;
}

/* Reads `count` whole 512 byte blocks from block `lba`. `sdhc` is true for a high
 * capacity card (blocks addressed by number), false for a standard capacity card
 * (addressed by byte). Returns false if the card does not answer, reports an
 * error, or stays busy too long. The card is always left deselected. */
static bool sd_block_read(uint32_t lba, uint32_t count, uint8_t *buffer, bool sdhc)
{
    bool ok = true;

    if (count == 0)
        return false;

    gpio_put(SD_CS, 0);
    if (sd_send_command(count == 1 ? SD_CMD17 : SD_CMD18, sd_address_of(lba, sdhc)) != 0)
    {
        sd_deselect();
        return false;
    }

    for (uint32_t i = 0; i < count && ok; i++)
    {
        ok = sd_wait_data_token();
        if (ok)
        {
            spi_read_blocking(SD_SPI, SD_IDLE_BYTE, buffer + i * DRIVE_BLOCK_SIZE, DRIVE_BLOCK_SIZE);
            (void)sd_read_byte(); /* the block's checksum, not checked */
            (void)sd_read_byte();
        }
    }

    /* A multiple block read runs until it is stopped. */
    if (count > 1 && !sd_stop_transmission())
        ok = false;

    sd_deselect();
    return ok;
}

/* Writes `count` whole 512 byte blocks to block `lba`; the same rules as sd_block_read(). */
static bool sd_block_write(uint32_t lba, uint32_t count, const uint8_t *buffer, bool sdhc)
{
    bool ok = true;
    uint8_t token = (count == 1) ? SD_DATA_START_BLOCK : SD_DATA_START_BLOCK_MULT;

    if (count == 0)
        return false;

    gpio_put(SD_CS, 0);

    if (count >= SD_PREERASE_MIN_BLOCKS)
    {
        /* Tell the card how many blocks are coming so it can erase them ahead (a hint only). */
        (void)sd_send_command(SD_CMD55, 0);
        (void)sd_send_command(SD_ACMD23, count);
    }

    if (sd_send_command(count == 1 ? SD_CMD24 : SD_CMD25, sd_address_of(lba, sdhc)) != 0)
    {
        sd_deselect();
        return false;
    }

    for (uint32_t i = 0; i < count && ok; i++)
    {
        if (!sd_wait_not_busy(SD_BUSY_TIMEOUT_MS)) /* the card is still writing the previous block */
        {
            ok = false;
            break;
        }
        sd_write_byte(token);
        spi_write_blocking(SD_SPI, buffer + i * DRIVE_BLOCK_SIZE, DRIVE_BLOCK_SIZE);
        sd_write_byte(SD_IDLE_BYTE); /* the block's checksum, not checked */
        sd_write_byte(SD_IDLE_BYTE);

        if ((sd_read_byte() & SD_DATA_RESPONSE_MASK) != SD_DATA_RESPONSE_ACCEPTED)
            ok = false;
    }

    if (count > 1 && ok)
    {
        /* Tell the card the run of blocks is over, then wait for it to finish. */
        (void)sd_wait_not_busy(SD_BUSY_TIMEOUT_MS);
        sd_write_byte(SD_DATA_STOP_MULT);
        (void)sd_read_byte(); /* one stuff byte follows the stop token */
    }
    else if (count > 1)
    {
        /* A block was refused or the card stopped answering: the specification wants CMD12 here. */
        (void)sd_stop_transmission();
    }
    if (!sd_wait_not_busy(SD_BUSY_TIMEOUT_MS)) /* until the last block is really written */
        ok = false;

    sd_deselect();
    return ok;
}

/* ======================================================================== */
/* 2. The media: the card in the slot                                        */
/* ======================================================================== */

/*
 * Everything the rest of the program knows about the card: is one in, how big is
 * it, read blocks, write blocks. Starting the card is done by the starter's
 * sd_card_init(). That driver has no call for the card's size, which a USB drive
 * must report, so media_read_csd() asks the card for it directly (SD command 9)
 * right after the driver has started the card.
 *
 * A wrong size would make the PC see the wrong disk, so the size register is
 * only believed after its own checksum (CRC7) is right and two reads in a row
 * give exactly the same bytes. The last register read is kept for the status
 * screen.
 */

static media_state_t media_state_now = MEDIA_NONE;
static uint32_t media_blocks = 0;
static uint32_t media_changes = 0;      /* goes up each time a card is inserted or removed */
static uint32_t media_last_poll_ms = 0;
static uint32_t media_last_attempt_ms = 0;
static uint8_t media_consecutive_failures = 0; /* failed transfers in a row, see media_note_failure() */

static uint8_t media_last_csd[CSD_BYTES]; /* the most recent size register read */
static uint8_t media_csd_read_count = 0;/* how many reads the last card start needed */
static bool media_csd_was_checked = false; /* the last start ended with a believed size register */

/* The 7 bit checksum the SD specification uses (polynomial x^7 + x^3 + 1). */
static uint8_t media_crc7(const uint8_t *data, int length)
{
    uint8_t crc = 0;

    for (int i = 0; i < length; i++)
    {
        uint8_t bits = data[i];
        for (int j = 0; j < 8; j++)
        {
            crc <<= 1;
            if ((bits & 0x80) ^ (crc & 0x80))
                crc ^= SD_CRC7_POLYNOMIAL;
            bits <<= 1;
        }
    }
    return crc & 0x7F;
}

/* The last byte of the size register is its own checksum in the top 7 bits and
 * a 1 in the bottom bit. */
static bool media_csd_crc_ok(const uint8_t csd[CSD_BYTES])
{
    return (csd[CSD_BYTES - 1] & 1) == 1 && (csd[CSD_BYTES - 1] >> 1) == media_crc7(csd, CSD_BYTES - 1);
}

/* Sends SEND_CSD and reads the 16 byte card-specific data. Returns false if the
 * card does not answer in time. */
static bool media_read_csd(uint8_t csd[CSD_BYTES])
{
    static const uint8_t command[6] = {SD_COMMAND_START_BITS | SD_CMD9, 0, 0, 0, 0, SD_COMMAND_DUMMY_CRC};
    uint8_t reply = SD_IDLE_BYTE;
    uint8_t token = SD_IDLE_BYTE;
    absolute_time_t deadline = make_timeout_time_ms(MEDIA_CSD_TIMEOUT_MS);

    gpio_put(SD_CS, 0);
    if (!sd_wait_not_busy(MEDIA_CSD_TIMEOUT_MS)) /* the card may still be busy from what it was doing before */
    {
        gpio_put(SD_CS, 1);
        return false;
    }
    spi_write_blocking(SD_SPI, command, sizeof(command));

    /* The card answers within a few bytes: a first byte with the top bit clear. */
    for (int i = 0; i < SD_REPLY_TRIES && (reply & SD_REPLY_NOT_YET_BIT); i++)
        spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &reply, 1);
    if (reply != 0)
    {
        gpio_put(SD_CS, 1);
        return false;
    }

    /* Then the data start token, followed by the 16 bytes and a 2 byte CRC. */
    while (token != SD_DATA_START_BLOCK)
    {
        if (time_reached(deadline))
        {
            gpio_put(SD_CS, 1);
            return false;
        }
        spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &token, 1);
    }
    spi_read_blocking(SD_SPI, SD_IDLE_BYTE, csd, CSD_BYTES);
    spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &reply, 1);
    spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &reply, 1);

    gpio_put(SD_CS, 1);
    spi_read_blocking(SD_SPI, SD_IDLE_BYTE, &reply, 1); /* the card needs a few more clocks to let go of the bus */
    return true;
}

/* Reads the size register until two reads in a row both have a correct checksum
 * and give the same bytes, at most MEDIA_CSD_TRIES reads. */
static bool media_read_csd_checked(uint8_t csd[CSD_BYTES])
{
    uint8_t previous[CSD_BYTES];
    bool have_previous = false;

    media_csd_read_count = 0;
    media_csd_was_checked = false;

    while (media_csd_read_count < MEDIA_CSD_TRIES)
    {
        media_csd_read_count++;
        if (!media_read_csd(csd))
        {
            have_previous = false;
            continue;
        }
        memcpy(media_last_csd, csd, CSD_BYTES);

        if (!media_csd_crc_ok(csd))
        {
            have_previous = false; /* a damaged read: start the comparison again */
            continue;
        }
        if (have_previous && memcmp(previous, csd, CSD_BYTES) == 0)
        {
            media_csd_was_checked = true;
            return true;
        }
        memcpy(previous, csd, CSD_BYTES);
        have_previous = true;
    }
    return false;
}

/* Number of DRIVE_BLOCK_SIZE blocks described by a CSD register, or 0 if it
 * looks wrong. */
static uint32_t media_blocks_from_csd(const uint8_t csd[CSD_BYTES])
{
    uint32_t structure = csd[0] >> 6;

    if (structure == 1)
    {
        /* CSD version 2 (SDHC and SDXC): size is (C_SIZE + 1) * 512 KiB. The largest
         * value is exactly 2^32 blocks, one more than the 32 bit block count can
         * hold, so it is cut to the largest count the USB drive can describe. */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        uint64_t blocks = ((uint64_t)c_size + 1) * 1024;

        return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
    }
    if (structure != 0)
        return 0; /* a layout this program does not know */

    /* CSD version 1 (standard capacity). */
    uint32_t read_bl_len = csd[5] & 0x0F;
    uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03) << 1) | (csd[10] >> 7);
    uint32_t blocks = (c_size + 1) << (c_size_mult + 2);

    if (read_bl_len < 9 || read_bl_len > 11)
        return 0;
    return blocks << (read_bl_len - 9); /* convert the card's own block length to 512 byte blocks */
}

/* Starts the card in the slot and learns its size. */
static void media_start_card(void)
{
    uint8_t csd[CSD_BYTES];

    media_last_attempt_ms = to_ms_since_boot(get_absolute_time());

    if (sd_card_init() == SD_OK && media_read_csd_checked(csd))
    {
        media_blocks = media_blocks_from_csd(csd);
        media_state_now = (media_blocks > 0) ? MEDIA_READY : MEDIA_ERROR;
    }
    else
    {
        media_state_now = MEDIA_ERROR;
    }
    media_changes++;
}

/* Sets up the card slot hardware. Call once at startup. */
static void media_init(void)
{
    sd_init();
}

/* Watches the card-detect switch and starts or drops the card as it is inserted
 * or removed. Call from the main loop; it limits its own rate. */
static void media_poll(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool present;

    if (now - media_last_poll_ms < MEDIA_POLL_MS)
        return;
    media_last_poll_ms = now;

    present = sd_card_present();

    if (!present && media_state_now != MEDIA_NONE)
    {
        media_state_now = MEDIA_NONE;
        media_blocks = 0;
        media_changes++;
    }
    else if (present && media_state_now == MEDIA_NONE)
    {
        media_start_card();
    }
    else if (present && media_state_now == MEDIA_ERROR && now - media_last_attempt_ms >= MEDIA_RETRY_MS)
    {
        media_start_card(); /* try a card that failed to start again, it may just have been seated badly */
    }
}

/* Number of DRIVE_BLOCK_SIZE blocks on the card; 0 unless the card is ready. */
static uint32_t media_block_count(void)
{
    return (media_state_now == MEDIA_READY) ? media_blocks : 0;
}

/* The last size register read from the card, as 32 hex characters (needs 33 bytes). */
static void media_csd_text(char *out, size_t out_size)
{
    size_t used = 0;

    if (out_size == 0)
        return;
    out[0] = '\0';
    for (int i = 0; i < CSD_BYTES && used + 3 <= out_size; i++)
        used += (size_t)snprintf(out + used, out_size - used, "%02X", media_last_csd[i]);
}

/* Reports the card as not ready to the PC and leaves it to media_poll() to start
 * it again from scratch. */
static void media_give_up_on_card(void)
{
    media_consecutive_failures = 0;
    media_state_now = MEDIA_ERROR;
    media_last_attempt_ms = to_ms_since_boot(get_absolute_time());
    media_changes++;
}

/* Called after a transfer failed twice (the second time after restarting the
 * card). A card that keeps failing is reported "not ready" to the PC and started
 * again from scratch by media_poll(), instead of failing for ever. */
static void media_note_failure(void)
{
    if (++media_consecutive_failures >= MEDIA_FAIL_LIMIT)
        media_give_up_on_card();
}

/* Restarts the card after a failed transfer and checks it is still the card we
 * know: a card swapped between two looks of media_poll() would otherwise keep the
 * old size. If it is a different size the drive drops to the error state, which
 * tells the PC the disc changed, and media_poll() starts the card from scratch. */
static bool media_restart_same_card(void)
{
    uint8_t csd[CSD_BYTES];

    if (sd_card_init() != SD_OK || !media_read_csd_checked(csd))
        return false;
    if (media_blocks_from_csd(csd) != media_blocks)
    {
        media_give_up_on_card();
        return false;
    }
    return true;
}

/* Reads (write false) or writes (write true) `count` whole blocks at block `lba`.
 * Returns false if there is no ready card, the range runs off the end of the card,
 * or the card reports an error. `lba` is 64 bit so that a start block near the top
 * of the 32 bit range plus an offset cannot wrap round into a valid range. On a
 * failure the card is restarted and the transfer tried once more. */
static bool media_transfer(bool write, uint64_t lba, uint32_t count, uint8_t *buffer)
{
    bool sdhc;

    if (media_state_now != MEDIA_READY || count == 0 || lba >= media_blocks || count > media_blocks - lba)
        return false;

    sdhc = sd_is_sdhc();
    if ((write ? sd_block_write((uint32_t)lba, count, buffer, sdhc) : sd_block_read((uint32_t)lba, count, buffer, sdhc)) ||
        (media_restart_same_card() && (write ? sd_block_write((uint32_t)lba, count, buffer, sd_is_sdhc())
                                             : sd_block_read((uint32_t)lba, count, buffer, sd_is_sdhc()))))
    {
        media_consecutive_failures = 0;
        return true;
    }
    if (media_state_now == MEDIA_READY)
        media_note_failure();
    return false;
}

/* ======================================================================== */
/* 3. Drive state                                                            */
/* ======================================================================== */

/* Speed is the average over a burst of transfers: the blocks moved divided by the
 * time the card was busy. A burst ends when the drive has been idle for
 * SPEED_BURST_GAP_MS, so the figure on the screen belongs to the last copy. */
typedef struct
{
    uint32_t last_ms;   /* when the burst last moved data */
    uint64_t blocks;
    uint64_t busy_us;
} burst_t;

static burst_t read_burst;
static burst_t write_burst;

/* Adds a transfer to a burst and returns the burst's average speed in KB/s. */
static uint32_t drive_burst_add(burst_t *b, uint32_t blocks, uint32_t now_ms, uint32_t busy_us)
{
    if (b->last_ms != 0 && now_ms - b->last_ms > SPEED_BURST_GAP_MS)
    {
        b->blocks = 0;
        b->busy_us = 0;
    }
    b->last_ms = now_ms ? now_ms : 1;
    b->blocks += blocks;
    b->busy_us += busy_us;

    if (b->busy_us == 0)
        return 0;
    /* a block is half a KB: KB/s = blocks / 2 * 1,000,000 / busy_us */
    return (uint32_t)(b->blocks * 500000ull / b->busy_us);
}

/* The PC asked to eject (true) or to load (false) the drive, or the user pressed the key. */
static void drive_set_ejected(bool ejected)
{
    if (drive.ejected != ejected)
    {
        drive.ejected = ejected;
        drive.epoch++; /* tells the USB side to report the change to the PC */
    }
}

/* Eject, or re-mount if already ejected. */
static void drive_toggle_eject(void)
{
    drive_set_ejected(!drive.ejected);
}

static void drive_toggle_write_protect(void)
{
    drive.write_protect = !drive.write_protect;
}

/* Count a successful or failed transfer. `now_ms` is the current time and
 * `busy_us` how long the SD card took, which feeds the speed figures. */
static void drive_note_read(uint32_t blocks, uint32_t now_ms, uint32_t busy_us)
{
    drive.blocks_read += blocks;
    drive.last_read_ms = now_ms ? now_ms : 1; /* 0 means "never" */
    drive.read_kbps = drive_burst_add(&read_burst, blocks, now_ms, busy_us);
}

static void drive_note_write(uint32_t blocks, uint32_t now_ms, uint32_t busy_us)
{
    drive.blocks_written += blocks;
    drive.last_write_ms = now_ms ? now_ms : 1;
    drive.write_kbps = drive_burst_add(&write_burst, blocks, now_ms, busy_us);
}

static void drive_note_error(void)
{
    drive.errors++;
}

/* ======================================================================== */
/* 4. USB descriptors                                                        */
/* ======================================================================== */

/* The USB descriptors that make the PicoCalc appear on the PC as a USB drive
 * (mass storage, bulk-only transport). The identity and layout constants are in
 * the header. Structure follows TinyUSB's own reference mass storage example. */

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    /* The class is given by the interface (mass storage), not the device. */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD_DEVICE,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = STRING_SERIAL_INDEX,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

static uint8_t const desc_fs_configuration[] = {
    /* Config number, interface count, string index, total length, attribute, power in mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* Interface number, string index, endpoint out, endpoint in, endpoint size */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRING_MSC_INDEX, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

static char const *const string_desc_arr[] = {
    NULL,                      /* 0: language ID, handled specially below */
    USB_MANUFACTURER_TEXT,     /* 1: manufacturer */
    USB_DEVICE_NAME_TEXT,      /* 2: product (shown by the PC) */
    NULL,                      /* 3: serial number, filled in from the chip's unique ID */
    USB_DEVICE_NAME_TEXT,      /* 4: mass storage interface */
};

/* PICO_UNIQUE_BOARD_ID_SIZE_BYTES is 8; as a hex string that is 16 characters plus the NUL. */
static char serial_number[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static bool serial_ready = false;

static uint16_t desc_str_buf[1 + USB_STRING_MAX_CHARS]; /* the length/type word, then the characters */

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    uint8_t char_count;
    (void)langid;

    if (index == 0)
    {
        desc_str_buf[1] = USB_LANGID_ENGLISH_US;
        char_count = 1;
    }
    else
    {
        const uint8_t string_desc_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
        char const *str;

        if (index == STRING_SERIAL_INDEX)
        {
            if (!serial_ready)
            {
                pico_get_unique_board_id_string(serial_number, sizeof(serial_number));
                serial_ready = true;
            }
            str = serial_number;
        }
        else
        {
            str = (index < string_desc_count) ? string_desc_arr[index] : NULL;
        }
        if (str == NULL)
            return NULL;

        char_count = (uint8_t)strlen(str);
        if (char_count > USB_STRING_MAX_CHARS)
            char_count = USB_STRING_MAX_CHARS;
        for (uint8_t i = 0; i < char_count; i++)
            desc_str_buf[1 + i] = (uint16_t)str[i];
    }

    desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * char_count + 2));
    return desc_str_buf;
}

/* ======================================================================== */
/* 5. USB mass storage                                                       */
/* ======================================================================== */

/* TinyUSB's mass storage class handles the SCSI protocol; the callbacks here
 * answer its questions (is there a disc, how big, read this, write that) from the
 * SD card and the drive state. */

/* The media change the PC has already been told about. */
static uint32_t reported_change = 0;

/* Starts the USB device. Call once at startup. */
static void usb_msc_init(void)
{
    tud_init(BOARD_TUD_RHPORT);
}

/* Runs the USB stack. Call every pass of the main loop, at least every few
 * milliseconds; a long delay elsewhere makes the PC time out. */
static void usb_msc_task(void)
{
    tud_task();
    drive.pc_connected = tud_mounted();
}

/* The sum of everything that counts as a change of disc from the PC's point of
 * view: the card being inserted or removed, and the drive being ejected or
 * re-mounted. */
static uint32_t usb_msc_epoch(void)
{
    return media_changes + drive.epoch;
}

/* Answers the PC's "who are you?" question. Fields are space padded, not NUL terminated. */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    static const char vendor[] = USB_VENDOR_TEXT;
    static const char product[] = USB_PRODUCT_TEXT;
    static const char revision[] = SD_DRIVE_USB_REVISION;

    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, vendor, strlen(vendor) < 8 ? strlen(vendor) : 8);
    memcpy(product_id, product, strlen(product) < 16 ? strlen(product) : 16);
    memcpy(product_rev, revision, strlen(revision) < 4 ? strlen(revision) : 4);
}

/* Is there a disc in the drive? */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (media_state_now != MEDIA_READY || drive.ejected)
    {
        reported_change = usb_msc_epoch(); /* the PC will see the change when the disc is ready again */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return false;
    }

    if (reported_change != usb_msc_epoch())
    {
        /* A disc has appeared since the PC last looked: tell it once, so it rereads the card. */
        reported_change = usb_msc_epoch();
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, ASC_MEDIA_CHANGED, 0x00);
        return false;
    }
    return true;
}

/* How big is the disc? */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = media_block_count();
    *block_size = DRIVE_BLOCK_SIZE;
}

/* The PC asks to eject (start false, load_eject true) or load (both true). */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject)
        drive_set_ejected(!start);
    return true;
}

/* The drive's write-protect switch, reported to the PC. */
bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return !drive.write_protect;
}

/* The checks a read or a write both start with: there is a disc in the drive, and
 * the transfer starts and ends on a block boundary (the transfer buffer,
 * CFG_TUD_MSC_EP_BUFSIZE, is a whole number of blocks, so TinyUSB's always do).
 * Returns false after setting the error the PC is told about. */
static bool usb_msc_transfer_ok(uint8_t lun, uint32_t offset, uint32_t bufsize)
{
    if (media_state_now != MEDIA_READY || drive.ejected)
    {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return false;
    }
    return (offset % DRIVE_BLOCK_SIZE) == 0 && (bufsize % DRIVE_BLOCK_SIZE) == 0;
}

/* The PC wants bufsize bytes starting at block lba. Returns the number of bytes
 * provided, or a negative number for an error. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint32_t blocks = bufsize / DRIVE_BLOCK_SIZE;
    uint64_t started_us;
    uint32_t busy_us;
    bool read_ok;

    if (!usb_msc_transfer_ok(lun, offset, bufsize))
        return -1;

    started_us = time_us_64();
    read_ok = media_transfer(false, (uint64_t)lba + offset / DRIVE_BLOCK_SIZE, blocks, buffer);
    busy_us = (uint32_t)(time_us_64() - started_us);

    if (!read_ok)
    {
        drive_note_error();
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, 0x00);
        return -1;
    }
    drive_note_read(blocks, to_ms_since_boot(get_absolute_time()), busy_us);
    return (int32_t)bufsize;
}

/* The PC sends bufsize bytes to go at block lba. Returns the number of bytes
 * accepted, or a negative number for an error. */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    uint32_t blocks = bufsize / DRIVE_BLOCK_SIZE;
    uint64_t started_us;
    uint32_t busy_us;
    bool write_ok;

    if (media_state_now == MEDIA_READY && !drive.ejected && drive.write_protect)
    {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0x00);
        return -1;
    }
    if (!usb_msc_transfer_ok(lun, offset, bufsize))
        return -1;

    started_us = time_us_64();
    write_ok = media_transfer(true, (uint64_t)lba + offset / DRIVE_BLOCK_SIZE, blocks, buffer);
    busy_us = (uint32_t)(time_us_64() - started_us);

    if (!write_ok)
    {
        drive_note_error();
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, 0x00);
        return -1;
    }
    drive_note_write(blocks, to_ms_since_boot(get_absolute_time()), busy_us);
    return (int32_t)bufsize;
}

/* Any SCSI command TinyUSB does not handle itself: not supported. */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;

    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0x00);
    return -1;
}

/* ======================================================================== */
/* 6. Status screen                                                          */
/* ======================================================================== */

/*
 * What the drive is doing, on the PicoCalc's LCD: whether the PC is connected, the
 * card, the write-protect switch, read and write counts and a live activity light,
 * and a reminder of the keys that control it. Drawing is done with the starter's
 * LCD driver (lcd.c by Blair Leduc). Each screen row is built as text, compared
 * with what that row showed last time, and redrawn only if it differs, so the
 * screen does not flicker and the LCD driver, which switches interrupts off while
 * it writes, is not kept busy.
 */

static char ui_shown[UI_ROWS][UI_LINE_MAX];     /* what each row currently shows */
static uint16_t ui_shown_color[UI_ROWS];        /* and in which colour */
static uint32_t ui_last_update_ms = 0;

/* Draws `text` on `row` in `color` unless the row already shows exactly that. */
static void ui_show_row(int row, uint16_t color, const char *text)
{
    if (ui_shown_color[row] == color && strcmp(ui_shown[row], text) == 0)
        return;

    snprintf(ui_shown[row], sizeof(ui_shown[row]), "%s", text);
    ui_shown_color[row] = color;

    lcd_erase_line((uint8_t)row, 0, UI_COLUMNS - 1);
    lcd_set_foreground(color);
    lcd_putstr(0, (uint8_t)row, text);
}

/* "14.8 GB" or "512 MB" for a number of 512 byte blocks. */
static void ui_format_size(char *out, size_t out_size, uint32_t blocks)
{
    uint32_t mb = blocks / 2048;

    if (mb >= 1024)
        snprintf(out, out_size, "%lu.%lu GB", (unsigned long)(mb / 1024), (unsigned long)((mb % 1024) * 10 / 1024));
    else
        snprintf(out, out_size, "%lu MB", (unsigned long)mb);
}

/* True if `last_ms` (0 = never) is recent enough to show as activity right now. */
static bool ui_recently(uint32_t last_ms, uint32_t now)
{
    return last_ms != 0 && (now - last_ms) < ACTIVITY_HOLD_MS;
}

/* Redraws whatever changed. Call from the main loop; it limits its own rate. */
static void ui_update(void)
{
    char line[UI_LINE_MAX];
    char size[UI_SIZE_TEXT_MAX];
    char csd_text[UI_CSD_TEXT_MAX];
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - ui_last_update_ms < UI_REFRESH_MS)
        return;
    ui_last_update_ms = now;

    snprintf(line, sizeof(line), "PicoCalc SD Drive %s", SD_DRIVE_VERSION);
    ui_show_row(UI_ROW_TITLE, UI_COLOR_TITLE, line);

    ui_show_row(UI_ROW_PC, drive.pc_connected ? UI_COLOR_GOOD : UI_COLOR_WARNING,
                drive.pc_connected ? "PC:     connected" : "PC:     not connected");

    switch (media_state_now)
    {
    case MEDIA_READY:
        ui_format_size(size, sizeof(size), media_block_count());
        snprintf(line, sizeof(line), "Card:   %s %s", size, sd_is_sdhc() ? "SDHC" : "SD");
        ui_show_row(UI_ROW_CARD, UI_COLOR_GOOD, line);
        break;
    case MEDIA_ERROR:
        ui_show_row(UI_ROW_CARD, UI_COLOR_BAD, "Card:   cannot be read");
        break;
    default:
        ui_show_row(UI_ROW_CARD, UI_COLOR_WARNING, "Card:   none inserted");
        break;
    }

    if (media_state_now != MEDIA_READY)
        ui_show_row(UI_ROW_DRIVE, UI_COLOR_WARNING, "Drive:  no media");
    else if (drive.ejected)
        ui_show_row(UI_ROW_DRIVE, UI_COLOR_WARNING, "Drive:  ejected");
    else
        ui_show_row(UI_ROW_DRIVE, UI_COLOR_GOOD, "Drive:  ready");

    if (drive.write_protect)
        ui_show_row(UI_ROW_WRITE, UI_COLOR_WARNING, "Writes: protected");
    else
        ui_show_row(UI_ROW_WRITE, UI_COLOR_GOOD, "Writes: allowed");

    snprintf(line, sizeof(line), "Read:   %lu KB", (unsigned long)(drive.blocks_read / 2));
    ui_show_row(UI_ROW_READ_COUNT, UI_COLOR_LABEL, line);
    snprintf(line, sizeof(line), "Written:%lu KB", (unsigned long)(drive.blocks_written / 2));
    ui_show_row(UI_ROW_WRITE_COUNT, UI_COLOR_LABEL, line);
    snprintf(line, sizeof(line), "Errors: %lu", (unsigned long)drive.errors);
    ui_show_row(UI_ROW_ERRORS, drive.errors ? UI_COLOR_BAD : UI_COLOR_LABEL, line);

    if (ui_recently(drive.last_write_ms, now))
        ui_show_row(UI_ROW_ACTIVITY, UI_COLOR_WRITE, "[ WRITING ]");
    else if (ui_recently(drive.last_read_ms, now))
        ui_show_row(UI_ROW_ACTIVITY, UI_COLOR_READ, "[ READING ]");
    else
        ui_show_row(UI_ROW_ACTIVITY, UI_COLOR_LABEL, "[ idle    ]");

    if (drive.read_kbps == 0 && drive.write_kbps == 0)
        ui_show_row(UI_ROW_SPEED, UI_COLOR_LABEL, "Speed:  --");
    else
    {
        snprintf(line, sizeof(line), "Speed:  R %lu KB/s  W %lu KB/s", (unsigned long)drive.read_kbps, (unsigned long)drive.write_kbps);
        ui_show_row(UI_ROW_SPEED, UI_COLOR_LABEL, line);
    }

    /* Diagnostics: the raw size register the card gave and how sure we are of it. */
    media_csd_text(csd_text, sizeof(csd_text));
    snprintf(line, sizeof(line), "CSD: %s", csd_text);
    ui_show_row(UI_ROW_CSD, UI_COLOR_LABEL, line);

    if (media_state_now == MEDIA_NONE)
        ui_show_row(UI_ROW_CSD_STATUS, UI_COLOR_LABEL, "Size: no card");
    else
    {
        snprintf(line, sizeof(line), "Size: %lu blocks, %s (%u reads)", (unsigned long)media_block_count(),
                 media_csd_was_checked ? "checked" : "NOT CHECKED", (unsigned)media_csd_read_count);
        ui_show_row(UI_ROW_CSD_STATUS, media_csd_was_checked ? UI_COLOR_GOOD : UI_COLOR_BAD, line);
    }

    ui_show_row(UI_ROW_KEYS, UI_COLOR_LABEL, "E  eject / mount again");
    ui_show_row(UI_ROW_KEYS + 1, UI_COLOR_LABEL, "W  write protect on / off");
    ui_show_row(UI_ROW_KEYS + 2, UI_COLOR_LABEL, "~  reboot to BOOTSEL (flash new .uf2)");
}

/* Starts the LCD and draws the screen. Call once at startup. */
static void ui_init(void)
{
    lcd_init();
    lcd_enable_cursor(false); /* the starter blinks a text cursor by default */
    lcd_set_background(UI_COLOR_BACKGROUND);
    lcd_clear_screen();

    memset(ui_shown, 0, sizeof(ui_shown));
    memset(ui_shown_color, 0, sizeof(ui_shown_color));
    ui_update();
}
