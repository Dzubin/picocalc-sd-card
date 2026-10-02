# Changelog

All notable changes to PicoCalc-SD-Drive.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/);
the version here matches `SD_DRIVE_VERSION` in `PicoCalc-SD-Drive.h`.

## [Unreleased]

## [0.02B] - 2026-10-02

- Build outputs are named without the `picocalc-` prefix: `SD-Drive-RP2040.uf2` and
  `SD-Drive-RP2350.uf2` (the chip at the end already says what they are for).

## [0.02A] - 2026-10-01

- **Q**, **q** and **Esc** quit the program and return to the PicoCalc UF2 Loader's menu: the
  USB connection is dropped, the loader is asked for its menu through the watchdog scratch
  registers, and the chip is rebooted.
- The main screen lists the keys in short form (E, W, Q/Esc, ~).

## [0.01A] - 2026-10-01

First version: the PicoCalc appears on a PC as a USB drive backed by its SD card,
with a status screen and keys to eject and to write-protect it. Builds and runs on
both the RP2040 and the RP2350 PicoCalc.
