/* Reading the setup button. See button.h for the wiring and the contract.
 *
 * Polled rather than interrupt-driven, on purpose. An ISR would have to debounce
 * anyway -- a bouncing contact generates a burst of edges, and an interrupt per
 * edge is the worst way to receive them -- and the thing being measured is a
 * human finger, where 20 ms of latency is not observable. A task that wakes 50
 * times a second and looks at a pin is less machinery for the same result, and
 * it can block harmlessly instead of running in interrupt context.
 */

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "button.h"

static const char *TAG = "button";

#define BUTTON_GPIO       GPIO_NUM_3   /* D1 */

/* 20 ms, which is 2 ticks at ESP-IDF's default 100 Hz. It must be at least one
 * tick: pdMS_TO_TICKS does integer arithmetic against configTICK_RATE_HZ, so
 * anything under 10 ms rounds to 0, and vTaskDelay(0) does not sleep -- it
 * yields, which busy-spins this task and starves the idle task until the task
 * watchdog complains. That is not hypothetical; it is exactly what the
 * standalone wiring test did before the numbers were fixed. The assert below
 * turns a future edit into a build error rather than a puzzling log. */
#define POLL_MS           20
_Static_assert(pdMS_TO_TICKS(POLL_MS) >= 1,
               "POLL_MS rounds down to zero ticks at this CONFIG_FREERTOS_HZ; "
               "vTaskDelay(0) busy-spins instead of sleeping");

/* Consecutive identical samples required before a change is believed: 3 x
 * 20 ms = 60 ms of agreement.
 *
 * The standalone wiring test measured NO bounce at all on this button, under
 * effectively continuous polling -- so on today's hardware this could be 1 and
 * nothing would change. It is 3 because debounce is insurance against the
 * button this project has, aging, or being swapped for a worse one, and 60 ms
 * is far below what a finger can notice. Cheap insurance, invisible cost. */
#define DEBOUNCE_SAMPLES  3

/* Pressed reads LOW, because the pull-up holds the pin high until the button
 * shorts it to ground. Named rather than written as a bare 0 wherever it is
 * used, because "level == 0 means pressed" is the single most confusing line
 * in any button driver. */
#define PRESSED_LEVEL     0

typedef struct {
    EventGroupHandle_t events;
    EventBits_t        bit;
} button_ctx_t;

static void button_task(void *arg)
{
    button_ctx_t ctx = *(button_ctx_t *)arg;   /* copied: the caller's struct
                                                * is on app_main's stack      */

    bool     stable_pressed = false;  /* the debounced state                  */
    int      candidate      = -1;     /* a raw level seen but not yet trusted */
    int      agree_count    = 0;      /* how many samples have agreed         */
    int64_t  pressed_at_us  = 0;      /* when the current press began         */
    bool     fired          = false;  /* long press already reported for this
                                       * press; cleared only on release       */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int level = gpio_get_level(BUTTON_GPIO);

        /* Debounce: a level has to repeat DEBOUNCE_SAMPLES times in a row
         * before it is allowed to change the state we act on. Any disagreement
         * restarts the count, so a bouncing contact simply never accumulates
         * enough agreement to be believed until it settles. */
        if (level != candidate) {
            candidate   = level;
            agree_count = 1;
            continue;
        }
        if (agree_count < DEBOUNCE_SAMPLES) {
            agree_count++;
            continue;
        }

        bool now_pressed = (level == PRESSED_LEVEL);

        if (now_pressed && !stable_pressed) {
            stable_pressed = true;
            pressed_at_us  = esp_timer_get_time();
            fired          = false;
            ESP_LOGI(TAG, "pressed");

        } else if (!now_pressed && stable_pressed) {
            stable_pressed = false;
            int64_t held_ms = (esp_timer_get_time() - pressed_at_us) / 1000;
            /* Reporting the duration of a press that did NOT qualify is the
             * useful half of this line: "released after 2740 ms" tells you the
             * button works and you let go early, which is a completely
             * different problem from "nothing happened". */
            ESP_LOGI(TAG, "released after %lld ms%s",
                     (long long)held_ms, fired ? " (long press already reported)" : "");
            fired = false;

        } else if (now_pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - pressed_at_us) / 1000;
            if (held_ms >= BUTTON_HOLD_MS) {
                /* Fire on reaching the threshold, not on release. Holding a
                 * button and having it act the moment it takes effect is what
                 * makes a long press feel deliberate rather than laggy -- and
                 * it means letting go afterwards cannot cancel it. */
                fired = true;
                ESP_LOGI(TAG, "long press (%lld ms) -- setup requested", (long long)held_ms);
                xEventGroupSetBits(ctx.events, ctx.bit);
            }
        }
    }
}

void button_start(EventGroupHandle_t events, EventBits_t long_press_bit)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,   /* a bit per pin, hence the shift */
        .mode         = GPIO_MODE_INPUT,
        /* The load-bearing line. With one leg on the pin and the other on GND,
         * a released button leaves the pin connected to nothing at all --
         * floating -- and a floating pin does not read high, it reads stray
         * charge and nearby noise, and can flicker as a hand moves near the
         * board. The internal pull-up is a weak resistor to 3.3 V: weak enough
         * that pressing the button wins easily, strong enough that an untouched
         * pin has a definite value instead of an opinion. */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, /* enabling both would fight  */
        .intr_type    = GPIO_INTR_DISABLE,     /* polled; see the file header */
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Let the pull-up charge the pin's stray capacitance before the first
     * reading. Microseconds in reality; 10 ms at startup is free. */
    vTaskDelay(pdMS_TO_TICKS(10));

    int level = gpio_get_level(BUTTON_GPIO);
    ESP_LOGI(TAG, "GPIO%d (D1) ready, idle level %d (%s)",
             BUTTON_GPIO, level, level == PRESSED_LEVEL ? "PRESSED" : "released");
    if (level == PRESSED_LEVEL) {
        /* Reads pressed before anyone has touched it. Not fatal -- and not
         * something to refuse to start over -- but it means the button will
         * never produce an edge, so setup mode would be unreachable and the
         * reason would be invisible. Say it once, loudly, at boot. */
        ESP_LOGW(TAG, "button reads PRESSED at rest -- check the wiring:");
        ESP_LOGW(TAG, "  a 4-leg tactile switch with both wires on the same");
        ESP_LOGW(TAG, "  internally-shorted pair looks exactly like this");
    }

    /* The context has to outlive this function: the task reads it after
     * button_start has returned. static, rather than malloc'd, because there
     * is exactly one button and freeing it would never happen anyway. */
    static button_ctx_t ctx;
    ctx.events = events;
    ctx.bit    = long_press_bit;

    /* 2560 bytes is generous for a loop that calls gpio_get_level and
     * ESP_LOGI; the logging is what needs most of it. Priority 3 sits below
     * usage_task's 5 -- this task has nothing urgent to do, and being
     * preempted by the thing that actually draws is the right ordering. */
    xTaskCreate(&button_task, "button", 2560, &ctx, 3, NULL);
}
