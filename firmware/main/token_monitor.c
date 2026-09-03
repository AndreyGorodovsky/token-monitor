/* Token monitor -- firmware stage 4: WiFi, plus one HTTP sanity check.
 *
 * Stage 3 (still here, unchanged) joins the WiFi network and prints the IP
 * address the router hands us. Stage 4 adds exactly one thing on top: a
 * single GET to a trivial, known-good web address, printing the raw response
 * body over serial. No JSON, no display, and deliberately not pc_service yet.
 *
 * Why bother with a throwaway URL instead of going straight to the real
 * service: if the first-ever esp_http_client call were also the first-ever
 * call to pc_service, a failure would have two suspects -- the HTTP client on
 * this chip and IDF version, or the service on the PC (not running? firewall?
 * wrong port?). Proving the client against something that is definitely up
 * turns the next stage's debugging into a single-suspect problem. It also
 * proves DNS resolution works, which pc_service (reached by raw IP) never
 * would.
 *
 * Success looks like: a 200, a content length, and readable HTML between the
 * two "---8<---" markers in the serial monitor.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED, top to bottom:
 *
 *   1. the secrets.h guard      -- fail the build early with a clear message
 *   2. includes                 -- and what each one is actually for
 *   3. module state             -- the handful of file-scope variables
 *   4. WiFi event handlers      -- code the WiFi driver calls back into
 *   5. the HTTP sanity check    -- stage 4: its accumulator, handler, task
 *   6. wifi_start()             -- one-time setup, in dependency order
 *   7. app_main()               -- the entry point; where execution begins
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
#include <inttypes.h>                /* PRId64 -- see the note where it is
                                      * used; printing a 64-bit value with a
                                      * hardcoded "%lld" is not portable      */

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
#include "esp_http_client.h"         /* stage 4: the HTTP client itself     */
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

/* --- stage 4: the HTTP sanity check -------------------------------------- */

/* A plain-HTTP address, on purpose. https:// would pull in TLS, a certificate
 * bundle, and roughly 40 KB of extra RAM at handshake time -- none of which
 * stage 5 needs, because pc_service serves plain HTTP on the LAN. Testing
 * with TLS here would prove something we are not going to use, and hide the
 * thing we are.
 *
 * example.com is maintained by IANA for exactly this purpose: it is stable,
 * small, and answers plain HTTP with a 200 rather than redirecting to HTTPS.
 * If a network hijacks it (some captive portals and ISPs do), http://neverssl.com
 * is the usual fallback -- it exists specifically to never redirect. */
#define SANITY_URL "http://example.com/"

/* Where the response body accumulates.
 *
 * The body does NOT arrive in one piece: esp_http_client hands it to us in
 * chunks as TCP segments land, calling our event handler once per chunk. That
 * is the single most important thing this stage demonstrates, because it is
 * exactly the shape stage 5 has to feed to cJSON -- which needs the whole
 * document at once, not a fragment. So the handler's job is to append, and
 * only the code after the request completes gets to look at the result. */
typedef struct {
    char *buf;          /* caller-owned storage                             */
    int   cap;          /* its size in bytes, including room for the NUL    */
    int   len;          /* bytes written so far                             */
    bool  truncated;    /* true if the response was bigger than cap         */
} body_buf_t;

/* 4 KB holds example.com's ~1.2 KB of HTML with room to spare, and is far
 * more than the few hundred bytes of JSON stage 5 will see. The cap exists
 * so a surprisingly large response cannot walk off the end of the buffer:
 * an undocumented endpoint changing its mind about response size is a real
 * possibility, and "truncate and say so" beats both "corrupt memory" and
 * "silently return half a document". */
#define BODY_CAP 4096

/* Called by esp_http_client at each stage of the request. Runs on whichever
 * task called esp_http_client_perform() -- ours, below -- not on a driver
 * task, so it is safe to log from here.
 *
 * Returning ESP_OK means "carry on"; returning an error aborts the request. */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_buf_t *body = (body_buf_t *)evt->user_data;

    switch (evt->event_id) {

    /* One call per response header. Printing them is not strictly needed,
     * but on a first run they are the proof that a real HTTP conversation
     * happened rather than something merely returning bytes. */
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "  header | %s: %s", evt->header_key, evt->header_value);
        break;

    /* One call per chunk of body. Append what fits; note if it doesn't. */
    case HTTP_EVENT_ON_DATA: {
        int room = body->cap - 1 - body->len;   /* -1 keeps room for a NUL */
        int take = (evt->data_len < room) ? evt->data_len : room;

        if (take > 0) {
            memcpy(body->buf + body->len, evt->data, take);
            body->len += take;
            body->buf[body->len] = '\0';        /* keep it printable as a
                                                 * C string at every step  */
        }
        if (take < evt->data_len) {
            body->truncated = true;             /* reported after the call */
        }
        break;
    }

    default:
        /* HTTP_EVENT_ON_CONNECTED, _HEADERS_SENT, _ON_FINISH, _DISCONNECTED,
         * _ERROR and _REDIRECT all land here. Nothing to do for this stage;
         * the return code from esp_http_client_perform() below already tells
         * us whether the request as a whole worked. */
        break;
    }

    return ESP_OK;
}

/* Performs the one request, prints the result, and deletes itself.
 *
 * Why this is a task rather than a few lines inside app_main: app_main runs
 * on a task whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE, which is 3584
 * bytes here. esp_http_client needs appreciably more than that once lwIP and
 * the HTTP parser are on the stack, and overflowing it produces a stack
 * canary panic and a reboot rather than a tidy error. 8 KB is what ESP-IDF's
 * own esp_http_client example allocates, and it is the number to start from
 * rather than tuning downward without a measurement.
 *
 * A task that has finished its work must delete itself; falling off the end
 * of a task function without calling vTaskDelete(NULL) crashes the system. */
static void http_sanity_task(void *arg)
{
    /* static, so this 4 KB sits in .bss rather than on the task's stack --
     * which we just went to some trouble to leave room in. */
    static char storage[BODY_CAP];

    body_buf_t body = { .buf = storage, .cap = sizeof(storage) };
    body.buf[0] = '\0';

    /* Fields we don't set stay zero, and zero means "the default" throughout
     * this struct. .user_data is the pointer handed back to our event handler
     * -- it is how the handler knows where to append without a global. */
    esp_http_client_config_t cfg = {
        .url           = SANITY_URL,
        .method        = HTTP_METHOD_GET,
        .event_handler = on_http_event,
        .user_data     = &body,
        .timeout_ms    = 10000,   /* 10s: generous for a LAN, and short
                                   * enough that a black-holed connection
                                   * fails visibly instead of hanging      */
    };

    ESP_LOGI(TAG, "stage 4: GET %s", SANITY_URL);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed (out of memory?)");
        vTaskDelete(NULL);
        return;                  /* unreachable; states the intent clearly */
    }

    /* perform() blocks until the whole exchange finishes: DNS, connect,
     * request, response, and every on_http_event call above. Note this is
     * NOT wrapped in ESP_ERROR_CHECK -- a failed network request is a normal
     * condition to report, not a reason to abort the program. That rule is
     * the whole reason stage 8 can have a "service unreachable" screen. */
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int     status = esp_http_client_get_status_code(client);
        int64_t clen   = esp_http_client_get_content_length(client);

        /* PRId64 expands to the right length modifier for a 64-bit integer on
         * this platform. Hardcoding "%lld" happens to work here but warns or
         * breaks elsewhere; IDF builds with -Wall, so this is the habit. */
        ESP_LOGI(TAG, "status %d, content-length %" PRId64 ", body %d bytes%s",
                 status, clen, body.len,
                 body.truncated ? " (TRUNCATED at BODY_CAP)" : "");

        /* printf rather than ESP_LOGI for the body itself: the log macros
         * prefix every call with "I (12345) token_monitor:" and colour codes,
         * which makes multi-line HTML almost unreadable. The markers make it
         * obvious where the body starts and stops. */
        printf("---8<--- body ---8<---\n%s\n---8<--- end ----8<---\n", body.buf);

        if (status != 200) {
            ESP_LOGW(TAG, "expected 200 -- a %d still proves the client works, "
                          "but check the URL", status);
        }
    } else {
        /* esp_err_to_name turns the numeric code into something searchable,
         * e.g. ESP_ERR_HTTP_CONNECT for a refused connection or
         * ESP_ERR_HTTP_EAGAIN for a timeout. */
        ESP_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "the chip has an IP, so suspect DNS, a captive portal, "
                      "or no route off the LAN -- not the WiFi join itself");
    }

    /* Always, on both paths: cleanup frees the socket and the parser state.
     * Skipping it leaks a few KB per request, which one request survives and
     * stage 5's once-a-minute loop would not. */
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "stage 4 complete. heartbeat continues; power-cycle to re-run.");
    vTaskDelete(NULL);
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

    ESP_LOGI(TAG, "stage 4: wifi + one http sanity check");
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

    ESP_LOGI(TAG, "connected.");

    /* Only now, with a DHCP lease in hand, is it worth making a request.
     * Association alone is not enough: the radio can be joined while DHCP is
     * still in progress, and a GET issued then fails with no route.
     *
     * The five arguments to xTaskCreate are the function, a name for it (it
     * shows up in crash dumps and task lists), the stack size in BYTES, the
     * argument passed to the function, and the priority. 5 is above the idle
     * task and below the WiFi driver's -- the same value ESP-IDF's own HTTP
     * example uses. The sixth parameter would receive a handle for later
     * control; we don't need one, because the task deletes itself. */
    xTaskCreate(&http_sanity_task, "http_sanity", 8192, NULL, 5, NULL);

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
