/* Runtime configuration: the four values that used to be #defines.
 *
 * Until now WiFi credentials and the pc_service address were compiled into
 * the binary from secrets.h, which means changing your WiFi -- or your router
 * handing the PC a different address -- required an editor, a toolchain and a
 * USB cable. This module moves those values to NVS (the chip's small key-value
 * store in flash) so they can be changed at runtime instead, and since
 * provisioning stage 4 the setup form writes them there.
 *
 * secrets.h does not go away. If it exists it supplies the DEFAULTS, used for
 * any value NVS does not have. That keeps every existing workflow working
 * unchanged, and keeps a compiled-in fallback as the escape hatch if a
 * provisioned config is ever wrong.
 *
 * Note the CFG_ prefix rather than CONFIG_: CONFIG_ is ESP-IDF's Kconfig
 * namespace (every symbol in sdkconfig starts with it), and colliding with it
 * would be a genuinely confusing bug to chase.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"                 /* esp_err_t, returned by config_save */

/* Sizes come straight from the 802.11 standard and are +1 for the NUL.
 *
 * The SSID field on the air is a length-counted array, not a C string, so a
 * full 32 characters is legal and needs all 32 bytes -- storing it as a C
 * string here is what forces the +1. Getting this wrong by one is not
 * theoretical: token_monitor.c's wifi_start carries a long comment about a
 * dropped final character presenting as an endless "reason 201" loop, i.e.
 * exactly the symptom the README tells you to blame on a typo. */
#define CFG_SSID_CAP  33   /* 32 characters + NUL */
#define CFG_PASS_CAP  65   /* 64 characters + NUL (a raw PSK is 64 hex chars) */
#define CFG_HOST_CAP  64   /* an IPv4 literal or a short hostname            */

typedef struct {
    char     ssid[CFG_SSID_CAP];
    char     password[CFG_PASS_CAP];
    char     host[CFG_HOST_CAP];
    uint16_t port;
} app_config_t;

/* Fill `out` from NVS, falling back to secrets.h per value.
 *
 * Never fails: a missing namespace, a missing key or a corrupt entry all just
 * mean "use the default", and an absent default just means empty. Callers get
 * a fully-initialised struct in every case and decide for themselves whether
 * it is usable -- see config_is_complete.
 *
 * nvs_flash_init() must have been called first.
 *
 * Logs where each value came from. It logs the SSID, the host and the port,
 * because those are what you need to diagnose a chip talking to the wrong
 * place; it logs the password's LENGTH but never the password.
 */
void config_load(app_config_t *out);

/* True if there is enough here to attempt normal operation: an SSID, a host
 * and a non-zero port. A blank password is deliberately allowed -- that is a
 * legitimate open network, not a missing value.
 *
 * A false return sends the chip into setup mode: there is nothing else it
 * could usefully do, and asking to be configured is the only way out. */
bool config_is_complete(const app_config_t *cfg);

/* Write the configuration to NVS, where the next boot will find it.
 *
 * `save_password` is the whole reason this takes a flag rather than just a
 * struct. The setup form's password field is deliberately blank, meaning
 * "keep whatever is already stored", and the difference between that and "set
 * the password to nothing" cannot be expressed by an empty string -- an empty
 * password is *also* a legitimate value, for an open network. So the caller
 * says which it meant. False leaves the stored key untouched, whatever it
 * held, including nothing at all.
 *
 * All four values go in under a single nvs_commit(), which is the point at
 * which any of them become durable. Lose power before it and the old
 * configuration is intact; lose power after it and the new one is. There is no
 * in-between state where half the settings changed -- which matters because
 * this is the one operation in the project that can destroy a working config.
 *
 * Even so the per-key fallback in config_load() is still the safety net worth
 * having: should a key ever end up missing or unreadable, that value falls
 * back to secrets.h on its own rather than taking the other three down with
 * it.
 *
 * Returns ESP_OK, or the first NVS error encountered. On failure the caller
 * should assume nothing was written and say so -- silently continuing would
 * leave someone believing they had reconfigured a chip that they had not.
 *
 * nvs_flash_init() must have been called first.
 */
esp_err_t config_save(const app_config_t *cfg, bool save_password);

/* Has THIS set of WiFi credentials ever successfully joined a network?
 *
 * The point of the question is to tell a mistake apart from an outage. A
 * password that has never worked is probably wrong, and the gadget should stop
 * waiting and ask to be fixed. A password that worked yesterday is still
 * right, and a network that is missing today is the router's problem -- the
 * gadget should keep retrying forever, as it did before setup mode existed.
 * Without this the two are indistinguishable after a reboot, because nothing
 * in RAM survives one.
 *
 * "This set" is the load-bearing part, and it is why these take a config
 * rather than being a bare flag. What is stored is a fingerprint of the SSID
 * and password, so the answer is false again the moment either changes --
 * whether they changed through the setup form or through a rebuilt secrets.h.
 * A plain yes/no flag would have kept saying "proven" about credentials it had
 * never seen, which is exactly the case that most needs to recover.
 *
 * The host and port are deliberately NOT part of the fingerprint. They have no
 * bearing on joining a network, so correcting the PC's address should not throw
 * away the knowledge that the WiFi credentials are good.
 */
bool config_is_proven(const app_config_t *cfg);

/* Record that these credentials just worked. Call it once per boot, after an
 * IP arrives -- an association alone is not proof, since a chip can associate
 * and still never get a lease.
 *
 * Failures are the caller's to ignore: not being able to write the flag costs
 * a little patience on some future boot, and nothing else. */
esp_err_t config_mark_proven(const app_config_t *cfg);
