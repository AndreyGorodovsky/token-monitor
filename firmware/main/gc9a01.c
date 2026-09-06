/* GC9A01 driver implementation -- see gc9a01.h for what this is and why it
 * is hand-rolled rather than built on esp_lcd.
 *
 * Ported unchanged in every way that touches the panel from the standalone
 * bring-up test that proved this wiring and this init sequence on hardware.
 * That is deliberate: the timings, the register table and the 10 MHz clock
 * are all *known good* on this exact board, so stage 6 changes only the
 * surroundings (a real project, with a WiFi radio running alongside). If the
 * screen stays dark now, the difference is the environment, not the driver.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED:
 *
 *   1. pins and bus settings   -- the wiring, in code
 *   2. lcd_transmit            -- the one place any byte reaches the panel
 *   3. the vendor init table   -- copied, not derived; see the note above it
 *   4. gc9a01_init             -- reset, table, then standard MIPI commands
 *   5. fill_rect / fill_screen -- address window + a row buffer, h times
 *   6. text                    -- glyph lookup, then one window per character
 *
 * Everything drawable is built from one idea: set an address window, then
 * stream pixels into it. A rectangle and a character differ only in how the
 * bytes are computed.
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>              /* strlen, for gc9a01_text_width */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"       /* vTaskDelay -- the reset and sleep-out
                                  * delays below are required by the panel,
                                  * not padding                              */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "gc9a01.h"
#include "font5x7.h"             /* generated -- see tools/make_font.py */

static const char *TAG = "gc9a01";

/* The wiring, as confirmed against the board's own silkscreen. The full table
 * and its rationale live in ../../ARCHITECTURE.md; if the screen is dark,
 * check GND and VCC against the silkscreen *before* suspecting these, since a
 * power fault on this board shows as total darkness (the backlight is tied
 * straight to VCC with no control pin) while the firmware logs stay clean. */
#define PIN_SCLK GPIO_NUM_4   /* D2 -- SCL */
#define PIN_MOSI GPIO_NUM_5   /* D3 -- SDA */
#define PIN_RST  GPIO_NUM_6   /* D4 -- RES */
#define PIN_DC   GPIO_NUM_7   /* D5 -- DC  */

/* CS is on GPIO8, and GPIO8 is one of the ESP32-C3's three strapping pins
 * (with GPIO2 and GPIO9): it has to read HIGH at reset for the chip to enter
 * download mode. This display module carries an onboard pull-down on CS (R8,
 * per its silkscreen), which pulls that same line the wrong way.
 *
 * In practice it has not blocked flashing -- repeated `idf.py flash` runs with
 * the panel wired have entered download mode normally, so the pull-down is
 * evidently too weak to win at reset. But it is the leading suspect if flashing
 * ever fails with `Failed to connect ... No serial data received`, which has
 * happened once on this board and was worked around with hold-B/tap-R/release-B
 * (recorded in STATUS.md). It would explain why that was intermittent rather
 * than constant.
 *
 * If it becomes a recurring nuisance there are two cheap outs, and neither is
 * urgent enough to disturb a proven-good wiring today: move CS to a
 * non-strapping pin (D10/GPIO10 is unused here), or leave CS disconnected
 * entirely -- the module's own documentation says it works unwired, this being
 * the only device on the bus. */
#define PIN_CS   GPIO_NUM_8   /* D8 -- CS; see the strapping note above */

/* Conservative, and deliberately not raised yet. Many GC9A01 boards run
 * happily at 40 MHz, and stage 7 will want the extra speed once it is redrawing
 * regions -- but 10 MHz is the number this panel was *proven* at over jumper
 * wires, and changing it in the same step as moving the driver into a new
 * project would give a failure two suspects instead of one. Raise it later, on
 * its own, with the screen already working. */
#define SPI_CLOCK_HZ (10 * 1000 * 1000)

static spi_device_handle_t s_spi;

/* First SPI failure since boot, or ESP_OK. Sticky, and deliberately so.
 *
 * It is tempting to treat SPI writes as something that cannot fail -- the
 * panel never answers, so nothing on the wire can report a problem. But the
 * *host side* can still fail before a byte is ever clocked out, and that is
 * worth knowing about: see lcd_transmit for the specific case that bites here.
 *
 * Recording the first failure rather than the latest, and logging only that
 * one, keeps a broken bus from printing 240 identical lines during a single
 * screen fill while still making the failure impossible to miss. */
static esp_err_t s_err = ESP_OK;

/* --- the one place any byte reaches the panel ---------------------------- */

/* Every command and every pixel goes through here, so this is the single
 * place that has to get error handling right.
 *
 * polling_transmit rather than the queued/interrupt form: it busy-waits for
 * the transfer instead of blocking on a semaphore, which is faster for the
 * short bursts this driver sends and keeps the call order obvious. A
 * full-screen fill is 240 of these back to back.
 *
 * Its return value is NOT ignorable, which is easy to assume it is. The path
 * is spi_device_polling_transmit -> spi_device_polling_start ->
 * setup_priv_desc, and that last one allocates a bounce buffer with
 * heap_caps_aligned_alloc whenever the source is not DMA-capable. Our init
 * table is `static const`, so it lives in flash-mapped rodata, which is not
 * DMA-capable -- meaning all 42 init payloads take exactly that path. Under
 * heap pressure the allocation fails, that init parameter never reaches the
 * panel, and without this check the driver would cheerfully log "init done"
 * over a half-configured display showing washed-out or garbled colour. */
static void lcd_transmit(const uint8_t *bytes, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,    /* in BITS, not bytes -- a classic slip */
        .tx_buffer = bytes,
    };

    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK && s_err == ESP_OK) {
        s_err = err;
        ESP_LOGE(TAG, "SPI transfer of %u bytes failed: %s "
                      "(further failures suppressed)",
                 (unsigned)len, esp_err_to_name(err));
    }
}

/* Command vs. data is not two different wires or two different transactions:
 * it is which state the DC pin is in when the byte lands on the bus. Hence a
 * gpio_set_level immediately before each transfer. CS is toggled by the SPI
 * driver itself, because it was given the pin in the device config. */
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    lcd_transmit(&cmd, 1);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;                  /* several init entries carry no payload */
    }
    gpio_set_level(PIN_DC, 1);
    lcd_transmit(data, len);
}

static void lcd_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));   /* the panel needs this settling time
                                       * before it will accept commands     */
}

/* --- the GC9A01's own vendor power-on sequence ---------------------------
 *
 * These registers are not in the public datasheet's command table. They are
 * the panel maker's recommended gamma/voltage/timing setup, and every working
 * GC9A01 driver sends some version of this same table before the display will
 * show anything correctly.
 *
 * Do not try to derive or "clean up" this table. It was cross-checked byte for
 * byte against two independent working drivers (a community ESP-IDF component
 * and Espressif's own esp-bsp GC9A01 driver) and then proven on this panel.
 * It is data, not logic. */

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
} lcd_init_cmd_t;

static const lcd_init_cmd_t init_cmds[] = {
    {0xFE, {0}, 0},
    {0xEF, {0}, 0},
    {0xEB, {0x14}, 1},
    {0x84, {0x60}, 1},
    {0x85, {0xFF}, 1},
    {0x86, {0xFF}, 1},
    {0x87, {0xFF}, 1},
    {0x8E, {0xFF}, 1},
    {0x8F, {0xFF}, 1},
    {0x88, {0x0A}, 1},
    {0x89, {0x23}, 1},
    {0x8A, {0x00}, 1},
    {0x8B, {0x80}, 1},
    {0x8C, {0x01}, 1},
    {0x8D, {0x03}, 1},
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4},
    {0xFF, {0x60, 0x01, 0x04}, 3},
    {0xC3, {0x13}, 1},
    {0xC4, {0x13}, 1},
    {0xC9, {0x30}, 1},
    {0xBE, {0x11}, 1},
    {0xE1, {0x10, 0x0E}, 2},
    {0xDF, {0x21, 0x0C, 0x02}, 3},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
    {0xED, {0x1B, 0x0B}, 2},
    {0xAE, {0x77}, 1},
    {0xCD, {0x63}, 1},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9},
    {0xE8, {0x34}, 1},
    {0x60, {0x38, 0x0B, 0x6D, 0x6D, 0x39, 0xF0, 0x6D, 0x6D}, 8},
    {0x61, {0x38, 0xF4, 0x6D, 0x6D, 0x38, 0xF7, 0x6D, 0x6D}, 8},
    {0x62, {0x38, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x38, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12},
    {0x63, {0x38, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x38, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10},
    {0x74, {0x10, 0x45, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7},
    {0x98, {0x3E, 0x07}, 2},
    {0x99, {0x3E, 0x07}, 2},
};

static void spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,     /* write-only panel -- nothing to read back,
                                    * which is also why a wiring fault cannot
                                    * be detected in software                */
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* The largest transfer we intend to make: one row of RGB565 pixels.
         *
         * Read this as a hint, not as the limit that will actually apply.
         * spi_bus_initialize passes it to spicommon_dma_desc_alloc, which
         * rounds it up to a whole number of DMA descriptors and writes the
         * larger figure back -- in practice a few KB, not 480 bytes. So do not
         * size a future multi-row transfer against this number in either
         * direction: it is neither the real cap nor a promise. */
        .max_transfer_sz = GC9A01_WIDTH * 2,
    };
    /* SPI2_HOST is the general-purpose SPI peripheral on the ESP32-C3 (SPI0
     * and SPI1 are spoken for by the flash chip -- do not use them). */
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode           = 0,       /* clock polarity/phase 0,0 */
        .spics_io_num   = PIN_CS,  /* handing CS to the driver is what makes
                                    * it toggle automatically per transaction */
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));
}

void gc9a01_init(void)
{
    /* Calling this twice is a programming error, but it must not be a fatal
     * one. spi_bus_initialize() returns ESP_ERR_INVALID_STATE for an already
     * initialized host, and it is wrapped in ESP_ERROR_CHECK below -- so
     * without this guard a second call would panic and reboot the chip. That
     * is a poor trade: stage 8 adds reconnect and recovery paths, and
     * re-initializing the display from one of them is an easy mistake to
     * make. Turning a redundant call into a logged no-op keeps a cosmetic
     * slip from becoming a boot loop. */
    static bool s_inited = false;
    if (s_inited) {
        ESP_LOGW(TAG, "gc9a01_init() called more than once -- ignoring");
        return;
    }

    /* DC and RST are plain outputs we drive by hand. CS is not configured
     * here on purpose -- the SPI driver claims that pin itself. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    spi_init();

    ESP_LOGI(TAG, "hardware reset...");
    lcd_reset();

    ESP_LOGI(TAG, "sending %d vendor init commands...",
             (int)(sizeof(init_cmds) / sizeof(init_cmds[0])));
    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        lcd_cmd(init_cmds[i].cmd);
        lcd_data(init_cmds[i].data, init_cmds[i].len);
    }

    /* Standard MIPI DCS commands from here down -- every controller of this
     * family understands them, so unlike the table above these can be trusted
     * from the datasheet without cross-checking against other drivers. */

    /* Memory access control: it sets both the scan direction and the RGB/BGR
     * order, which is why one byte decides two apparently unrelated things.
     * The bits that matter here:
     *
     *   0x80  MY   row address order      (flips vertically)
     *   0x40  MX   column address order   (flips horizontally)
     *   0x20  MV   row/column exchange    (rotates 90 degrees)
     *   0x08  BGR  colour component order
     *
     * 0x48 = MX | BGR.
     *
     * This was 0x08 through stage 6, on the evidence of a red/green/blue fill
     * test that showed true colours. That evidence was real but incomplete,
     * and the distinction is worth keeping: a solid fill is symmetric, so it
     * can prove the colour order and cannot say anything at all about
     * orientation. The first asymmetric thing ever drawn on this panel -- text,
     * at stage 7 -- came out mirrored left-to-right, which is MX. The BGR bit
     * stays set, because that half of the original finding still holds. */
    uint8_t madctl = 0x48;
    lcd_cmd(0x36);
    lcd_data(&madctl, 1);

    uint8_t colmod = 0x05;        /* 16 bits/pixel, RGB565 */
    lcd_cmd(0x3A);
    lcd_data(&colmod, 1);

    lcd_cmd(0x21);                /* display inversion ON -- counter-intuitive,
                                   * but most GC9A01 panels need it for correct
                                   * colours; without it everything is negative */

    lcd_cmd(0x11);                /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x29);                /* display on */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* This is what makes the header's promise true. Without it, a transfer
     * that never left the host would be logged and then walked straight past,
     * and "init done" would print over a half-configured panel -- the exact
     * silent-wrong-state this project keeps trying to avoid. Aborting is right
     * *here*, in setup: there is no meaningful way to continue from a display
     * that was not configured. Drawing calls take the opposite view, and say
     * why in gc9a01.h. */
    ESP_ERROR_CHECK(s_err);

    s_inited = true;
    ESP_LOGI(TAG, "init done");
}

/* Sets the rectangle that subsequent pixel writes land in, then opens the
 * memory-write command. Every lcd_data() call after this one is pixel data
 * until another command is sent. Coordinates are inclusive on both ends,
 * which is why callers pass WIDTH-1 rather than WIDTH. */
static void lcd_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    lcd_cmd(0x2A);                /* column address set */
    lcd_data(col, sizeof(col));

    uint8_t row[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd(0x2B);                /* row address set */
    lcd_data(row, sizeof(row));

    lcd_cmd(0x2C);                /* memory write */
}

void gc9a01_fill_rect(int x, int y, int w, int h, uint16_t color565)
{
    /* Clip first, so a caller may compute a rectangle that runs off the edge
     * without having to check. Note the width is reduced *and* the origin
     * moved when x or y is negative -- adjusting one without the other is the
     * classic way to get a rectangle that is the wrong size and in the wrong
     * place. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GC9A01_WIDTH)  { w = GC9A01_WIDTH  - x; }
    if (y + h > GC9A01_HEIGHT) { h = GC9A01_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;                  /* entirely offscreen; nothing to do */
    }

    lcd_set_addr_window(x, y, x + w - 1, y + h - 1);

    /* One row's worth of pixels, sent h times, rather than a full 115,200-byte
     * framebuffer. That is not a micro-optimisation: a whole frame would be
     * most of this chip's free heap, and the panel does not need it -- it
     * tracks its own write position, so consecutive writes simply continue
     * where the last one stopped.
     *
     * static, so it lives in .bss rather than on the caller's stack. Sized for
     * the widest possible row. RGB565 goes out most-significant byte first. */
    static uint8_t row_buf[GC9A01_WIDTH * 2];
    for (int i = 0; i < w; i++) {
        row_buf[i * 2]     = color565 >> 8;
        row_buf[i * 2 + 1] = color565 & 0xFF;
    }

    for (int r = 0; r < h; r++) {
        lcd_data(row_buf, (size_t)w * 2);
    }
}

void gc9a01_fill_screen(uint16_t color565)
{
    gc9a01_fill_rect(0, 0, GC9A01_WIDTH, GC9A01_HEIGHT, color565);
}

/* --- text ---------------------------------------------------------------- */

/* Which five column bytes to draw for a character. Everything the font cannot
 * represent funnels through here to the box glyph, so no caller has to think
 * about it. */
static const uint8_t *glyph_for(char c)
{
    unsigned char u = (unsigned char)c;

    /* The table is uppercase-only; folding here rather than storing a second
     * 130 bytes of near-identical shapes. */
    if (u >= 'a' && u <= 'z') {
        u -= ('a' - 'A');
    }
    if (u < FONT5X7_FIRST || u > FONT5X7_LAST) {
        return font5x7_unknown;
    }
    return font5x7[u - FONT5X7_FIRST];
}

/* One character cell, as a single address window: set the rectangle once, then
 * stream every pixel of it. The alternative -- a fill_rect per lit pixel --
 * would be hundreds of tiny SPI transactions per character. */
static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    const uint8_t *glyph = glyph_for(c);

    const int cw = GC9A01_CHAR_W * scale;
    const int ch = GC9A01_CHAR_H * scale;

    /* Skip rather than clip. Half a character is not a smaller character, it
     * is a corrupted one, and the address-window arithmetic for a partial
     * glyph is exactly the sort of thing that goes subtly wrong. */
    if (x < 0 || y < 0 || x + cw > GC9A01_WIDTH || y + ch > GC9A01_HEIGHT) {
        return;
    }

    lcd_set_addr_window(x, y, x + cw - 1, y + ch - 1);

    /* Big enough for the widest cell at the maximum scale; the clamp in
     * gc9a01_draw_text is what keeps that promise true. */
    static uint8_t row_buf[GC9A01_CHAR_W * GC9A01_MAX_TEXT_SCALE * 2];

    for (int gy = 0; gy < FONT5X7_H; gy++) {
        int n = 0;
        for (int gx = 0; gx < GC9A01_CHAR_W; gx++) {
            /* Column FONT5X7_W is the spacer: always background, never in the
             * font data, which is why this reads the bit only for gx < 5. */
            bool on = (gx < FONT5X7_W) && ((glyph[gx] >> gy) & 1);
            uint16_t colour = on ? fg : bg;

            for (int s = 0; s < scale; s++) {   /* horizontal magnification */
                row_buf[n++] = colour >> 8;
                row_buf[n++] = colour & 0xFF;
            }
        }
        for (int s = 0; s < scale; s++) {       /* vertical magnification */
            lcd_data(row_buf, (size_t)n);
        }
    }
}

void gc9a01_draw_text(int x, int y, const char *text,
                      uint16_t fg565, uint16_t bg565, int scale)
{
    if (text == NULL) {
        return;
    }
    /* Clamped, not asserted: scale sizes a fixed buffer in draw_char, so an
     * out-of-range value would be a buffer overrun rather than an ugly glyph.
     * Every caller in this project passes a literal, so this is a guard
     * against future edits, not a runtime condition. */
    if (scale < 1) { scale = 1; }
    if (scale > GC9A01_MAX_TEXT_SCALE) { scale = GC9A01_MAX_TEXT_SCALE; }

    for (const char *p = text; *p != '\0'; p++) {
        draw_char(x, y, *p, fg565, bg565, scale);
        x += GC9A01_CHAR_W * scale;
    }
}

int gc9a01_text_width(const char *text, int scale)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    if (scale < 1) { scale = 1; }
    if (scale > GC9A01_MAX_TEXT_SCALE) { scale = GC9A01_MAX_TEXT_SCALE; }

    /* Every character advances a full cell, but the last one's trailing spacer
     * is not ink. Counting it would push centred text half a spacer to the
     * left, which is visible at scale 5. */
    int len = (int)strlen(text);
    return (len * GC9A01_CHAR_W - 1) * scale;
}
