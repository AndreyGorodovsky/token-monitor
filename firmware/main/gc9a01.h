/* GC9A01 round TFT driver -- 1.28", 240x240, RGB565, SPI.
 *
 * Stage 6's whole job is this file and its .c: get the panel lit inside the
 * real firmware, with nothing else in the way. The interface is deliberately
 * two functions wide for now. Stage 7 adds the drawing primitives (rectangle
 * fill, text, arcs) that render actual usage data; keeping stage 6 to "init
 * and fill" means that if the screen stays dark, no drawing code exists yet
 * to be a suspect.
 *
 * Written against plain `spi_master` + `gpio` rather than `esp_lcd` plus a
 * Component Registry driver: the point of the esp_lcd route would have been
 * to avoid writing a GC9A01 driver, and a working one already existed --
 * proven on this exact panel by a standalone solid-fill test before any of it
 * came near this project. See ../../ARCHITECTURE.md for that reasoning and
 * for the pinout.
 *
 * Not thread-safe, and not made so: every call must come from one task. The
 * SPI device handle and the row buffer inside the .c are shared mutable
 * state, and two tasks drawing at once would interleave pixel data mid-frame.
 * Today only app_main draws; when stage 8 adds a refresh loop, drawing stays
 * on a single task.
 */
#pragma once

#include <stdint.h>

/* The panel is 240x240. It is also *round*, which the controller knows
 * nothing about: the addressable framebuffer is the full square, and the
 * corners simply aren't visible behind the bezel. Stage 7 has to keep
 * anything that matters inside the inscribed circle -- the controller will
 * happily accept pixels you will never see. */
#define GC9A01_WIDTH  240
#define GC9A01_HEIGHT 240

/* Colours are RGB565, big-endian on the wire (the .c handles the byte
 * order). Red and blue are 5 bits, green 6 -- the extra green bit is not a
 * typo, it is where the human eye is most sensitive. */
#define GC9A01_BLACK   0x0000
#define GC9A01_WHITE   0xFFFF
#define GC9A01_RED     0xF800
#define GC9A01_GREEN   0x07E0
#define GC9A01_BLUE    0x001F

/* Configures the SPI bus, resets the panel, and sends the vendor power-on
 * sequence. Call once, before anything else here; a second call logs a
 * warning and returns rather than re-initializing (or panicking).
 *
 * Aborts on failure via ESP_ERROR_CHECK -- covering both the bus setup and
 * every byte of the init sequence. That is the right call for setup: a bad
 * SPI configuration is a programming mistake, and a panel that was only
 * half-configured has no meaningful state to continue from. (Network calls in
 * this project are handled the opposite way, and for the opposite reason.)
 *
 * What it cannot detect is a *wiring* fault. SPI writes are unacknowledged,
 * so this function succeeds just as cheerfully into a disconnected panel:
 * only your eyes can confirm that part. */
void gc9a01_init(void);

/* Fills all 240x240 pixels with one colour.
 *
 * Unlike init, a failed transfer here is logged (once) and not fatal. A desk
 * gadget that panics because one row of pixels did not go out is worse than
 * one showing a stale or partial screen -- and stage 8's whole job is to
 * degrade visibly rather than die. */
void gc9a01_fill_screen(uint16_t color565);
