/* Reading the runtime configuration. See config.h for what and why.
 *
 * The whole file is the read half. There is deliberately no write half yet:
 * this stage changes only where the values come from, not who can set them,
 * so that "the gadget behaves exactly as before" is a meaningful test result.
 */

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "esp_log.h"

#include "config.h"

/* secrets.h is now OPTIONAL.
 *
 * It used to be a hard #error in token_monitor.c, because without it the
 * firmware had no possible way to know a network. That is no longer true --
 * NVS can supply everything -- and making it optional is what will let someone
 * clone this repo and flash it without editing a file first.
 *
 * __has_include is a compiler feature test. The outer #if defined() guard is
 * because it is not universally available, though GCC (what ESP-IDF uses) has
 * had it for years; where it is missing we include unconditionally, which is
 * the old behaviour.
 *
 * TRAP, hit while building this and worth knowing about. ESP-IDF runs the
 * compiler through ccache, and ccache in direct mode keys a cached object on
 * the source file plus the list of headers the PREVIOUS compile actually
 * opened. Build once with no secrets.h and that list does not mention it --
 * so creating secrets.h afterwards changes nothing ccache looks at, and you
 * silently get the old object back. `idf.py fullclean` does not help either,
 * because the cache lives outside the build directory. The symptom is a chip
 * that ignores a secrets.h you can see perfectly well on disk.
 *
 * The fix is one build that refreshes the cache entry:
 *
 *     CCACHE_RECACHE=1 idf.py build      (or: idf.py --no-ccache build)
 *
 * after which ordinary builds are correct again. This is a property of
 * conditional includes in general, not of this file. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#else
#  include "secrets.h"
#endif

/* Whatever secrets.h did not define, define as empty. An empty default is a
 * perfectly good answer -- it just means NVS had better have the value. */
#ifndef WIFI_SSID
#  define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#  define WIFI_PASSWORD ""
#endif
#ifndef PC_SERVICE_HOST
#  define PC_SERVICE_HOST ""
#endif
#ifndef PC_SERVICE_PORT
#  define PC_SERVICE_PORT 0
#endif

static const char *TAG = "config";

/* One namespace holding all four keys. NVS keys are limited to 15 characters,
 * which is not close to binding here, but is why they are abbreviated rather
 * than spelled out. */
#define NVS_NAMESPACE  "cfg"
#define KEY_SSID       "ssid"
#define KEY_PASS       "pass"
#define KEY_HOST       "host"
#define KEY_PORT       "port"

/* Read one string key, and only overwrite `out` if the read fully succeeded.
 *
 * The temporary buffer is the point of this function. nvs_get_str leaves the
 * caller's buffer in an unspecified state when it fails -- notably on
 * ESP_ERR_NVS_INVALID_LENGTH, where the stored value is longer than the space
 * offered. Reading straight into `out` would therefore let a bad NVS entry
 * destroy a perfectly good secrets.h default, turning a recoverable situation
 * into a chip that cannot connect to anything. So: read into scratch, and copy
 * across only on success.
 *
 * Returns true if `out` now holds a value from NVS. */
static bool load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    char   scratch[CFG_PASS_CAP];   /* the largest of the three capacities */
    size_t len = cap;

    if (cap > sizeof(scratch)) {
        /* Cannot happen with today's capacities, and is a programming error
         * rather than a runtime condition if it ever does. Say so and keep the
         * default rather than overflowing scratch. */
        ESP_LOGE(TAG, "%s: capacity %u exceeds scratch buffer", key, (unsigned)cap);
        return false;
    }

    esp_err_t err = nvs_get_str(h, key, scratch, &len);
    if (err == ESP_OK) {
        memcpy(out, scratch, len);
        return true;
    }

    /* Not finding a key is the normal case on a chip that has never been
     * provisioned, and deserves no warning. Anything else is worth saying out
     * loud, because it means NVS holds something we could not use. */
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "%s: unreadable in nvs (%s), keeping default",
                 key, esp_err_to_name(err));
    }
    return false;
}

/* Same shape for the port, which is a number rather than a string. */
static bool load_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    uint16_t  v   = 0;
    esp_err_t err = nvs_get_u16(h, key, &v);

    if (err == ESP_OK) {
        *out = v;
        return true;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "%s: unreadable in nvs (%s), keeping default",
                 key, esp_err_to_name(err));
    }
    return false;
}

/* For the log lines below: which of the two sources a value ended up from. */
static const char *source(bool from_nvs)
{
    return from_nvs ? "nvs" : "secrets.h";
}

void config_load(app_config_t *out)
{
    /* Start from the compiled-in defaults. snprintf rather than strcpy so an
     * over-long value in secrets.h truncates safely instead of overrunning --
     * the build used to catch that with _Static_assert, which is no longer
     * possible now that the same fields can also be filled at runtime. */
    memset(out, 0, sizeof(*out));
    snprintf(out->ssid,     sizeof(out->ssid),     "%s", WIFI_SSID);
    snprintf(out->password, sizeof(out->password), "%s", WIFI_PASSWORD);
    snprintf(out->host,     sizeof(out->host),     "%s", PC_SERVICE_HOST);
    out->port = PC_SERVICE_PORT;

    bool n_ssid = false, n_pass = false, n_host = false, n_port = false;

    /* NVS_READONLY, because this stage genuinely never writes. Opening a
     * namespace read-only that has never been created returns
     * ESP_ERR_NVS_NOT_FOUND rather than creating it, which is exactly the
     * behaviour we want on a chip that has only ever been flashed. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        n_ssid = load_str(h, KEY_SSID, out->ssid,     sizeof(out->ssid));
        n_pass = load_str(h, KEY_PASS, out->password, sizeof(out->password));
        n_host = load_str(h, KEY_HOST, out->host,     sizeof(out->host));
        n_port = load_u16(h, KEY_PORT, &out->port);
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no '%s' namespace in nvs yet -- using secrets.h for everything",
                 NVS_NAMESPACE);
    } else {
        ESP_LOGW(TAG, "nvs_open failed (%s) -- using secrets.h for everything",
                 esp_err_to_name(err));
    }

    /* Per-key fallback, not all-or-nothing, and the log says so per key.
     *
     * Two reasons for that choice. A power cut part-way through a future save
     * leaves a chip that still works on its old values rather than a brick.
     * And it makes this stage testable on its own: an NVS image containing
     * only host and port proves the read path, while the SSID visibly still
     * comes from secrets.h. */
    ESP_LOGI(TAG, "config:");
    ESP_LOGI(TAG, "  ssid     \"%s\" (from %s)", out->ssid, source(n_ssid));
    /* The password itself is never logged, here or anywhere. Its length is,
     * because a value one character short is the failure this project has
     * already documented once, and "10 chars" makes that visible while giving
     * away nothing worth having. */
    ESP_LOGI(TAG, "  password %u chars (from %s)",
             (unsigned)strlen(out->password), source(n_pass));
    ESP_LOGI(TAG, "  host     \"%s\" (from %s)", out->host, source(n_host));
    ESP_LOGI(TAG, "  port     %u (from %s)", (unsigned)out->port, source(n_port));
}

bool config_is_complete(const app_config_t *cfg)
{
    /* Note what is NOT checked: the password. An empty one is a legitimate
     * open network, and refusing to start on it would be wrong. */
    return cfg->ssid[0] != '\0' && cfg->host[0] != '\0' && cfg->port != 0;
}
