/* The runtime configuration: reading it, and -- since provisioning stage 4 --
 * writing it. See config.h for what and why.
 *
 * The read half came first and on its own, deliberately: stage 1 changed only
 * where the values came from and not who could set them, so that "the gadget
 * behaves exactly as before" was a meaningful test result. The write half is
 * one function at the bottom, and it is the only code in the project that can
 * destroy a working configuration -- which is why config.h spends most of its
 * words on it.
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
 * There are TWO layers to get past, which is why the obvious single remedies
 * all fail -- each was tried here and did not work:
 *
 *   - ninja decides whether to invoke the compiler at all. It consults the .d
 *     dependency file, which does not list secrets.h, so it considers the
 *     object up to date and runs nothing. CCACHE_RECACHE alone therefore does
 *     nothing: ccache is never even called.
 *   - ccache decides what that invocation returns. So `idf.py fullclean`
 *     alone does not work either: ninja rebuilds, calls ccache, and ccache
 *     hands back the same stale object.
 *
 * Both have to be defeated together:
 *
 *     idf.py fullclean
 *     CCACHE_RECACHE=1 idf.py build
 *
 * RECACHE rather than --no-ccache on purpose: --no-ccache bypasses the cache
 * for that one build and leaves the poisoned entry in place to be served
 * again later, while RECACHE replaces it. After this, ordinary builds are
 * correct.
 *
 * This is a property of conditional includes in general, not of this file. */
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

/* The fingerprint of the credentials that are known to work. See config.h. */
#define KEY_PROVEN     "proven"

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

    /* NVS_READONLY even now that config_save exists, and for a better reason
     * than "this function does not write": opening a namespace read-only that
     * has never been created returns ESP_ERR_NVS_NOT_FOUND rather than
     * creating it, so reading a chip's defaults does not leave an empty
     * namespace behind.
     *
     * Do NOT read the "no 'cfg' namespace yet" line below as proof that
     * nothing was ever provisioned, though -- it stopped meaning that when
     * config_mark_proven arrived. That function opens the same namespace
     * read-write, so the first time a secrets.h-only chip connects, the
     * namespace comes into existence with nothing in it but the proven
     * fingerprint, and this line never appears again. The per-key "(from
     * secrets.h)" lines further down are the diagnostic to trust: they are
     * more specific, and they stay true. */
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

/* --- the write half (provisioning stage 4) ------------------------------- */

esp_err_t config_save(const app_config_t *cfg, bool save_password)
{
    /* NVS_READWRITE, which unlike the read path DOES create the namespace if
     * it is not there -- which is the case on every chip that has never been
     * provisioned, i.e. the interesting one. */
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for writing failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Each set is attempted only if everything before it worked, so the first
     * error is the one reported and nothing is written on top of a failure.
     * Note that none of these is durable until the commit below. */
    err = nvs_set_str(h, KEY_SSID, cfg->ssid);

    if (err == ESP_OK && save_password) {
        err = nvs_set_str(h, KEY_PASS, cfg->password);
    }
    /* When save_password is false the key is simply not touched. Whatever it
     * held stays: a password saved on an earlier visit, or nothing at all, in
     * which case config_load falls back to secrets.h for that one value. Both
     * are "keep the current password", which is what a blank field means. */

    if (err == ESP_OK) { err = nvs_set_str(h, KEY_HOST, cfg->host); }
    if (err == ESP_OK) { err = nvs_set_u16(h, KEY_PORT, cfg->port); }

    if (err == ESP_OK) {
        /* The moment it becomes real. nvs_set_* only stages a value; this is
         * what guarantees it survives a power cut. One commit for all four
         * means there is no window in which the SSID has changed and the host
         * has not. */
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the config failed: %s -- nothing was changed",
                 esp_err_to_name(err));
        return err;
    }

    /* Same rule as everywhere else: the password's presence, never its value.
     * Saying which of the two branches ran is worth a line, because "I changed
     * my WiFi password and it still uses the old one" is otherwise a mystery
     * with no evidence either way. */
    ESP_LOGI(TAG, "config saved to nvs:");
    ESP_LOGI(TAG, "  ssid     \"%s\"", cfg->ssid);
    ESP_LOGI(TAG, "  password %s", save_password
                                     ? "replaced"
                                     : "left as it was (field was blank)");
    ESP_LOGI(TAG, "  host     \"%s\"", cfg->host);
    ESP_LOGI(TAG, "  port     %u", (unsigned)cfg->port);
    return ESP_OK;
}

/* FNV-1a over the SSID and password, with a separator between them so that
 * ("ab", "c") and ("a", "bc") do not collide by construction.
 *
 * A hash rather than the values themselves because this is a comparison, not a
 * lookup: nothing ever needs to read it back. It is NOT a security measure and
 * must not be mistaken for one -- NVS already holds the password in plain text
 * a few keys away, so this hides nothing that is not already there.
 *
 * A collision would mean trusting credentials that had not actually been
 * proven, which degrades to exactly the behaviour this project had before the
 * flag existed: the chip waits ten minutes before offering setup mode. At 32
 * bits over two short strings that is not worth defending against further. */
static uint32_t wifi_fingerprint(const app_config_t *cfg)
{
    uint32_t h = 2166136261u;                 /* FNV offset basis */

    for (const char *p = cfg->ssid; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;    /* FNV prime */
    }
    h = (h ^ 0xFFu) * 16777619u;              /* the separator */
    for (const char *p = cfg->password; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

bool config_is_proven(const app_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;                          /* never provisioned: unproven */
    }

    uint32_t  stored = 0;
    esp_err_t err    = nvs_get_u32(h, KEY_PROVEN, &stored);
    nvs_close(h);

    if (err != ESP_OK) {
        return false;
    }

    /* The comparison, not merely the presence of the key. A stored value from
     * different credentials is not an answer about these ones. */
    const bool proven = (stored == wifi_fingerprint(cfg));
    ESP_LOGI(TAG, "wifi credentials %s",
             proven ? "have connected before"
                    : "are new or changed -- not yet proven");
    return proven;
}

esp_err_t config_mark_proven(const app_config_t *cfg)
{
    /* READWRITE creates the namespace if it is not there, which on a
     * secrets.h-only chip it will not be. That is intended -- there is nowhere
     * else to put this -- but it does mean the namespace can exist without any
     * configuration having been saved. config_load's comment says so. */
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(h, KEY_PROVEN, wifi_fingerprint(cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "these wifi credentials are now recorded as working");
    }
    return err;
}

bool config_is_complete(const app_config_t *cfg)
{
    /* Note what is NOT checked: the password. An empty one is a legitimate
     * open network, and refusing to start on it would be wrong. */
    return cfg->ssid[0] != '\0' && cfg->host[0] != '\0' && cfg->port != 0;
}
