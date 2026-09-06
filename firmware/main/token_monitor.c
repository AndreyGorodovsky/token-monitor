/* Token monitor -- firmware stage 5: fetch the real usage JSON and parse it.
 *
 * Stage 3 (still here, unchanged) joins the WiFi network and prints the IP
 * address the router hands us. Stage 4 proved esp_http_client works on this
 * chip by GETting a throwaway URL and printing the raw bytes. Stage 5 points
 * that same, barely-changed machinery at the real pc_service on the LAN and
 * turns the reply into numbers: cJSON parses the body against the contract in
 * ARCHITECTURE.md, and the parsed values are printed over serial. Still no
 * display -- that is stage 6 (panel bring-up) and stage 7 (the two combined).
 *
 * What stage 5 is actually testing is the *contract*, not the plumbing. The
 * plumbing was stage 4's job. The new questions here are: can the chip reach
 * the PC at all (a different problem from reaching the internet -- it depends
 * on the PC's firewall and its DHCP address, not on DNS or a route out), and
 * do the field names and types this file expects match the ones service.py
 * actually sends?
 *
 * Success looks like: a 200, then a "parsed:" block listing both percentages,
 * both reset strings and the freshness line. pc_service must be RUNNING on
 * the PC named by PC_SERVICE_HOST in secrets.h -- stage 4 needed nothing on
 * the PC, so this is a new prerequisite and the most likely reason for a
 * "request failed: ESP_ERR_HTTP_CONNECT" on the first run.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED, top to bottom:
 *
 *   1. the secrets.h guard      -- fail the build early with a clear message
 *   2. includes                 -- and what each one is actually for
 *   3. module state             -- the handful of file-scope variables
 *   4. WiFi event handlers      -- code the WiFi driver calls back into
 *   5. the usage fetch          -- stage 5: URL, accumulator, JSON, its task
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

#include "gc9a01.h"                  /* stage 6: the round display driver,
                                      * hand-rolled on spi_master + gpio    */

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

/* --- stage 5: fetching and parsing the usage JSON ------------------------ */

/* The URL is assembled from secrets.h rather than written out here, because
 * the host part is a real LAN address -- machine-specific, and this file is
 * committed to a public repo. secrets.h is gitignored; see SECRETS.md.
 *
 * The two-step STRINGIFY is a standard C preprocessor idiom, and it is worth
 * understanding rather than copying. The `#` operator turns a macro argument
 * into a string literal, but it does so *before* that argument is itself
 * expanded. So a one-step version of this would produce the literal text
 * "PC_SERVICE_PORT" instead of "8734". Passing it through an outer macro
 * first forces the expansion to happen, and only the inner macro stringifies.
 *
 * Adjacent string literals are concatenated by the compiler, so the result is
 * a single compile-time constant -- no sprintf, no buffer, nothing to get
 * wrong at runtime. Note pc_service serves plain HTTP: no TLS on the LAN, by
 * design (ARCHITECTURE.md's "trust boundary is the home network"), which is
 * also why stage 4 deliberately tested without it. */
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
#define USAGE_URL "http://" PC_SERVICE_HOST ":" STRINGIFY(PC_SERVICE_PORT) "/usage"

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
         * flashing a stale badge over a typo'd field name. Stage 8 is where
         * that age becomes a decision rather than a printed number. */
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
     * chip needing to know what time it is. This is the number stage 8 will
     * use to decide when data is too old to show at all. */
    if (u->updated_epoch > 0 && u->now_epoch > 0) {
        printf("  data age: %" PRId64 "s by the PC's clock\n",
               u->now_epoch - u->updated_epoch);
    }
    printf("---8<--- end ------8<---\n");
}

/* --- the request itself --------------------------------------------------- */

/* Performs the one request, prints what came back, and deletes itself.
 *
 * Still one shot, not a loop: the periodic refresh is stage 8's job, and
 * keeping this single-shot means a failure here is one request to reason
 * about rather than a scrolling log. Power-cycle to run it again.
 *
 * Why this is a task rather than a few lines inside app_main: app_main runs
 * on a task whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE, which is 3584
 * bytes here. esp_http_client needs appreciably more than that once lwIP and
 * the HTTP parser are on the stack, and overflowing it produces a stack
 * canary panic and a reboot rather than a tidy error. 8 KB is what ESP-IDF's
 * own esp_http_client example allocates, and it is the number to start from
 * rather than tuning downward without a measurement. cJSON adds little to
 * that: its tree goes on the heap, not on this stack.
 *
 * A task that has finished its work must delete itself; falling off the end
 * of a task function without calling vTaskDelete(NULL) crashes the system. */
static void usage_fetch_task(void *arg)
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
        .url           = USAGE_URL,
        .method        = HTTP_METHOD_GET,
        .event_handler = on_http_event,
        .user_data     = &body,
        .timeout_ms    = 10000,   /* 10s: generous for a LAN, and short
                                   * enough that a black-holed connection
                                   * fails visibly instead of hanging      */
    };

    ESP_LOGI(TAG, "stage 5: GET %s", USAGE_URL);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed (out of memory?)");
        vTaskDelete(NULL);
        return;                  /* unreachable; states the intent clearly */
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
     * having if this polled every few seconds, but stage 8 polls once a
     * minute -- far longer than any server holds an idle connection open, so
     * the connection would be dead before the next request anyway. */
    esp_http_client_set_header(client, "Connection", "close");

    /* perform() blocks until the whole exchange finishes: connect, request,
     * response, and every on_http_event call above. Note this is NOT wrapped
     * in ESP_ERROR_CHECK -- a failed network request is a normal condition to
     * report, not a reason to abort the program. That rule is the whole reason
     * stage 8 can have a "service unreachable" screen. */
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
        /* Stage 4 already proved this chip can resolve DNS and reach the
         * internet, so a failure here is almost certainly on the PC side or in
         * secrets.h -- worth saying, because the instinct is to blame WiFi. */
        ESP_LOGE(TAG, "stage 4 reached the internet from this chip, so suspect: "
                      "pc_service not running; the PC firewall blocking this "
                      "port; or PC_SERVICE_HOST in secrets.h pointing at an "
                      "address DHCP has since handed to something else");
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

        } else if (status == 200) {
            usage_t usage;
            if (parse_usage(body.buf, body.len, &usage)) {
                print_usage(&usage);
                ESP_LOGI(TAG, "stage 5 passed: the JSON contract works end to end");
            } else {
                /* The parse failed, so the raw bytes are the evidence. */
                printf("---8<--- body ---8<---\n%s\n---8<--- end ----8<---\n",
                       body.buf);
            }

        } else if (status == 503) {
            /* A documented, expected response -- not a failure of this
             * firmware. It means pc_service is up but has never completed a
             * poll (it was just started, or Anthropic is rate-limiting it), so
             * there is no last-known data to serve even as stale. From stage 7
             * on this is a "no data" screen, not an error screen. */
            char reason[96];
            parse_error_reason(body.buf, body.len, reason, sizeof(reason));
            ESP_LOGW(TAG, "pc_service has no data yet (503): %s", reason);
            ESP_LOGW(TAG, "the network path works -- this is the PC's upstream "
                          "fetch failing, not the chip. Give it a poll interval "
                          "and retry, or read pc_service's own log");

        } else {
            ESP_LOGW(TAG, "unexpected status %d -- the contract only defines "
                          "200 and 503; body follows", status);
            printf("---8<--- body ---8<---\n%s\n---8<--- end ----8<---\n",
                   body.buf);
        }
    }

    /* Always, on every path: cleanup frees the socket and the parser state.
     * Skipping it leaks a few KB per request, which one request survives and
     * stage 8's once-a-minute loop would not. */
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "stage 5 complete. heartbeat continues; power-cycle to re-run.");
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
     * confirms madctl = 0x08 is right for this board).
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
    gc9a01_fill_screen(GC9A01_BLUE);
    ESP_LOGI(TAG, "display ready (screen should now be solid BLUE and stay that way)");

    /* Must exist before wifi_start(), because the handlers it registers can
     * fire -- and touch this group -- the instant the radio starts. */
    s_wifi_events = xEventGroupCreate();

    ESP_LOGI(TAG, "stage 5: wifi + one fetch of the real usage JSON");
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
    xTaskCreate(&usage_fetch_task, "usage_fetch", 8192, NULL, 5, NULL);

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
