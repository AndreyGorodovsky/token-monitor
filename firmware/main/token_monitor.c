/* Token monitor -- firmware stage 3: WiFi only.
 *
 * Goal of this stage, and nothing more: join the WiFi network and print the
 * IP address the router hands us over serial. No HTTP, no JSON, no display.
 * Success looks like a line reading "got IP: 192.168.1.x" in the monitor.
 *
 * Why this is its own stage: the chip has no screen output yet, so serial is
 * the only channel for finding out what it is doing. If WiFi and HTTP and
 * parsing all went in at once, a failure anywhere would look identical --
 * "nothing on the display". Proving the network layer alone means every
 * later stage starts from a known-good foundation.
 *
 * This also retires the last unverified assumption in the network path:
 * pc_service has already been reached from a phone on this WiFi, so once
 * the chip gets an address on the same 192.168.1.x subnet, PC-to-chip
 * reachability is established end to end.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED, top to bottom:
 *
 *   1. the secrets.h guard      -- fail the build early with a clear message
 *   2. includes                 -- and what each one is actually for
 *   3. module state             -- the handful of file-scope variables
 *   4. event handlers           -- code the WiFi driver calls back into
 *   5. wifi_start()             -- one-time setup, in dependency order
 *   6. app_main()               -- the entry point; where execution begins
 *
 * The control flow is NOT top-to-bottom like a script. app_main() sets things
 * up and then *sleeps*; the interesting work happens in the event handlers,
 * which run on a different task entirely, whenever the radio has news. If you
 * read only one comment in this file, make it the one above s_reconnect_timer
 * -- that distinction is the source of the one real bug this file has had.
 * ---------------------------------------------------------------------------
 */

/* Fail with a sentence you can act on, rather than the compiler's
 * "secrets.h: No such file or directory" pointing at an #include line.
 * __has_include is a compiler feature test; the outer #if defined() guard is
 * because it isn't universally available, though GCC (what ESP-IDF uses) has
 * had it for years. */
#if defined(__has_include)
#  if !__has_include("secrets.h")
#    error "main/secrets.h is missing -- copy main/secrets.h.example to main/secrets.h and fill in your WiFi details. See SECRETS.md."
#  endif
#endif

#include <string.h>                  /* memcpy, for copying credentials     */

/* FreeRTOS is the operating system underneath all of this. The ESP32 runs
 * several tasks (threads) at once -- our code, the WiFi driver, the TCP/IP
 * stack -- and these headers are how we take part in that. */
#include "freertos/FreeRTOS.h"       /* pdMS_TO_TICKS, portMAX_DELAY        */
#include "freertos/task.h"           /* vTaskDelay -- sleep a task          */
#include "freertos/event_groups.h"   /* the "wake me when X happens" flags  */

#include "nvs_flash.h"               /* non-volatile storage; WiFi needs it */
#include "esp_wifi.h"                /* the radio driver itself             */
#include "esp_event.h"               /* the event loop drivers report to    */
#include "esp_netif.h"               /* TCP/IP stack + network interfaces   */
#include "esp_timer.h"               /* one-shot timers, off the event loop */
#include "esp_log.h"                 /* ESP_LOGI / ESP_LOGW / ESP_LOGE      */

#include "secrets.h"                 /* YOUR values -- gitignored           */

/* Every log line we emit is prefixed with this, so our messages stay
 * greppable among the much noisier driver output ("wifi:", "esp_netif_
 * handlers:", and so on). */
static const char *TAG = "token_monitor";

/* --- module state ------------------------------------------------------- */

/* A FreeRTOS event group is a set of bit flags that tasks can block on. It is
 * the plumbing that lets app_main() sleep until the WiFi driver -- running on
 * its own task -- reports something worth waking for. The alternative would
 * be a polling loop burning CPU asking "connected yet?"; blocking on a flag
 * costs nothing while it waits.
 *
 * We use exactly one bit. BIT0 is just the value 1; a second independent flag
 * would be BIT1, then BIT2, and so on. */
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

/* Reconnect attempts since the last success. Only used for logging at this
 * stage -- we retry forever, because a desk gadget that gives up after five
 * tries is useless if the router reboots overnight. Proper backoff and a
 * visible "disconnected" state on the display are stage 8. */
static int s_retry_count = 0;

/* One-shot timer used to space out reconnect attempts.
 *
 * The obvious way to wait 2 seconds before retrying is vTaskDelay() straight
 * inside the disconnect handler -- and it appears to work. It is still wrong:
 * event handlers run on the shared default event loop task, so sleeping there
 * stops every other handler on that loop, including on_got_ip below. While
 * the loop is parked, drivers keep posting to a fixed-size queue
 * (CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE, 32 by default); overflow silently
 * drops events. A dropped IP_EVENT_STA_GOT_IP would leave WIFI_CONNECTED_BIT
 * clear and app_main blocked forever on a chip that actually holds a valid
 * DHCP lease -- a hang with no error message anywhere.
 *
 * esp_timer runs the callback on its own task instead, so the loop stays free.
 * The rule this encodes, and the one to carry into stages 4-8: never sleep,
 * and never do slow work, inside an event handler. Start something that
 * finishes later, and return.
 */
static esp_timer_handle_t s_reconnect_timer;

/* Runs on the esp_timer task, 2 seconds after a disconnect. `arg` is the user
 * pointer supplied at creation time -- we don't need one, so it goes unused. */
static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

/* --- event handlers ----------------------------------------------------- */

/* The four parameters are the standard esp_event handler signature:
 *
 *   arg   -- the user pointer we passed at registration (NULL here)
 *   base  -- which FAMILY of event this is (WIFI_EVENT, IP_EVENT, ...)
 *   id    -- which event within that family (WIFI_EVENT_STA_START, ...)
 *   data  -- pointer to an event-specific struct, or NULL
 *
 * `data` is void* because every event carries a different payload type, so
 * the caller cannot know which one. You cast it to the struct matching this
 * particular `id` -- which is why casting on the wrong id is a real hazard,
 * and why each cast below sits inside its id check.
 *
 * We registered this for ESP_EVENT_ANY_ID on WIFI_EVENT, so it sees every
 * WiFi event; the ones we don't handle simply fall through and return.
 *
 * Note the driver does not auto-connect: WIFI_EVENT_STA_START means "radio is
 * up, now ask it to connect", and every disconnect means "ask again". */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "wifi started, connecting to \"%s\"...", WIFI_SSID);
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;

        /* Clear the flag first: nothing waiting on it should go on believing
         * we are connected while we retry. */
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

        /* The reason code is the single most useful number when this stage
         * fails. Common ones: 201 = AP not found (wrong SSID, or the network
         * is 5 GHz only -- this chip is 2.4 GHz only), 15 = 4-way handshake
         * timeout (wrong password), 2 = AUTH_EXPIRE (seen routinely at boot
         * on WPA3, and it recovers on its own), 205 = connection lost. */
        ESP_LOGW(TAG, "disconnected (reason %d), retry %d", e->reason, ++s_retry_count);

        /* Hand the wait to esp_timer and return immediately, so this handler
         * never blocks the event loop. See s_reconnect_timer above.
         *
         * The stop() guards against re-arming a timer that is already running
         * (two disconnects in quick succession), which would otherwise return
         * an error; it is a harmless no-op when the timer is idle. esp_timer
         * counts in MICROseconds, hence 2000 * 1000 for two seconds. */
        esp_timer_stop(s_reconnect_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer, 2000 * 1000));
        return;
    }
}

/* Called for IP_EVENT_STA_GOT_IP. This -- not the WiFi association -- is the
 * moment the chip can actually talk to anything: association means "joined
 * the network", an IP lease means "DHCP finished and routing works". Code
 * that starts talking at association time works on a fast network and fails
 * intermittently on a slow one. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;

    s_retry_count = 0;               /* a success resets the retry tally    */

    /* IPSTR and IP2STR are a matched pair of ESP-IDF macros for printing an
     * IP address: IPSTR expands to the "%d.%d.%d.%d" format string, IP2STR
     * expands to the four comma-separated byte arguments that fill it. They
     * are always used together. */
    ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "netmask: " IPSTR ", gateway: " IPSTR,
             IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));

    /* Setting the bit is what wakes app_main out of xEventGroupWaitBits. */
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
}

/* --- setup -------------------------------------------------------------- */

static void wifi_start(void)
{
    /* Four layers, bottom up. Each has to exist before the next one can:
     *   1. NVS        -- the WiFi driver stores calibration data here.
     *   2. netif      -- the TCP/IP stack (lwIP) and its interfaces.
     *   3. event loop -- how the driver reports state changes back to us.
     *   4. wifi       -- the radio driver itself.
     *
     * ESP_ERROR_CHECK wraps a call returning esp_err_t and aborts the program
     * -- printing the failing file and line -- if the result isn't ESP_OK. It
     * is the right tool for setup steps like these, where a failure means
     * nothing afterwards can work. It is the WRONG tool for anything that can
     * fail in normal operation, such as a network request: crashing the chip
     * because a server was briefly unreachable is not a fallback state. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Creates the station-mode network interface and attaches it to the
     * default event loop. It returns a handle, which we discard: nothing here
     * needs to reconfigure the interface afterwards. */
    esp_netif_create_default_wifi_sta();

    /* WIFI_INIT_CONFIG_DEFAULT() fills a struct with sane defaults (buffer
     * counts, task priorities). Espressif's docs are explicit that you should
     * start from it and adjust, rather than assemble the struct by hand. */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Created before the handlers are registered, so a disconnect event
     * arriving immediately can never find a NULL timer handle. */
    const esp_timer_create_args_t reconnect_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect",    /* the name shows up in esp_timer_dump */
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));

    /* Subscribe to the two event families we care about. ESP_EVENT_ANY_ID on
     * WIFI_EVENT means "every WiFi event"; for IP_EVENT we want just the one.
     * The two trailing NULLs are the user pointer (arrives as `arg`) and an
     * optional handle for later unregistration -- neither is needed here. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL, NULL));

    /* The = { 0 } is load-bearing, not tidiness: it zeroes every field, which
     * is what leaves the credential arrays NUL-padded after the partial
     * copies below, and leaves every option we don't set at its default. */
    wifi_config_t wifi_cfg = { 0 };

    /* Caught at build time rather than presenting as a runtime mystery.
     * ssid is uint8_t[32] and password uint8_t[64]. The 802.11 SSID field is
     * a length-counted array, not a C string, so a full 32 characters is
     * legal and needs all 32 bytes. Copying with sizeof-1 would quietly drop
     * the last character of a maximum-length SSID -- which surfaces as an
     * endless "disconnected (reason 201)" loop, i.e. exactly the symptom the
     * README tells you to blame on a typo or a 5 GHz network. Same shape for
     * a 64-hex-character raw WPA2 PSK.
     *
     * sizeof(WIFI_SSID) - 1 is the string's length: WIFI_SSID is a literal,
     * so sizeof counts its bytes including the terminating NUL, and the -1
     * drops that. _Static_assert is evaluated by the compiler, so an
     * over-long value in secrets.h fails the build with the message below
     * rather than misbehaving on the desk. */
    _Static_assert(sizeof(WIFI_SSID) - 1 <= 32,
                   "WIFI_SSID in secrets.h is longer than the 32-character 802.11 limit");
    _Static_assert(sizeof(WIFI_PASSWORD) - 1 <= 64,
                   "WIFI_PASSWORD in secrets.h is longer than the 64-character limit");

    memcpy(wifi_cfg.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);

    /* WIFI_AUTH_OPEN as a *threshold* does not mean "connect to open
     * networks" -- it means "do not require a minimum security level".
     * Note the driver does not necessarily keep this value: with a password
     * set it logs "authmode threshold changes from OPEN to WPA2" and raises
     * it itself. That is fine, because WPA3 outranks WPA2 in the threshold
     * ordering -- this network is in fact WPA3-SAE and connects normally.
     * Leave this permissive anyway so the config never has to know which
     * security mode the router is running.
     *
     * sae_pwe_h2e is not optional here: this AP negotiates WPA3-SAE H2E
     * (hash-to-element), and a station that doesn't offer it cannot associate
     * at all. */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    /* Order matters: mode, then config, then start. esp_wifi_set_config needs
     * to know which mode it is configuring, and esp_wifi_start is what finally
     * powers the radio and fires WIFI_EVENT_STA_START -- so by the time that
     * event lands, the handler for it is already registered above. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* --- entry point -------------------------------------------------------- */

/* app_main is where an ESP-IDF program begins -- the equivalent of main() in
 * an ordinary C program. It runs on a FreeRTOS task ("main_task") that the
 * framework creates for it. Unlike main(), returning from app_main is
 * perfectly legal and does not end the program: the other tasks carry on. We
 * loop forever anyway, so the heartbeat below keeps printing. */
void app_main(void)
{
    /* nvs_flash_init() can legitimately fail on a chip whose NVS partition is
     * full or was written by a different IDF version -- erasing and retrying
     * is the documented recovery, not a workaround. Note we only erase for
     * those two specific errors; any other failure falls through to the
     * ESP_ERROR_CHECK below rather than being papered over. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erasing, doing that now");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Must exist before wifi_start(), because the handlers it registers can
     * fire -- and touch this group -- the instant the radio starts. */
    s_wifi_events = xEventGroupCreate();

    ESP_LOGI(TAG, "stage 3: wifi only");
    wifi_start();

    /* Block until connected. The four arguments after the group are:
     *   WIFI_CONNECTED_BIT -- which bit(s) to wait for
     *   pdFALSE            -- do NOT clear the bit on exit; later stages want
     *                         to keep reading it to see if we are still up
     *   pdTRUE             -- wait for ALL requested bits (moot with a single
     *                         bit, but it states the intent correctly)
     *   portMAX_DELAY      -- wait forever, no timeout
     *
     * Waiting forever is acceptable only because the retry loop lives in the
     * disconnect handler and never gives up. */
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "connected. nothing else to do at this stage.");

    /* Heartbeat, so a silent serial monitor means "the chip crashed or reset"
     * rather than leaving you guessing whether it is merely idle.
     *
     * vTaskDelay sleeps this task without burning CPU. Unlike the delay we
     * removed from the event handler, blocking here is correct: app_main's
     * task has nothing else to do, and no other handler is waiting on it. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        /* Ask the driver about the AP we are associated with. A failure
         * return is itself the useful signal -- it means we are not currently
         * associated, which an RSSI number alone could not tell us. */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            /* RSSI is negative dBm; closer to zero is stronger. Roughly:
             * -50 excellent, -60 good, -70 workable, -80 unreliable. */
            ESP_LOGI(TAG, "still connected, rssi %d dBm", ap.rssi);
        } else {
            ESP_LOGW(TAG, "not currently associated");
        }
    }
}
