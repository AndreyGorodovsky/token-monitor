/* GC9A01 round TFT driver -- 1.28", 240x240, RGB565, SPI.
 *
 * Init and one solid fill was the whole of it at stage 6, deliberately: if
 * the screen had stayed dark, no drawing code existed yet to be a suspect.
 * Stage 7 added the rest -- rectangles and text -- on the same foundation.
 *
 * Everything here is built from one idea: set an address window on the panel,
 * then stream pixels into it. A rectangle and a character differ only in how
 * the bytes are computed.
 *
 * Written against plain `spi_master` + `gpio` rather than `esp_lcd` plus a
 * Component Registry driver: the point of the esp_lcd route would have been
 * to avoid writing a GC9A01 driver, and a working one already existed --
 * proven on this exact panel by a standalone solid-fill test before any of it
 * came near this project. See ../../ARCHITECTURE.md for that reasoning and
 * for the pinout.
 *
 * NOT THREAD-SAFE, and not made so. The SPI device handle, the row buffer and
 * the address window are all shared mutable state; two tasks drawing at once
 * would interleave pixel data mid-frame, and because the panel tracks its own
 * write position the damage would not be confined to one of them.
 *
 * As of stage 7 two tasks *do* call in here -- app_main draws the boot
 * messages, and the fetch task draws everything after. That is safe only
 * because they never overlap: app_main stops drawing before it creates the
 * fetch task, and never draws again. This is an ordering guarantee held by
 * convention, with nothing enforcing it. Any third caller, or a refresh loop
 * that draws while app_main still might, needs a mutex first.
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

/* Any other colour, from ordinary 8-bit components. The low bits are simply
 * dropped -- 8:8:8 does not fit in 5:6:5 -- so nearby shades collapse
 * together, which matters for gradients and not at all for flat UI colour. */
#define GC9A01_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | \
                                        (((g) & 0xFC) << 3) | \
                                        (((b) & 0xF8) >> 3)))

/* Text metrics. The font is 5x7 with a one-pixel spacer column baked into the
 * advance, so a character cell is 6x7 and a string of n characters occupies
 * n*6*scale pixels including the trailing spacer. `scale` is an integer pixel
 * multiplier: scale 1 is 5x7, scale 5 is a 25x35 glyph in a 30x35 cell.
 * Integer scaling keeps the font a single 295-byte table instead of one table
 * per size, at the cost of visible blockiness at large scales. */
#define GC9A01_CHAR_W          6
#define GC9A01_CHAR_H          7
#define GC9A01_MAX_TEXT_SCALE  8

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
 * degrade visibly rather than die. The same applies to everything below. */
void gc9a01_fill_screen(uint16_t color565);

/* Fills a rectangle. Clipped to the panel, so partly- or wholly-offscreen
 * rectangles are safe and simply draw less.
 *
 * This is also the primitive that makes partial redraws possible: repainting
 * only the region that changed, rather than the whole screen, is what keeps a
 * once-a-minute refresh from visibly flashing. */
void gc9a01_fill_rect(int x, int y, int w, int h, uint16_t color565);

/* Draws text with an opaque background, top-left anchored at (x, y).
 *
 * Opaque rather than transparent on purpose: it means redrawing a changed
 * value over its old one needs no separate clear step, and so cannot flicker
 * between the two.
 *
 * Lowercase input is mapped to uppercase -- the table holds one case, which
 * halves it, and this display shows numbers and short labels. Characters with
 * no glyph (punctuation outside the table, or any byte of a multi-byte UTF-8
 * sequence -- a non-English weekday abbreviation from the PC, for instance)
 * draw as a conspicuous box, so a text problem looks like a text problem.
 *
 * NOT clipped, unlike fill_rect: a character that would fall outside the panel
 * is skipped entirely rather than half-drawn. Position text within bounds.
 * `scale` is clamped to 1..GC9A01_MAX_TEXT_SCALE, since it sizes a fixed
 * internal buffer. */
void gc9a01_draw_text(int x, int y, const char *text,
                      uint16_t fg565, uint16_t bg565, int scale);

/* Width in pixels of `text` at `scale`, excluding the trailing spacer column,
 * so that centring on it looks centred. Height is always
 * GC9A01_CHAR_H * scale.
 *
 * Careful with right-alignment: this reports *ink*, but gc9a01_draw_text
 * needs the full six-column cell of every glyph to be on-screen. Placing text
 * at `x = 240 - gc9a01_text_width(...)` therefore pushes the last cell one
 * scale-step past the edge, and that glyph is dropped -- this function says it
 * fits, and the renderer disagrees. For alignment against a right-hand edge,
 * measure the advance instead: strlen * GC9A01_CHAR_W * scale. */
int gc9a01_text_width(const char *text, int scale);
