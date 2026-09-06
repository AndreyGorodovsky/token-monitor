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
 *   2. lcd_cmd / lcd_data      -- the only two ways anything reaches the panel
 *   3. the vendor init table   -- copied, not derived; see the note above it
 *   4. gc9a01_init             -- reset, table, then standard MIPI commands
 *   5. gc9a01_fill_screen      -- address window + a row buffer, 240 times
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"       /* vTaskDelay -- the reset and sleep-out
                                  * delays below are required by the panel,
                                  * not padding                              */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "gc9a01.h"

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
#define PIN_CS   GPIO_NUM_8   /* D8 -- CS  */

/* Conservative, and deliberately not raised yet. Many GC9A01 boards run
 * happily at 40 MHz, and stage 7 will want the extra speed once it is redrawing
 * regions -- but 10 MHz is the number this panel was *proven* at over jumper
 * wires, and changing it in the same step as moving the driver into a new
 * project would give a failure two suspects instead of one. Raise it later, on
 * its own, with the screen already working. */
#define SPI_CLOCK_HZ (10 * 1000 * 1000)

static spi_device_handle_t s_spi;

/* --- the only two ways anything reaches the panel ------------------------ */

/* Command vs. data is not two different wires or two different transactions:
 * it is which state the DC pin is in when the byte lands on the bus. Hence a
 * gpio_set_level immediately before each transfer. CS is toggled by the SPI
 * driver itself, because it was given the pin in the device config. */
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = {
        .length    = 8,          /* in BITS, not bytes -- a classic slip */
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;                  /* several init entries carry no payload */
    }
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    /* polling_transmit rather than the queued/interrupt form: it busy-waits
     * for the transfer instead of blocking on a semaphore, which is faster for
     * the short bursts this driver sends and keeps the call order obvious.
     * A full-screen fill is 240 of these back to back. */
    spi_device_polling_transmit(s_spi, &t);
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
        .max_transfer_sz = GC9A01_WIDTH * 2,   /* one row of RGB565 pixels */
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

    /* Memory access control: scan direction and RGB/BGR order. 0x08 is
     * confirmed correct on this panel -- a red/green/blue cycle showed true
     * colours, no swap. If red and blue ever come out exchanged, this is the
     * byte to flip (try 0x00 or 0x48); that would be a calibration detail,
     * never a wiring fault. */
    uint8_t madctl = 0x08;
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

void gc9a01_fill_screen(uint16_t color565)
{
    lcd_set_addr_window(0, 0, GC9A01_WIDTH - 1, GC9A01_HEIGHT - 1);

    /* One row's worth of pixels, sent 240 times, rather than a full 115,200-byte
     * framebuffer. That is not a micro-optimisation: a whole frame would be most
     * of this chip's free heap, and the panel does not need it -- it tracks its
     * own write position, so consecutive writes simply continue where the last
     * one stopped.
     *
     * static, so it lives in .bss rather than on the caller's stack. RGB565 goes
     * out most-significant byte first. */
    static uint8_t row_buf[GC9A01_WIDTH * 2];
    for (int i = 0; i < GC9A01_WIDTH; i++) {
        row_buf[i * 2]     = color565 >> 8;
        row_buf[i * 2 + 1] = color565 & 0xFF;
    }

    for (int y = 0; y < GC9A01_HEIGHT; y++) {
        lcd_data(row_buf, sizeof(row_buf));
    }
}
