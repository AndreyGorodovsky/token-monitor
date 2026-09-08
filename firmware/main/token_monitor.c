/* Token monitor -- firmware stage 8: the polish that makes it a gadget.
 *
 * Stage 7 did the thing this project exists for: usage numbers, on the glass.
 * It did it exactly once, at boot. Stage 8 is the difference between that and
 * something you can leave on a desk -- it refreshes on its own every 45
 * seconds, survives the router rebooting, and when it cannot get fresh numbers
 * it says so on the screen instead of quietly showing you yesterday's.
 *
 * Three things, per CLAUDE.md's definition of done:
 *
 *   1. A refresh loop. usage_task replaces stage 7's one-shot fetch task, and
 *      only repaints the lines that actually changed -- repainting all 240x240
 *      once a minute is a black flash you cannot stop watching.
 *   2. WiFi that recovers visibly. The retry timer already never gave up;
 *      what is new is that it backs off, and that a dropped link reaches the
 *      display instead of only the serial log.
 *   3. Failure states that degrade rather than blank. Once a reading has been
 *      seen, a failed fetch keeps the numbers and puts a banner over them
 *      saying what went wrong and how old they now are. The words-only screens
 *      are reserved for having nothing to show at all.
 *
 * The idea running through all three is that the screen must never be able to
 * lie by omission. A number with no indicator means it is current; that is a
 * promise, and every branch here exists to keep it.
 *
 * Added after stage 8 landed: two gauge arcs around the rim, one per window,
 * so the readings can be taken in at a glance without reading digits. They
 * cost about 1.5 KB and no RAM, and they brought one layout change with them
 * -- the status banner moved from the bottom of the screen to the middle,
 * because the ring took the space it used to occupy. See the note above
 * BANNER_MAX_CHARS for that story, and the one above CONTENT_R for the rule
 * that keeps text and ring out of each other's way.
 *
 * Success looks like: a colour cycle, "CONNECTING", then both percentages with
 * their reset times, updating quietly every 45 seconds. pc_service must be
 * RUNNING on the PC this chip is configured to poll (see config.h) --
 * otherwise the screen says NO LINK, which is itself the correct behaviour.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED, top to bottom:
 *
 *   1. the secrets.h guard      -- fail the build early with a clear message
 *   2. includes                 -- and what each one is actually for
 *   3. module state             -- the handful of file-scope variables
 *   4. WiFi event handlers      -- code the WiFi driver calls back into
 *   5. the JSON contract        -- stage 5: usage_t and its parser
 *   6. the screen               -- palette, layout, staleness, rendering
 *   7. the gauge arcs           -- geometry, state, and drawing the delta
 *   8. the request and the loop -- stage 8: one fetch, then forever
 *   9. wifi_start()             -- one-time setup, in dependency order
 *  10. app_main()               -- the entry point; where execution begins
 *
 * One rule that is invisible in the code: gc9a01 is not thread-safe, and
 * drawing happens from two places -- app_main (the boot messages) and
 * usage_task (everything after). They never overlap, because app_main draws
 * only before creating that task and never again. That is an ordering
 * guarantee held by convention, with nothing enforcing it: a second drawing
 * task needs a mutex first.
 *
 * The control flow is NOT top-to-bottom like a script. app_main() sets things
 * up and then *sleeps*; the interesting work happens on other tasks -- the
 * event handlers, which run whenever the radio has news, and usage_task, which
 * owns the display. If you read only one comment in this file, make it the one
 * above s_reconnect_timer -- that distinction is the source of the one real
 * bug this file has had.
 * ---------------------------------------------------------------------------
 */

/* secrets.h used to be required here, with a hard #error if it was missing.
 * It is now optional, and the check moved to config.c: NVS can supply every
 * value on its own, and secrets.h -- when present -- supplies the defaults.
 * See config.h for the whole arrangement. */

#include <stdio.h>                   /* printf, and snprintf for the safe
                                      * bounded string copies in the parser  */
#include <string.h>                  /* memcpy, for copying credentials;
                                      * memset, for zeroing the parsed struct */
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
#include "cJSON.h"                   /* stage 5: the JSON parser (IDF's own
                                      * `json` component -- added to REQUIRES
                                      * in CMakeLists.txt, same as the HTTP
                                      * client was; no download needed)     */
#include "esp_log.h"                 /* ESP_LOGI / ESP_LOGW / ESP_LOGE      */
#include "esp_system.h"              /* esp_get_free_heap_size -- stage 8
                                      * watches it, because this is the
                                      * first code here that runs forever  */

#include "gc9a01.h"                  /* stage 6: the round display driver,
                                      * hand-rolled on spi_master + gpio    */

#include "config.h"                  /* the four runtime values: NVS first,
                                      * secrets.h as the fallback           */
#include "button.h"                  /* the setup button on D1              */

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
static EventGroupHandle_t s_events;

/* The configuration this run is using: WiFi credentials, and where pc_service
 * lives. Filled once by config_load() at the top of app_main, before anything
 * else reads it, and never written again -- which is what makes it safe for
 * the event handlers and usage_task to read without a lock. */
static app_config_t s_cfg;
#define WIFI_CONNECTED_BIT BIT0

/* Set by the button task when a long press completes; cleared by usage_task
 * once it has acted on it. It lives in the same group as the WiFi bit for one
 * concrete reason: usage_task sometimes blocks waiting for the radio to come
 * back, and FreeRTOS cannot wait on two event groups at once. A button in a
 * group of its own would therefore be ignored during exactly the situation
 * where you are most likely to press it -- the network is broken and you want
 * to reconfigure it. */
#define BUTTON_SETUP_BIT   BIT1

/* Reconnect attempts since the last success. Only used for logging at this
 * stage -- we retry forever, because a desk gadget that gives up after five
 * tries is useless if the router reboots overnight. Proper backoff and a
 * visible "disconnected" state on the display are stage 8. */
static int s_retry_count = 0;

/* True once a DHCP lease has ever been obtained. It separates "has not
 * associated yet" from "was connected and lost it" -- indistinguishable to the
 * radio, but the difference between showing CONNECTING and showing NO WIFI,
 * and therefore between a gadget that looks like it is starting up and one
 * that looks broken every time it boots.
 *
 * Written on the event loop task, read on the display task. `volatile` because
 * the compiler must not cache it in a register across the loop below; a single
 * aligned bool needs nothing stronger than that on this chip. */
static volatile bool s_ever_connected = false;

/* How long to wait before the next reconnect attempt.
 *
 * Fixed at 2 seconds through stage 7, which is right for the common case and
 * wrong for the uncommon one. STATUS.md records ordinary boots needing up to
 * three retries before associating -- so the first few must stay fast, or the
 * gadget takes a minute to start on a network it can perfectly well join. But
 * a router that is off for the night should not be probed eighteen hundred
 * times an hour to no purpose.
 *
 * The policy is unchanged and deliberate: retry forever. A desk gadget that
 * gives up after five attempts is useless the first time the router reboots
 * overnight. This only changes the spacing. */
static uint32_t reconnect_delay_ms(int retry)
{
    if (retry <= 3)  { return 2000; }    /* the observed boot case          */
    if (retry <= 10) { return 5000; }    /* a brief outage                  */
    return 30000;                        /* something is off; stop hammering */
}

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
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        return;                  /* the outcome arrives later, as an event */
    }

    /* If this is not handled here, nothing retries at all.
     *
     * The whole retry chain is driven by WIFI_EVENT_STA_DISCONNECTED re-arming
     * this timer -- but that event only exists if an association was actually
     * attempted. A synchronous failure means it was not, so no event is ever
     * posted, no timer is ever re-armed, and the chip sits on NO WIFI until
     * someone power-cycles it, with nothing in the log to explain why. That
     * would quietly falsify the one promise stage 8 makes about WiFi: that it
     * recovers unattended.
     *
     * So this path re-arms itself. The tally is bumped too, or a persistent
     * failure would spin at the same short delay forever instead of backing
     * off like every other retry. */
    ESP_LOGW(TAG, "esp_wifi_connect() failed: %s -- retrying anyway",
             esp_err_to_name(err));
    s_retry_count++;

    esp_timer_stop(s_reconnect_timer);
    esp_err_t rearm = esp_timer_start_once(
        s_reconnect_timer, (uint64_t)reconnect_delay_ms(s_retry_count) * 1000);
    if (rearm != ESP_OK) {
        /* Logged rather than ESP_ERROR_CHECKed: panicking inside the recovery
         * path would be a worse outcome than the fault being recovered from. */
        ESP_LOGE(TAG, "could not re-arm the reconnect timer: %s",
                 esp_err_to_name(rearm));
    }
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
        ESP_LOGI(TAG, "wifi started, connecting to \"%s\"...", s_cfg.ssid);
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;

        /* Clear the flag first: nothing waiting on it should go on believing
         * we are connected while we retry. */
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);

        /* The reason code is the single most useful number when this stage
         * fails. Common ones: 201 = AP not found (wrong SSID, or the network
         * is 5 GHz only -- this chip is 2.4 GHz only), 15 = 4-way handshake
         * timeout (wrong password), 2 = AUTH_EXPIRE (seen routinely at boot
         * on WPA3, and it recovers on its own), 205 = connection lost. */
        /* Incremented on its own line, deliberately -- do not fold it back
         * into the log call. ESP_LOGW expands to
         * `if (LOG_LOCAL_LEVEL >= ESP_LOG_WARN) ...`, so a side effect in its
         * argument list vanishes along with the log line if this build's log
         * level is ever lowered past WARN (a normal thing to do for a quiet
         * release build). That was harmless while the counter only fed a
         * printed number. It stopped being harmless when reconnect_delay_ms()
         * started reading it: the tally would sit at zero, the backoff would
         * flatten to a fixed 2 seconds, and an absent router would be probed
         * eighteen hundred times an hour -- the exact behaviour the backoff
         * was added to stop, reappearing silently from a config change in a
         * different file. */
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected (reason %d), retry %d", e->reason, s_retry_count);

        /* Hand the wait to esp_timer and return immediately, so this handler
         * never blocks the event loop. See s_reconnect_timer above.
         *
         * The stop() guards against re-arming a timer that is already running
         * (two disconnects in quick succession), which would otherwise return
         * an error; it is a harmless no-op when the timer is idle. esp_timer
         * counts in MICROseconds, hence 2000 * 1000 for two seconds. */
        uint32_t delay_ms = reconnect_delay_ms(s_retry_count);
        ESP_LOGW(TAG, "retrying in %" PRIu32 " ms", delay_ms);

        esp_timer_stop(s_reconnect_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer,
                                             (uint64_t)delay_ms * 1000));
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

    s_retry_count = 0;               /* a success resets the retry tally,
                                      * and with it the backoff spacing     */
    s_ever_connected = true;

    /* IPSTR and IP2STR are a matched pair of ESP-IDF macros for printing an
     * IP address: IPSTR expands to the "%d.%d.%d.%d" format string, IP2STR
     * expands to the four comma-separated byte arguments that fill it. They
     * are always used together. */
    ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "netmask: " IPSTR ", gateway: " IPSTR,
             IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));

    /* Setting the bit is what wakes app_main out of xEventGroupWaitBits. */
    xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
}

/* --- stage 5: fetching and parsing the usage JSON ------------------------ */

/* The URL pc_service is polled at, assembled once at startup.
 *
 * This used to be a compile-time constant -- "http://" PC_SERVICE_HOST ":"
 * STRINGIFY(PC_SERVICE_PORT) "/usage", three macros pasted together by the
 * preprocessor into a single string literal. That was genuinely the nicer
 * arrangement: no snprintf, no buffer, nothing to get wrong at runtime. It is
 * the price of being able to change the service address without a rebuild.
 * The host and port simply are not known until config_load() has run, so the
 * string cannot exist before then.
 *
 * 128 bytes is comfortable rather than tight: the longest value this can hold
 * is "http://" (7) + a 63-character host + ":65535" (6) + "/usage" (6) + NUL,
 * which is 83. snprintf truncates rather than overruns if that arithmetic is
 * ever wrong.
 *
 * Note pc_service serves plain HTTP: no TLS on the LAN, by design
 * (ARCHITECTURE.md's "trust boundary is the home network"), which is also why
 * stage 4 deliberately tested without it. */
static char s_usage_url[128];

/* Where the response body accumulates.
 *
 * The body does NOT arrive in one piece: esp_http_client hands it to us in
 * chunks as TCP segments land, calling our event handler once per chunk. That
 * is what stage 4 existed to demonstrate, and this stage is where it pays
 * off: cJSON needs the whole document at once, and would fail on a fragment.
 * So the handler's job is to append, and only the code after the request
 * completes gets to look at the result.
 *
 * `len` is also the authoritative answer to "how much body arrived". Do not
 * substitute the Content-Length header: stage 4 hit a chunked response where
 * esp_http_client_get_content_length() returns -1, and a parser gated on that
 * number would have refused a perfectly good document. */
typedef struct {
    char *buf;          /* caller-owned storage                             */
    int   cap;          /* its size in bytes, including room for the NUL    */
    int   len;          /* bytes written so far                             */
    bool  truncated;    /* true if the response was bigger than cap         */
} body_buf_t;

/* 4 KB is generous: the real payload is around 250 bytes of JSON. The cap
 * exists so a surprisingly large response cannot walk off the end of the
 * buffer -- "truncate and say so" beats both "corrupt memory" and "silently
 * return half a document". Truncation is treated as a hard failure below
 * rather than something to parse anyway: half a JSON document is not a
 * smaller document, it is a broken one. */
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


/* --- reading the contract out of the JSON --------------------------------- */

/* The parsed form of ARCHITECTURE.md's contract: everything the display will
 * eventually need, and nothing else. Filling this in is the whole point of
 * stage 5; stage 7 renders it.
 *
 * Absent or null fields are represented rather than being an error, because
 * the contract genuinely allows them: a reset time can arrive as JSON null if
 * pc_service could not parse the upstream timestamp, and the "updated"/"now"
 * pair is absent whenever there has been no successful poll. Only the two
 * percentages are mandatory -- without them there is nothing to show. */
#define TIME_STR_CAP 24    /* "Fri 17:00" is 9 bytes, but %a is locale-
                            * dependent on the PC: a non-English Windows can
                            * send a longer, multi-byte weekday. Truncating
                            * safely is snprintf's job below; this cap just
                            * has to be comfortably larger than "HH:MM".   */

typedef struct {
    int     five_pct;                        /* 0-100, already rounded by PC */
    int     seven_pct;
    char    five_resets_at[TIME_STR_CAP];    /* "" when the field was null   */
    char    seven_resets_at[TIME_STR_CAP];
    int64_t five_resets_epoch;               /* 0 when absent                */
    int64_t seven_resets_epoch;
    char    updated_at[TIME_STR_CAP];
    int64_t updated_epoch;
    int64_t now_epoch;                       /* the PC's clock, for age math */
    bool    stale;                           /* true = last-known, not fresh */
} usage_t;

/* Three small accessors, so the parse below reads as a list of fields rather
 * than a wall of NULL checks. Each one answers "is this field present AND the
 * type I expect?" -- and a JSON null answers no, which is exactly right: the
 * cJSON type check does the null-handling for free. */

static bool json_get_int(const cJSON *obj, const char *key, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    /* +0.5 so a float that slipped through rounds rather than truncating. The
     * upstream endpoint sends utilization as 24.0 and pc_service rounds it,
     * but this is a contract with another program -- not an assumption to bet
     * on. Percentages are never negative, so adding a half is safe here. */
    *out = (int)(cJSON_GetNumberValue(item) + 0.5);
    return true;
}

static bool json_get_epoch(const cJSON *obj, const char *key, int64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    /* Deliberately the double, not item->valueint. cJSON stores every number
     * twice: as a double, and as an `int` it clamps to INT_MAX. A Unix epoch
     * (1.79e9) still fits in a 32-bit int today, but stops fitting in 2038 --
     * at which point valueint would silently pin to 2147483647. A double
     * represents every integer up to 2^53 exactly, so it has no such cliff. */
    *out = (int64_t)cJSON_GetNumberValue(item);
    return true;
}

static void json_get_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *value = cJSON_GetStringValue(item);   /* NULL unless a string */

    /* snprintf, not strcpy: it always NUL-terminates and never writes past
     * cap, so a longer-than-expected weekday name truncates instead of
     * corrupting the struct. An absent field becomes "", which print_usage
     * renders as "--". */
    snprintf(out, cap, "%s", value ? value : "");
}

/* Turns the accumulated body into a usage_t. Returns false if the response
 * was not JSON at all, or was JSON that does not carry the two percentages.
 *
 * Note `len` rather than a NUL-terminated string: cJSON_ParseWithLength can
 * never read past the end of the buffer even if the terminator went missing,
 * and we have an exact byte count from the accumulator anyway. */
static bool parse_usage(const char *json, int len, usage_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(json, (size_t)len);
    if (root == NULL) {
        /* cJSON_GetErrorPtr points into the buffer we just handed in, so the
         * subtraction gives the byte offset where parsing gave up -- much more
         * useful than "parse failed" when the reply turns out to be an HTML
         * error page from something that is not pc_service. (It is global
         * state inside cJSON, valid only until the next parse on any task;
         * fine here, where one task does all the parsing.) */
        const char *stop = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "body is not valid JSON (parser gave up at byte %d of %d)",
                 stop ? (int)(stop - json) : -1, len);
        return false;
    }

    bool ok = json_get_int(root, "five_hour_pct", &out->five_pct)
           && json_get_int(root, "seven_day_pct", &out->seven_pct);

    if (!ok) {
        ESP_LOGE(TAG, "valid JSON, but five_hour_pct/seven_day_pct are missing "
                      "or not numbers -- pc_service and this firmware disagree "
                      "about the contract in ARCHITECTURE.md");
    } else {
        json_get_str(root, "five_hour_resets_at", out->five_resets_at,
                     sizeof(out->five_resets_at));
        json_get_str(root, "seven_day_resets_at", out->seven_resets_at,
                     sizeof(out->seven_resets_at));
        json_get_str(root, "updated_at", out->updated_at,
                     sizeof(out->updated_at));

        json_get_epoch(root, "five_hour_resets_epoch", &out->five_resets_epoch);
        json_get_epoch(root, "seven_day_resets_epoch", &out->seven_resets_epoch);
        json_get_epoch(root, "updated_epoch",          &out->updated_epoch);
        json_get_epoch(root, "now_epoch",              &out->now_epoch);

        /* cJSON_IsTrue is false for absent, null, and non-boolean alike. That
         * is the safe default because it is not the only freshness signal:
         * updated_epoch against now_epoch gives the true age independently, so
         * a missing `stale` field degrades to "trust the clock" rather than
         * flashing a stale badge over a typo'd field name. data_age_seconds()
         * is where that clock is read, and build_banner where it becomes a
         * decision rather than a printed number. */
        out->stale = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stale"));
    }

    /* One Delete for the whole tree. cJSON allocates every node on the heap
     * and frees children with their parent, so this single call is the
     * complete cleanup -- but skipping it leaks the lot, which a once-a-minute
     * refresh loop would notice within a day. */
    cJSON_Delete(root);
    return ok;
}

/* Pulls just the "error" string out of a 503 body. A separate, tiny function
 * because a 503 is a different document (ARCHITECTURE.md: `{stale, error}`)
 * and running it through parse_usage would only report the percentages as
 * missing, which is true but unhelpful. */
static void parse_error_reason(const char *json, int len, char *out, size_t cap)
{
    snprintf(out, cap, "%s", "no reason given");

    cJSON *root = cJSON_ParseWithLength(json, (size_t)len);
    if (root == NULL) {
        return;
    }
    const char *reason = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "error"));
    if (reason != NULL) {
        snprintf(out, cap, "%s", reason);
    }
    cJSON_Delete(root);
}

/* printf rather than ESP_LOGI, for the same reason stage 4 printed the body
 * that way: the log macros prefix and colour every line, which makes a small
 * aligned block hard to read. This is the human-facing proof that stage 5
 * worked, so it is worth laying out. */
static void print_usage(const usage_t *u)
{
    /* An empty string means the field was null or absent; "--" says that
     * plainly instead of printing nothing and looking like a formatting bug. */
    const char *five_at  = u->five_resets_at[0]  ? u->five_resets_at  : "--";
    const char *seven_at = u->seven_resets_at[0] ? u->seven_resets_at : "--";
    const char *updated  = u->updated_at[0]      ? u->updated_at      : "--";

    printf("---8<--- parsed ---8<---\n");
    printf("  5-hour : %3d%%   resets %-10s (epoch %" PRId64 ")\n",
           u->five_pct, five_at, u->five_resets_epoch);
    printf("  7-day  : %3d%%   resets %-10s (epoch %" PRId64 ")\n",
           u->seven_pct, seven_at, u->seven_resets_epoch);
    printf("  updated %s   stale: %s\n", updated, u->stale ? "YES" : "no");

    /* The chip has no reliable clock of its own (no RTC battery, no SNTP), and
     * that is exactly why the contract ships now_epoch alongside updated_epoch:
     * both come from the PC, so subtracting them gives a true age without the
     * chip needing to know what time it is. data_age_seconds() computes the
     * same number for the display, with one addition: the time elapsed on this
     * chip since the fetch, which is what keeps the age honest once the
     * service stops answering at all. */
    if (u->updated_epoch > 0 && u->now_epoch > 0) {
        printf("  data age: %" PRId64 "s by the PC's clock\n",
               u->now_epoch - u->updated_epoch);
    }
    printf("---8<--- end ------8<---\n");
}

/* --- the screen ----------------------------------------------------------- */

/* The palette. Defined here rather than in gc9a01.h because these are choices
 * about this product, not facts about the panel -- the driver supplies
 * GC9A01_RGB and stays out of it. */
#define COL_BG     GC9A01_BLACK
#define COL_LABEL  GC9A01_RGB(115, 120, 135)   /* dim: labels are context   */
#define COL_TIME   GC9A01_RGB(190, 195, 210)   /* reset times, readable     */
#define COL_OK     GC9A01_RGB( 45, 200,  95)
#define COL_WARN   GC9A01_RGB(240, 175,  45)
#define COL_ALERT  GC9A01_RGB(235,  65,  60)
#define COL_DEAD   GC9A01_RGB( 95, 100, 110)   /* stage 8: a number that is
                                                * no longer about now       */

/* The layout, as named constants rather than numbers buried in the drawing
 * code, because the useful question about a round display is always "does this
 * still fit inside the circle" and that is much easier to answer from a list.
 *
 * What it looks like, with the y of each row on the left:
 *
 *              . - - - - - - - - .          the 5-hour arc runs along the
 *          .  '   _______________  ` .      rim of the top half, clockwise
 *        '      /                 \    `    from nine o'clock
 *   28  |          5 - H O U R          |
 *   46  |             7 0 %             |   <- big, colour-banded
 *   86  |            1 5 : 3 0          |
 *  104  |        - - - - - - - -        |   <- divider, or the banner
 *  122  |           7 - D A Y           |      when something is wrong
 *  140  |             1 6 %             |
 *  180  |          T H U  1 9 : 0 0     |
 *        .      \_________________/    ,    the 7-day arc runs along the
 *          .  ,                    . '      rim of the bottom half, also
 *              ' - - - - - - - - '          clockwise, from three o'clock
 *
 * Each y is the TOP row of that line's glyphs; a line at SCALE_LABEL is 14
 * pixels tall and one at SCALE_VALUE is 35, so ROW_5H_VALUE at 46 occupies
 * rows 46 to 80. That matters more than it sounds: on a round panel the
 * bottom of a glyph can fall outside the circle while its top is comfortably
 * inside.
 *
 * The panel is 240x240 but *round*: the controller addresses the full square
 * and the corners are simply behind the bezel. The usable half-width at a
 * given row y is sqrt(120^2 - (y-120)^2), so the top and bottom rows here are
 * the tight ones -- at y=28 there are about 154 usable pixels, at y=202 about
 * 146. Every line below is comfortably inside that. */
#define ROW_5H_LABEL   28
#define ROW_5H_VALUE   46
#define ROW_5H_RESET   86
#define ROW_MIDDLE    104
#define ROW_7D_LABEL  122
#define ROW_7D_VALUE  140
#define ROW_7D_RESET  180

/* The hairline that lives in the middle slot whenever there is no banner to
 * show. Offset to sit centred within that slot's 14-pixel band. */
#define DIVIDER_X       60
#define DIVIDER_W      120
#define DIVIDER_INSET    6

/* The gauge ring, and the boundary it imposes on everything else.
 *
 * Six pixels thick: enough to read as a gauge from across a desk, thin enough
 * to look like an instrument rather than a pie chart. It was twelve first,
 * which was heavy.
 *
 * CONTENT_R is the important one. Everything that is not the ring -- every
 * glyph, every background fill behind a glyph, every row clear -- has to stay
 * inside this radius, or it will erase the ring the next time it is redrawn.
 * That is not hypothetical: the row clears were full panel width to begin
 * with, which cut two bites out of the ring on every text row that changed,
 * and the display only looked right until the first percentage moved. Keeping
 * the two radii adjacent, with the reason next to them, is the cheapest
 * defence against re-introducing it. */
#define ARC_R_OUT      118
#define ARC_R_IN       112
#define CONTENT_R      (ARC_R_IN - 2)

#define SCALE_LABEL  2       /* 12x14 px per character */
#define SCALE_VALUE  5       /* 30x35 -- the number you read from across a desk */

/* The banner is the one line that says whether to believe the numbers above
 * it, and where it sits has now been decided twice, for two different reasons.
 *
 * Stage 8 put it at the bottom, at y=202 rather than 205, because the circle
 * narrows fast down there: at 205 only eleven characters are visible and
 * "BAD DATA 12M" is twelve, so three pixels of headroom bought the longest
 * string this screen needs to say.
 *
 * The arcs then took that space away entirely. On the banner's bottom row the
 * ring occupies x=50..61, straight through text that spans 49..191 -- and
 * thinning it does not help, because what matters is the ring's *inner* edge:
 * at 6 pixels it sits at x=61 and at 12 it sat at x=73, both well inside the
 * banner either way. The ring has to be near the edge to read as a gauge and
 * the banner has to be wide to say anything useful. Shortening the banner to
 * fit caps it at seven characters, which loses the age, and the age is the
 * part that earns its place.
 *
 * So it moved to the vertical centre, where the circle is at its widest and
 * twelve characters clear the ring by 34 pixels on each side. It shares that
 * slot with the divider, which is what shows when there is nothing wrong --
 * so the banner appearing displaces something familiar, which makes it harder
 * to miss rather than easier. */
#define BANNER_MAX_CHARS 12
#define BANNER_CAP       (BANNER_MAX_CHARS + 1)

/* --- stage 8: how old is too old ------------------------------------------
 *
 * The chip refreshes every 45 seconds; pc_service polls Anthropic every 120.
 * So in normal running the numbers on screen are between 0 and 120 seconds
 * old, and that is not staleness -- that is just how the cache works.
 *
 * The thresholds below come from pc_service's documented backoff (120s ->
 * 240s -> 480s -> capped 900s, reset on success; see STATUS.md), because that
 * backoff is what decides how old the data can get while everything is in fact
 * working correctly:
 *
 *   one upstream 429  ->  next success at t=360s  ->  age reaches  6 minutes
 *   two in a row      ->  next success at t=840s  ->  age reaches 14 minutes
 *
 * Both of those are healthy, self-recovering conditions. A badge at 5 minutes
 * would fire on the first, and greying the numbers at 15 would fire on the
 * second. That is exactly the cry-wolf problem that pushed the poll interval
 * from 60s to 120s: an indicator that is wrong a third of the time is one you
 * learn to ignore, and then it cannot tell you the thing it exists for.
 *
 * So: badge above the one-429 case and below the 900s cap; grey out only past
 * anything the backoff can produce at all.
 *
 * Neither is the primary alarm. A PC that has actually gone away also fails
 * the fetch outright, which puts NO LINK on the screen within one 45s cycle.
 * These two thresholds are the backstop for the quieter failure -- a service
 * that is still answering, politely, with data from an hour ago. */
#define REFRESH_INTERVAL_MS  45000    /* the brief asks for 30-60s           */
#define STALE_BADGE_AGE_S      600    /* 10 min: badge it                    */
#define STALE_DEAD_AGE_S      1800    /* 30 min: stop colouring it as a fact */

/* The last reading that parsed, and when this chip received it.
 *
 * Kept so a failed fetch can go on showing real numbers instead of blanking to
 * an error -- which is what ARCHITECTURE.md asks for, and the difference
 * between a gadget that degrades and one that simply breaks. */
static usage_t s_last_good;
static bool    s_have_good;
static int64_t s_last_good_us;      /* esp_timer_get_time() at that moment */

/* How old the numbers on screen actually are, in seconds; -1 if there are none
 * yet.
 *
 * Two clocks add up here, and both halves are load-bearing:
 *
 *   now_epoch - updated_epoch   how stale the data already was when the PC
 *                               handed it over. Both values come from the PC,
 *                               so this needs no clock on the chip at all --
 *                               which is the whole reason the contract ships
 *                               the pair instead of just an "HH:MM" string.
 *
 *   esp_timer since the fetch   how long ago that was. This is the half that
 *                               keeps working during an outage: with the
 *                               service unreachable no new now_epoch arrives,
 *                               so without it the age would freeze at whatever
 *                               it was when the link died, and a dead PC would
 *                               look eternally fresh.
 *
 * esp_timer counts microseconds since boot and never runs backwards. This chip
 * cannot tell you what time it is; it can measure an interval exactly. */
static int64_t data_age_seconds(void)
{
    if (!s_have_good) {
        return -1;
    }

    int64_t reported = 0;
    if (s_last_good.updated_epoch > 0 && s_last_good.now_epoch > 0) {
        reported = s_last_good.now_epoch - s_last_good.updated_epoch;
        if (reported < 0) {
            reported = 0;    /* the PC's own two timestamps disagreeing is a
                              * PC problem; it is not a negative age         */
        }
    }

    return reported + (esp_timer_get_time() - s_last_good_us) / 1000000;
}

/* Green while there is room, amber when it is worth noticing, red when it is
 * nearly gone. The thresholds are a judgement, not a standard: 50% of a
 * five-hour window with hours left is fine, 80% is not. */
static uint16_t usage_colour(int pct)
{
    if (pct < 50) { return COL_OK; }
    if (pct < 80) { return COL_WARN; }
    return COL_ALERT;
}

/* Horizontal centring is worth a helper because every line on this display is
 * centred -- on a round panel there is no left margin to align to. */
static void draw_centred(int y, const char *text, uint16_t fg, int scale)
{
    const int cell = GC9A01_CHAR_W * scale;   /* full advance per character */

    /* How many characters fit *inside the circle* on this row.
     *
     * GC9A01_WIDTH would be the answer on a rectangular panel, and using it
     * here was a real (if hard to reach) bug: this panel is round, so the
     * controller addresses the full 240x240 square while the corners sit
     * behind the bezel. The usable width depends on how far the row is from
     * the vertical centre -- at ROW_7D_RESET only about 190 pixels are
     * visible, so the square assumption permitted twenty characters where
     * fifteen can be seen.
     *
     * Why that mattered more than it looks: the ">" marking a deliberate
     * truncation would itself have been drawn outside the circle, so an
     * over-long string would have lost glyphs off *both* ends with nothing on
     * screen to say it had been cut -- which is the exact silent failure this
     * guard exists to prevent. Reachable from network data, too: the reset
     * times come from the PC, and the contract allows a longer non-English
     * weekday than "Thu 19:00".
     *
     * The narrow row is the top of the glyph for text above centre and the
     * bottom for text below it, so measure at whichever is further out -- which
     * is what gc9a01_chord_half is asked twice for.
     *
     * Note the radius is CONTENT_R, not the panel edge: the gauge ring owns
     * the rim, so as far as text is concerned the display is smaller than it
     * looks. Sanity figures, since they drove the layout: this allows 10
     * characters at ROW_5H_LABEL, 12 at ROW_7D_RESET, and 18 at ROW_MIDDLE
     * where the circle is near its widest. The banner needs 12, which is why
     * it lives in the middle -- at the old bottom row it had 11. */
    const int top_half = gc9a01_chord_half(y, CONTENT_R);
    const int bot_half = gc9a01_chord_half(y + GC9A01_CHAR_H * scale - 1, CONTENT_R);
    const int usable   = 2 * ((top_half < bot_half) ? top_half : bot_half);

    int max_chars = usable / cell;

    if (max_chars < 1) {
        return;    /* this row lies entirely behind the bezel; drawing into it
                    * would be invisible, and the truncation maths below
                    * assumes there is room for at least one character */
    }

    char clipped[32];
    if (max_chars > (int)sizeof(clipped) - 2) {
        max_chars = (int)sizeof(clipped) - 2;   /* room for '>' and the NUL */
    }
    if ((int)strlen(text) > max_chars) {
        snprintf(clipped, sizeof(clipped), "%.*s>", max_chars - 1, text);
        text = clipped;
    }

    const int len = (int)strlen(text);

    /* gc9a01_text_width reports *ink*, excluding the last character's spacer
     * column, which is what makes centring look right. But draw_text needs
     * the whole six-column cell on screen for every glyph, so centring on ink
     * alone can push that final cell one scale-step past the edge and lose
     * the last character. Nudge left when that happens. */
    int x = (GC9A01_WIDTH - gc9a01_text_width(text, scale)) / 2;
    if (x + len * cell > GC9A01_WIDTH) {
        x = GC9A01_WIDTH - len * cell;
    }
    if (x < 0) {
        x = 0;
    }

    gc9a01_draw_text(x, y, text, fg, COL_BG, scale);
}

/* --- stage 8: a model of what is currently on the glass --------------------
 *
 * Stage 7 repainted all 240x240 every time it drew. Once, at boot, that is
 * invisible. Once every 45 seconds it is a black flash you cannot help
 * watching -- and it happens whether or not a single digit changed, which is
 * the part that makes a working gadget feel broken.
 *
 * The fix is not a faster fill, it is not filling: remember what was drawn,
 * compare, and touch only the lines that actually differ. In steady state
 * nothing differs at all, so a refresh writes zero pixels and the screen is
 * simply still.
 *
 * Rows, not pixels. Every line here is centred, so a shorter string starts
 * further right and would leave the tail of the previous one stranded beside
 * it. Clearing the full width of the row band first makes that impossible, and
 * a 240x14 band is far too cheap to be worth outsmarting.
 *
 * All of this state is plain, unlocked, and touched from exactly one task --
 * see the note at the top of the file. It stays correct only while that is
 * true. */

typedef enum {
    SCREEN_NOTHING,    /* nothing drawn yet: the first draw must be a full one */
    SCREEN_MESSAGE,    /* words only: CONNECTING, NO LINK, ...                 */
    SCREEN_USAGE,      /* the real layout: labels, numbers, reset times        */
} screen_mode_t;

typedef enum {
    SLOT_5H_VALUE,
    SLOT_5H_RESET,
    SLOT_7D_VALUE,
    SLOT_7D_RESET,
    SLOT_MIDDLE,      /* the divider, or the banner when there is one */
    SLOT_COUNT
} slot_id_t;

/* One redrawable line. `text` and `fg` hold what is *on the panel*, not what
 * is wanted -- the comparison between those two is the whole mechanism.
 *
 * The buffer has to be the largest thing any slot can hold: a reset time from
 * the PC (TIME_STR_CAP) or a banner (BANNER_CAP), whichever is bigger. */
typedef struct {
    int      y;
    int      scale;
    char     text[TIME_STR_CAP > BANNER_CAP ? TIME_STR_CAP : BANNER_CAP];
    uint16_t fg;
} slot_t;

static slot_t s_slots[SLOT_COUNT] = {
    [SLOT_5H_VALUE] = { .y = ROW_5H_VALUE, .scale = SCALE_VALUE },
    [SLOT_5H_RESET] = { .y = ROW_5H_RESET, .scale = SCALE_LABEL },
    [SLOT_7D_VALUE] = { .y = ROW_7D_VALUE, .scale = SCALE_VALUE },
    [SLOT_7D_RESET] = { .y = ROW_7D_RESET, .scale = SCALE_LABEL },
    [SLOT_MIDDLE]   = { .y = ROW_MIDDLE,   .scale = SCALE_LABEL },
};

/* --- the two usage arcs --------------------------------------------------
 *
 * A ring around the rim, split into halves that tile it exactly: the 5-hour
 * gauge is anchored at nine o'clock and sweeps clockwise over the top, the
 * 7-day gauge is anchored at three o'clock and sweeps clockwise under the
 * bottom. Each is capped at a half turn, so they can never run into each
 * other -- at 100% and 100% they meet at nine and three and close the circle.
 *
 * The arc takes its colour from the same usage_colour() the number does, so
 * the two can never disagree about how alarming a reading is. There is no
 * unfilled "track" behind the arc: an empty rim reads as zero perfectly well,
 * and a track would compete with the numbers for attention.
 *
 * Like the text slots, these remember what is drawn -- but the redraw is
 * cheaper still, because an arc only ever grows or shrinks. A tick from 70% to
 * 71% paints the 1.8-degree wedge between them and touches nothing else. Only
 * a colour-band crossing costs a full repaint. */
#define ARC_CX        (GC9A01_WIDTH  / 2)
#define ARC_CY        (GC9A01_HEIGHT / 2)
#define ARC_5H_START  270    /* nine o'clock  */
#define ARC_7D_START   90    /* three o'clock */

typedef enum { ARC_5H, ARC_7D, ARC_COUNT } arc_id_t;

typedef struct {
    int      start_deg;
    int      pct;        /* what is on the panel now; -1 = nothing drawn yet */
    uint16_t fg;
} arc_t;

static arc_t s_arcs[ARC_COUNT] = {
    [ARC_5H] = { .start_deg = ARC_5H_START, .pct = -1 },
    [ARC_7D] = { .start_deg = ARC_7D_START, .pct = -1 },
};

/* 1.8 degrees per percent, rounded, so 100% is exactly a half turn. */
static int arc_sweep(int pct)
{
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }
    return (180 * pct + 50) / 100;
}

static screen_mode_t s_mode = SCREEN_NOTHING;
static char s_msg1[24];    /* the message screen's two lines, remembered for  */
static char s_msg2[24];    /* the same reason: don't repaint an unchanged one */

/* Clear one text row band -- but only the part inside the gauge ring.
 *
 * This began as a single full-width fill_rect, and that was the bug behind the
 * first arc build: the ring is an annulus at the rim, so a 240-pixel clear
 * cuts through both of its arms on every row it covers. It looked correct
 * until it wasn't, because the arcs are drawn last on a full repaint -- so the
 * first frame was perfect and the first percentage change bit two chunks out
 * of the ring, one per arm, which the delta-wedge redraw never repairs.
 *
 * Clearing row by row out to CONTENT_R costs one short transfer per row rather
 * than one per band. That is more calls, and still nothing: a 35-pixel band is
 * 35 transfers of about 200 pixels, well under a millisecond in total. */
static void clear_band(int y, int h)
{
    for (int row = y; row < y + h; row++) {
        const int half = gc9a01_chord_half(row, CONTENT_R);
        if (half > 0) {
            gc9a01_fill_rect(GC9A01_WIDTH / 2 - half, row, 2 * half, 1, COL_BG);
        }
    }
}

/* Draw one line, but only if it is not already there.
 *
 * The empty string is a real value meaning "nothing on this row": the band is
 * cleared and nothing is drawn. That is how the stale badge disappears again
 * when the data goes fresh. */
static void set_slot(slot_id_t id, const char *text, uint16_t fg)
{
    slot_t *s = &s_slots[id];

    if (s->fg == fg && strcmp(s->text, text) == 0) {
        return;
    }

    clear_band(s->y, GC9A01_CHAR_H * s->scale);
    if (text[0] != '\0') {
        draw_centred(s->y, text, fg, s->scale);
    }

    snprintf(s->text, sizeof(s->text), "%s", text);
    s->fg = fg;
}

/* Forget everything drawn, so the next update paints unconditionally. Must
 * follow every full-screen fill: the panel is black again, and if the model
 * still claims otherwise, everything it believes unchanged stays blank.
 *
 * The arcs reset to -1 rather than 0, because those mean different things: 0%
 * is a real reading that happens to draw nothing, while -1 means "nothing has
 * been drawn at all", which is what lets the next update skip its erase pass. */
static void forget_drawn(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        s_slots[i].text[0] = '\0';
        s_slots[i].fg = COL_BG;
    }
    for (int i = 0; i < ARC_COUNT; i++) {
        s_arcs[i].pct = -1;
        s_arcs[i].fg  = COL_BG;
    }
}

/* Bring one arc to `pct`, drawing as little as possible.
 *
 * Three cases, and the middle two are why this is cheap. Growing paints only
 * the wedge between the old angle and the new; shrinking paints that same
 * wedge in the background colour, which erases it. Only a change of colour
 * needs the whole half-ring repainted, and that happens twice over the life of
 * a reading, at the 50% and 80% band edges. */
static void set_arc(arc_id_t id, int pct, uint16_t fg)
{
    arc_t *a = &s_arcs[id];

    if (a->pct == pct && a->fg == fg) {
        return;
    }

    const int old_sweep = (a->pct < 0) ? 0 : arc_sweep(a->pct);
    const int new_sweep = arc_sweep(pct);

    if (a->pct < 0) {
        /* First paint after a full-screen clear: the rim is already black, so
         * there is nothing to erase first. */
        gc9a01_fill_arc(ARC_CX, ARC_CY, ARC_R_IN, ARC_R_OUT,
                        a->start_deg, new_sweep, fg);

    } else if (a->fg != fg) {
        /* A band crossing recolours everything already drawn, so the old arc
         * has to go first. Erasing the full half turn rather than just the old
         * sweep costs the same and cannot leave a fragment behind if the two
         * ever disagree. */
        gc9a01_fill_arc(ARC_CX, ARC_CY, ARC_R_IN, ARC_R_OUT,
                        a->start_deg, 180, COL_BG);
        gc9a01_fill_arc(ARC_CX, ARC_CY, ARC_R_IN, ARC_R_OUT,
                        a->start_deg, new_sweep, fg);

    } else if (new_sweep > old_sweep) {
        gc9a01_fill_arc(ARC_CX, ARC_CY, ARC_R_IN, ARC_R_OUT,
                        a->start_deg + old_sweep, new_sweep - old_sweep, fg);

    } else if (new_sweep < old_sweep) {
        gc9a01_fill_arc(ARC_CX, ARC_CY, ARC_R_IN, ARC_R_OUT,
                        a->start_deg + new_sweep, old_sweep - new_sweep, COL_BG);
    }

    a->pct = pct;
    a->fg  = fg;
}

/* The middle band holds one of two things, so it gets its own setter rather
 * than going through set_slot: an empty banner is not an empty row here, it is
 * the divider. Keeping both in one slot is what makes the banner *displace*
 * something familiar rather than merely appear somewhere, which is harder to
 * overlook. */
static void set_middle(const char *text, uint16_t fg)
{
    slot_t *s = &s_slots[SLOT_MIDDLE];

    if (s->fg == fg && strcmp(s->text, text) == 0) {
        return;
    }

    clear_band(s->y, GC9A01_CHAR_H * s->scale);
    if (text[0] != '\0') {
        draw_centred(s->y, text, fg, s->scale);
    } else {
        /* A hairline, not a box. It separates the two readings without
         * competing with them for attention. */
        gc9a01_fill_rect(DIVIDER_X, s->y + DIVIDER_INSET, DIVIDER_W, 2, COL_LABEL);
    }

    snprintf(s->text, sizeof(s->text), "%s", text);
    s->fg = fg;
}

/* Compact enough for the banner's twelve characters, and never more than three
 * of them: "45S", "9M", "23H", "99D". Deliberately coarse -- the question it
 * answers is "should I believe the number above this", and nobody needs
 * seconds of precision to decide that. */
static void format_age(int64_t secs, char *out, size_t cap)
{
    if (secs < 0)       { snprintf(out, cap, "?");                        return; }
    if (secs < 60)      { snprintf(out, cap, "%dS", (int)secs);           return; }
    if (secs < 3600)    { snprintf(out, cap, "%dM", (int)(secs / 60));    return; }
    if (secs < 86400)   { snprintf(out, cap, "%dH", (int)(secs / 3600));  return; }
    if (secs < 8640000) { snprintf(out, cap, "%dD", (int)(secs / 86400)); return; }

    /* Past a hundred days the exact number has stopped being the interesting
     * part of the message. */
    snprintf(out, cap, "OLD");
}

/* Builds the bottom line and returns the colour to draw it in. An empty result
 * means "say nothing", which is itself the design: the badge appears only when
 * something is wrong, so its presence is information rather than decoration. */
static uint16_t build_banner(const usage_t *u, const char *reason,
                             int64_t age_s, char *out, size_t cap)
{
    char age[8];
    format_age(age_s, age, sizeof(age));

    /* Red once the numbers have stopped being current, amber while they are
     * merely old. The same rule for both branches below. */
    const uint16_t severity =
        (age_s >= 0 && age_s >= STALE_DEAD_AGE_S) ? COL_ALERT : COL_WARN;

    /* A reason means this refresh failed outright, and that outranks whatever
     * the payload said about itself -- the payload is, by definition, the
     * previous one. */
    if (reason != NULL) {
        snprintf(out, cap, "%s %s", reason, age);
        return severity;
    }

    /* Two independent ways to be stale, and either one is enough on its own.
     * The flag is pc_service reporting that its own upstream call failed; the
     * age is this chip working it out from the clock pair. Neither can see
     * what the other sees -- the flag cannot arrive at all if the service is
     * unreachable, and the age cannot tell a slow poll from a broken one. */
    if (u->stale || (age_s >= 0 && age_s >= STALE_BADGE_AGE_S)) {
        snprintf(out, cap, "STALE %s", age);
        return severity;
    }

    out[0] = '\0';
    return COL_BG;
}

/* Anything that is not a reading: "CONNECTING", "NO LINK", and so on.
 *
 * Reached only when there is no last-known data to show instead -- see
 * show_failure. Once a real reading has arrived, a failure becomes a banner
 * over the numbers rather than a screen that replaces them. */
static void render_message(const char *line1, const char *line2)
{
    if (line2 == NULL) {
        line2 = "";
    }

    /* Already saying exactly this. Repainting would blink it once every
     * refresh for no reason, and a screen that blinks on a schedule teaches
     * you to stop looking at it. */
    if (s_mode == SCREEN_MESSAGE &&
        strcmp(s_msg1, line1) == 0 && strcmp(s_msg2, line2) == 0) {
        return;
    }

    gc9a01_fill_screen(COL_BG);
    draw_centred(100, line1, COL_TIME, 3);
    if (line2[0] != '\0') {
        draw_centred(140, line2, COL_LABEL, SCALE_LABEL);
    }

    snprintf(s_msg1, sizeof(s_msg1), "%s", line1);
    snprintf(s_msg2, sizeof(s_msg2), "%s", line2);
    s_mode = SCREEN_MESSAGE;
    forget_drawn();
}

/* The actual point of the whole project: usage_t, on the glass.
 *
 * `reason` is NULL when this reading is the one just fetched, or a short word
 * (BANNER_MAX_CHARS minus room for " 12M") when it is the last known reading
 * being shown because a fetch failed. `age_s` is how old the numbers are, or
 * -1 if that is unknown. */
static void render_usage(const usage_t *u, const char *reason, int64_t age_s)
{
    char buf[12];

    /* The parts that never change are drawn once, on arriving at this screen
     * from another. After that only the five slots below are touched, which is
     * what makes a refresh cost nothing when nothing has changed. */
    if (s_mode != SCREEN_USAGE) {
        gc9a01_fill_screen(COL_BG);
        forget_drawn();
        draw_centred(ROW_5H_LABEL, "5-HOUR", COL_LABEL, SCALE_LABEL);
        draw_centred(ROW_7D_LABEL, "7-DAY",  COL_LABEL, SCALE_LABEL);
        s_mode = SCREEN_USAGE;
        s_msg1[0] = '\0';
        s_msg2[0] = '\0';
    }

    /* Past the point where these numbers describe the present, they lose the
     * green/amber/red they earned and go flat grey. Losing the colour *is* the
     * signal, and a stronger one than the badge: a red 94% and a grey 94% mean
     * genuinely different things, and the grey one has no business alarming
     * anybody. The numbers stay on screen because they are still the last
     * thing known to be true -- they are just no longer a claim about now. */
    const bool dead = (age_s >= 0 && age_s >= STALE_DEAD_AGE_S);

    snprintf(buf, sizeof(buf), "%d%%", u->five_pct);
    set_slot(SLOT_5H_VALUE, buf, dead ? COL_DEAD : usage_colour(u->five_pct));
    /* "--" rather than an empty gap: the contract allows a null reset time,
     * and a blank line would read as a rendering bug rather than as missing
     * data. */
    set_slot(SLOT_5H_RESET, u->five_resets_at[0] ? u->five_resets_at : "--",
             dead ? COL_DEAD : COL_TIME);

    snprintf(buf, sizeof(buf), "%d%%", u->seven_pct);
    set_slot(SLOT_7D_VALUE, buf, dead ? COL_DEAD : usage_colour(u->seven_pct));
    set_slot(SLOT_7D_RESET, u->seven_resets_at[0] ? u->seven_resets_at : "--",
             dead ? COL_DEAD : COL_TIME);

    char banner[BANNER_CAP];
    uint16_t fg = build_banner(u, reason, age_s, banner, sizeof(banner));
    set_middle(banner, fg);

    /* The gauges last, so that if a redraw is ever interrupted the numbers are
     * already right -- they are the reading, and the arcs are the illustration
     * of it. Same colour as the number they belong to, including the flat grey
     * once the data is too old to be a claim about now: an arc that stayed
     * green while its number went grey would be the display contradicting
     * itself. */
    set_arc(ARC_5H, u->five_pct,  dead ? COL_DEAD : usage_colour(u->five_pct));
    set_arc(ARC_7D, u->seven_pct, dead ? COL_DEAD : usage_colour(u->seven_pct));
}

/* --- stage 8: the two ways the screen gets updated ------------------------ */

/* A fetch worked and parsed. Remember it -- including *when* it arrived, which
 * is what lets the age calculation survive a later outage -- and show it. */
static void show_usage(const usage_t *u)
{
    s_last_good    = *u;
    s_have_good    = true;
    s_last_good_us = esp_timer_get_time();

    render_usage(u, NULL, data_age_seconds());
}

/* A fetch did not work. `reason` goes in the banner, so it has to be short.
 * `line1`/`line2` are the fallback for when there is nothing better to show.
 *
 * ARCHITECTURE.md is specific about the choice being made here: keep showing
 * the last known values, visibly marked, rather than replacing them with an
 * error. An old number under a banner saying how old it is remains useful; a
 * screen reading only NO LINK has thrown away the last thing it knew. So the
 * words-only screen is reserved for the one case where that really is all
 * there is -- nothing has ever been fetched successfully. */
static void show_failure(const char *reason, const char *line1, const char *line2)
{
    if (s_have_good) {
        render_usage(&s_last_good, reason, data_age_seconds());
    } else {
        render_message(line1, line2);
    }
}

/* --- the request itself --------------------------------------------------- */

/* One request: perform it, and put whatever happened on the screen.
 *
 * Split out from the loop below for two reasons. The loop then reads as a
 * loop, and -- less cosmetically -- `body` is a local that is constructed
 * fresh on every call.
 *
 * That second part is not a style preference. The accumulator carries `len`
 * and `truncated`, and a version of this that hoisted them out of the loop to
 * "avoid re-initialising them" would append the second response to the first
 * and hand cJSON a document with two roots. It has been the known trap since
 * stage 5, and a local is what makes it structurally impossible rather than
 * merely remembered.
 *
 * `storage` is passed in rather than declared here because it is 4 KB and this
 * runs on a task with 8 KB of stack; it lives in .bss, owned by the caller. */
static void fetch_once(char *storage, int storage_cap)
{
    body_buf_t body = { .buf = storage, .cap = storage_cap };
    body.buf[0] = '\0';

    /* Fields we don't set stay zero, and zero means "the default" throughout
     * this struct. .user_data is the pointer handed back to our event handler
     * -- it is how the handler knows where to append without a global. */
    esp_http_client_config_t cfg = {
        .url           = s_usage_url,
        .method        = HTTP_METHOD_GET,
        .event_handler = on_http_event,
        .user_data     = &body,
        .timeout_ms    = 10000,   /* 10s: generous for a LAN, and short
                                   * enough that a black-holed connection
                                   * fails visibly instead of hanging      */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        /* Out of memory, which on this chip means something else has leaked.
         * Not fatal -- the loop will try again in 45 seconds -- but the screen
         * must not go on implying the numbers are current. */
        ESP_LOGE(TAG, "esp_http_client_init failed (out of memory?)");
        show_failure("NO LINK", "NO LINK", "OUT OF MEM");
        return;
    }

    /* Ask the server to close the connection once it has answered.
     *
     * esp_http_client sends no Connection header of its own, and under
     * HTTP/1.1 the absence of one means keep-alive. That leaves the server
     * holding the socket open after it replies, with a thread parked in a
     * read waiting for a second request that is never coming -- this client
     * makes one request and then closes. On Windows that close arrives on the
     * pending read as an abort (WinError 10054) rather than a clean
     * end-of-file, and pc_service printed a traceback for every fetch.
     *
     * Saying "close" out loud fixes both ends: the server closes the socket
     * itself, in order, and nothing is left parked. Keep-alive would be worth
     * having if this polled every few seconds, but the refresh interval is 45
     * -- far longer than any server holds an idle connection open, so the
     * connection would be dead before the next request anyway. */
    esp_http_client_set_header(client, "Connection", "close");

    /* perform() blocks until the whole exchange finishes: connect, request,
     * response, and every on_http_event call above. Note this is NOT wrapped
     * in ESP_ERROR_CHECK -- a failed network request is a normal condition to
     * report, not a reason to abort the program. That rule is the whole reason
     * this stage can have a "service unreachable" screen at all. */
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        /* Ask the radio again before naming a culprit.
         *
         * The loop checked WiFi before calling in here, but that was up to ten
         * seconds ago -- the length of the timeout this request may have just
         * spent. A link that dropped mid-request lands on this branch, and
         * calling that NO LINK would send someone to debug the PC over what is
         * actually a radio problem: exactly the misdiagnosis the NO WIFI /
         * NO LINK split was added to prevent. */
        if (!(xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "request failed (%s), and wifi is down -- the link "
                          "dropped mid-request, so this is not the PC",
                     esp_err_to_name(err));
            show_failure("NO WIFI", "NO WIFI", "RECONNECTING");
            esp_http_client_cleanup(client);
            return;
        }

        ESP_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
        /* Stage 4 already proved this chip can resolve DNS and reach the
         * internet, and the loop below has already confirmed WiFi is up before
         * calling in here -- so a failure at this point is almost certainly on
         * the PC side or in the configured address. Worth saying, because the
         * instinct is to blame WiFi. */
        ESP_LOGE(TAG, "wifi is up and stage 4 reached the internet from this "
                      "chip, so suspect: pc_service not running; the PC "
                      "firewall blocking this port; or the configured host "
                      "(logged as \"polling ...\" at startup) pointing at an "
                      "address DHCP has since handed to something else");
        show_failure("NO LINK", "NO LINK", "PC SERVICE");

    } else {
        int     status = esp_http_client_get_status_code(client);
        int64_t clen   = esp_http_client_get_content_length(client);

        /* PRId64 expands to the right length modifier for a 64-bit integer on
         * this platform. Hardcoding "%lld" happens to work here but warns or
         * breaks elsewhere; IDF builds with -Wall, so this is the habit.
         *
         * A content-length of -1 is not an error: it means the reply arrived
         * chunked, with no Content-Length header, which is what stage 4 saw
         * from example.com. The accumulator's own byte count is the number
         * that matters, and it is the one the parser is given. */
        ESP_LOGI(TAG, "status %d, content-length %" PRId64 "%s, body %d bytes%s",
                 status, clen, (clen < 0) ? " (chunked)" : "", body.len,
                 body.truncated ? " (TRUNCATED at BODY_CAP)" : "");

        if (body.truncated) {
            /* Parsing a truncated document would fail anyway, but with a
             * misleading "invalid JSON" message pointing at the last byte.
             * Say what actually happened instead. */
            ESP_LOGE(TAG, "response exceeded BODY_CAP (%d bytes) -- not parsing "
                          "a partial document; raise BODY_CAP if the contract "
                          "really did grow this much", BODY_CAP);
            show_failure("BAD DATA", "BAD DATA", "TOO LARGE");

        } else if (status == 200) {
            usage_t usage;
            if (parse_usage(body.buf, body.len, &usage)) {
                print_usage(&usage);
                show_usage(&usage);
            } else {
                /* The parse failed, so the raw bytes are the evidence. */
                show_failure("BAD DATA", "BAD DATA", "NOT JSON");
                printf("---8<--- body ---8<---\n%s\n---8<--- end ----8<---\n",
                       body.buf);
            }

        } else if (status == 503) {
            /* A documented, expected response -- not a failure of this
             * firmware. It means pc_service is up but has never completed a
             * poll (it was just started, or Anthropic is rate-limiting it), so
             * there is no last-known data to serve even as stale. This is a
             * "no data" state, not an error state. */
            char reason[96];
            parse_error_reason(body.buf, body.len, reason, sizeof(reason));
            ESP_LOGW(TAG, "pc_service has no data yet (503): %s", reason);
            ESP_LOGW(TAG, "the network path works -- this is the PC's upstream "
                          "fetch failing, not the chip. Give it a poll interval "
                          "and retry, or read pc_service's own log");
            show_failure("NO DATA", "NO DATA", "PC SERVICE");

        } else {
            ESP_LOGW(TAG, "unexpected status %d -- the contract only defines "
                          "200 and 503; body follows", status);
            show_failure("BAD DATA", "BAD DATA", "BAD STATUS");
            printf("---8<--- body ---8<---\n%s\n---8<--- end ----8<---\n",
                   body.buf);
        }
    }

    /* Always, on every path: cleanup frees the socket and the parser state.
     * Skipping it leaks a few KB per request -- which one request survives and
     * a refresh every 45 seconds absolutely would not. */
    esp_http_client_cleanup(client);
}

/* --- stage 8: the refresh loop -------------------------------------------- */

/* The task that owns the display for the rest of the program's life.
 *
 * Every drawing call after boot happens on this one task. That is not an
 * accident of structure, it is the reason gc9a01 needs no mutex: app_main
 * draws the boot sequence, then creates this task and never draws again. Any
 * third caller breaks the guarantee and has to add a lock first.
 *
 * Why a task rather than a loop inside app_main: app_main runs with
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE, 3584 bytes here, and esp_http_client needs
 * appreciably more than that once lwIP and the HTTP parser are on the stack.
 * Overflowing it produces a stack canary panic and a reboot rather than a tidy
 * error. 8 KB is what ESP-IDF's own esp_http_client example allocates, and it
 * is the number to start from rather than tuning downward without a
 * measurement. cJSON adds little: its tree goes on the heap, not this stack.
 *
 * This one never returns, so it never calls vTaskDelete -- unlike stage 7's
 * one-shot version, which had to. */
static void usage_task(void *arg)
{
    /* static, so this 4 KB sits in .bss rather than on the task's stack --
     * which we just went to some trouble to leave room in. */
    static char storage[BODY_CAP];

    /* Printed once, not once a cycle. The URL is the most useful line in this
     * whole log when the chip cannot reach the service: it is assembled from
     * whichever config won (NVS or secrets.h), so seeing it spelled out is how
     * a host address that DHCP has moved out from under you gets caught in
     * seconds rather than after an hour of blaming the firewall. Repeating it
     * every 45 seconds would bury the lines that only appear when something is
     * actually wrong. */
    ESP_LOGI(TAG, "polling %s every %d s", s_usage_url, REFRESH_INTERVAL_MS / 1000);

    while (1) {

        /* The button, first thing, so a press is answered before anything
         * slow happens. usage_task rather than the button task does this,
         * because the button task must not draw: the panel belongs to this
         * task alone once app_main has handed it over.
         *
         * Stage 2 has nothing to enter yet, so it acknowledges and carries on.
         * The acknowledgement is not decoration -- it is what proves the whole
         * chain (pin, debounce, event bit, task wake, display) works, and it
         * is the thing stage 4 replaces with the actual setup screen. */
        if (xEventGroupGetBits(s_events) & BUTTON_SETUP_BIT) {
            xEventGroupClearBits(s_events, BUTTON_SETUP_BIT);
            ESP_LOGI(TAG, "setup requested -- no setup mode to enter yet (stage 2)");
            render_message("BUTTON", "HELD 3S");
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* render_message called forget_drawn(), so whatever is drawn next
             * repaints in full rather than diffing against a screen that is no
             * longer there. Falling through to the normal path from here is
             * deliberate: it puts the real numbers back immediately instead of
             * leaving this message up until the next refresh. */
        }

        /* Ask the radio before spending a ten-second HTTP timeout discovering
         * something it already knows. This also gets the diagnosis right on
         * screen: NO WIFI points at the network, NO LINK points at the PC, and
         * a firmware that showed NO LINK whenever the router rebooted would
         * send you to debug the wrong machine. */
        if (!(xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT)) {

            /* Two different situations that look identical to the radio. At
             * boot the chip has simply not associated yet, which takes up to
             * twenty seconds on this network and is not a fault; after that,
             * a cleared bit means a link that existed and went away. Saying
             * CONNECTING for the first and NO WIFI for the second is the
             * difference between a gadget that looks like it is starting up
             * and one that looks broken during every normal boot. */
            if (s_ever_connected) {
                show_failure("NO WIFI", "NO WIFI", "RECONNECTING");
            } else {
                render_message("CONNECTING", NULL);
            }

            /* Block until the radio is back, or until the refresh interval is
             * up -- whichever happens first. Waiting on the bit rather than
             * sleeping on a timer means a reconnect is picked up the moment it
             * happens, so the screen recovers as fast as the radio does
             * instead of up to 45 seconds later. The reconnect attempts
             * themselves are the disconnect handler's job and are already
             * running; this only waits for them to succeed. */
            /* pdFALSE for xWaitForAllBits: wake on EITHER the radio coming
             * back or the button being held, whichever happens first. Bits are
             * not cleared on exit -- the loop's top clears the button bit
             * itself, after it has decided what to do about it. */
            xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | BUTTON_SETUP_BIT,
                                pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(REFRESH_INTERVAL_MS));
            continue;
        }

        fetch_once(storage, sizeof(storage));

        /* Free heap on every cycle, because this is the first code in the
         * project that runs forever: a leak of even a few hundred bytes per
         * fetch is invisible in one request and fatal within a day, and this
         * number is what makes it visible. It should be flat. */
        ESP_LOGI(TAG, "free heap %" PRIu32 " bytes; next refresh in %d s",
                 esp_get_free_heap_size(), REFRESH_INTERVAL_MS / 1000);

        /* Was a plain vTaskDelay. Waiting on the bit instead is what makes
         * the button feel like it works: a press during the 45-second gap is
         * acted on immediately rather than whenever the timer happens to
         * expire. With no press this behaves exactly as the sleep did. */
        xEventGroupWaitBits(s_events, BUTTON_SETUP_BIT,
                            pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(REFRESH_INTERVAL_MS));
    }
}

/* --- setup -------------------------------------------------------------- */

/* Brings the radio up: NVS, the TCP/IP stack, the event loop, then the WiFi
 * driver itself, in that order because each depends on the one before.
 *
 * This only *starts* the process. It returns as soon as the driver is running,
 * long before there is a connection -- association and the DHCP lease arrive
 * later, on the event handlers above. Nothing here blocks waiting for a
 * network. */
static void wifi_start(const app_config_t *cfg)
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

    /* This used to be two _Static_asserts. They cannot survive the move to a
     * runtime config -- the compiler no longer knows what the strings are --
     * so the same rule is enforced here instead, and the reasoning is worth
     * keeping either way.
     *
     * sta.ssid is uint8_t[32] and sta.password uint8_t[64]. The 802.11 SSID
     * field is a length-counted array, not a C string, so a full 32 characters
     * is legal and needs all 32 bytes; copying only 31 would quietly drop the
     * last character of a maximum-length SSID, which surfaces as an endless
     * "disconnected (reason 201)" loop -- exactly the symptom the README tells
     * you to blame on a typo or a 5 GHz network. Same shape for a
     * 64-hex-character raw WPA2 PSK.
     *
     * By construction these clamps cannot fire: config.h sizes its buffers at
     * 32+1 and 64+1, so strlen can never exceed the destination. They are here
     * because "cannot happen" is a property of today's config.h rather than of
     * this function, and a silent buffer overrun is a bad way to discover that
     * somebody changed one number. Truncating loudly is the safe failure. */
    size_t ssid_len = strlen(cfg->ssid);
    size_t pass_len = strlen(cfg->password);

    if (ssid_len > sizeof(wifi_cfg.sta.ssid)) {
        ESP_LOGE(TAG, "ssid is %u bytes, truncating to %u -- this will not connect",
                 (unsigned)ssid_len, (unsigned)sizeof(wifi_cfg.sta.ssid));
        ssid_len = sizeof(wifi_cfg.sta.ssid);
    }
    if (pass_len > sizeof(wifi_cfg.sta.password)) {
        ESP_LOGE(TAG, "password is %u bytes, truncating to %u -- this will not connect",
                 (unsigned)pass_len, (unsigned)sizeof(wifi_cfg.sta.password));
        pass_len = sizeof(wifi_cfg.sta.password);
    }

    memcpy(wifi_cfg.sta.ssid,     cfg->ssid,     ssid_len);
    memcpy(wifi_cfg.sta.password, cfg->password, pass_len);

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

    /* --- the runtime configuration, before anything that could use it -----
     *
     * After nvs_flash_init, because config_load reads NVS; before the display
     * and the radio, because both depend on what it finds. Nothing here draws
     * or transmits -- it only decides what this run is going to try to do, and
     * says so in the log before trying it.
     *
     * NOTE the interaction with the erase-and-retry just above: if NVS was
     * full or written by a different IDF version, it has now been WIPED, and a
     * provisioned config went with it. That is correct -- an unreadable NVS
     * cannot be trusted to hold credentials either -- but it means the chip
     * falls back to secrets.h, and once the setup portal exists it will mean
     * the chip comes up asking to be configured again. */
    config_load(&s_cfg);

    /* Assembled once, here, because both the fetch task and its log lines want
     * it and neither should rebuild it. See the comment above s_usage_url for
     * what this replaced. */
    snprintf(s_usage_url, sizeof(s_usage_url), "http://%s:%u/usage",
             s_cfg.host, (unsigned)s_cfg.port);

    if (!config_is_complete(&s_cfg)) {
        /* No SSID, or nowhere to poll. There is nothing this stage can do
         * about that -- the setup portal is what will fix it -- but saying so
         * precisely beats letting it present as an endless "reason 201" WiFi
         * failure, which sends you off to debug the wrong thing entirely. */
        ESP_LOGE(TAG, "config incomplete: ssid, host and port must all be set");
        ESP_LOGE(TAG, "fill in main/secrets.h and reflash, or provision over nvs");
    }

    /* --- stage 6: the display, before anything network-shaped -------------
     *
     * Deliberately first. The panel does not depend on WiFi, and doing it here
     * means two things: the screen shows something within a third of a second
     * of power-on rather than after a 20-second WiFi join, and a dark screen
     * cannot be blamed on the network because the network has not started yet.
     *
     * The colour cycle is the actual test. A single fill would leave a screen
     * that might simply be *stuck* on one colour from a previous run; three
     * colours in sequence prove the chip is genuinely driving the panel, and
     * that red, green and blue come out as red, green and blue (which is what
     * confirms the BGR bit of madctl -- and only that bit: a solid fill is
     * symmetric, so it cannot say anything about orientation. That blind spot
     * is why the mirroring went unnoticed until stage 7 drew text).
     *
     * It then rests on blue rather than black, deliberately. Black would be
     * the natural background for stage 7 -- but at this stage a black screen
     * and a dead panel look exactly alike, so the resting state would prove
     * nothing to anyone who missed the two-second cycle. Ending on a lit
     * colour means the screen is still answering the question minutes later.
     * Stage 7 clears to black as its first act. */
    ESP_LOGI(TAG, "stage 6: display bring-up");
    gc9a01_init();

    static const struct { const char *name; uint16_t color; } probe[] = {
        { "RED",   GC9A01_RED   },
        { "GREEN", GC9A01_GREEN },
        { "BLUE",  GC9A01_BLUE  },
    };
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        ESP_LOGI(TAG, "  fill: %s", probe[i].name);
        gc9a01_fill_screen(probe[i].color);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Stage 6 rested on solid blue here, because at that stage a black screen
     * and a dead panel looked identical and the resting state had to prove the
     * panel was alive. Stage 7 has something truthful to say instead, and
     * saying it is better: the WiFi join below can take twenty seconds, and a
     * screen that sat blank or blue for that long would look broken. The
     * colour cycle just above still does the panel-is-alive job. */
    /* CONNECTING is a promise that something is about to happen. With no
     * usable config nothing is, so say that instead -- the brief's "degrade
     * visibly, not silently" applies just as much to a chip that was never
     * told where to go as to one that lost its network. */
    if (config_is_complete(&s_cfg)) {
        render_message("CONNECTING", NULL);
    } else {
        render_message("NO CONFIG", "SEE SERIAL");
    }
    ESP_LOGI(TAG, "display ready");

    /* Must exist before wifi_start(), because the handlers it registers can
     * fire -- and touch this group -- the instant the radio starts. */
    s_events = xEventGroupCreate();

    ESP_LOGI(TAG, "stage 8: wifi, then refresh every %d s forever",
             REFRESH_INTERVAL_MS / 1000);
    wifi_start(&s_cfg);

    /* Stage 7 blocked here until WIFI_CONNECTED_BIT was set, and only then
     * created the fetch task. Stage 8 does not, and the change matters: the
     * display task is now the thing that reports on WiFi, so it has to be
     * running *before* the radio is up in order to say so. Blocking here would
     * leave the screen frozen on whatever app_main drew last for the whole
     * join -- which is the exact failure mode the brief asks to avoid.
     *
     * The task handles a missing connection itself, and waits on the same bit
     * from there.
     *
     * This is also the moment the drawing guarantee is handed over. app_main
     * has drawn its last pixel above; from the next line on, the display
     * belongs to usage_task alone and nothing else may touch gc9a01.
     *
     * The five arguments to xTaskCreate are the function, a name for it (it
     * shows up in crash dumps and task lists), the stack size in BYTES, the
     * argument passed to the function, and the priority. 5 is above the idle
     * task and below the WiFi driver's -- the same value ESP-IDF's own HTTP
     * example uses. The sixth parameter would receive a handle for later
     * control; nothing here needs one, since the task runs for the life of the
     * program. */
    xTaskCreate(&usage_task, "usage", 8192, NULL, 5, NULL);

    /* After usage_task, so the log reads in the order things become true --
     * though the ordering does not actually matter: a press that lands before
     * usage_task is scheduled just leaves the bit set, and the first pass of
     * the loop picks it up. */
    button_start(s_events, BUTTON_SETUP_BIT);

    /* Heartbeat, so a silent serial monitor means "the chip crashed or reset"
     * rather than leaving you guessing whether it is merely idle. It is also
     * the only thing app_main does from here on: it deliberately draws
     * nothing, so the single-task display guarantee holds.
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
