/* Runtime configuration: the four values that used to be #defines.
 *
 * Until now WiFi credentials and the pc_service address were compiled into
 * the binary from secrets.h, which means changing your WiFi -- or your router
 * handing the PC a different address -- required an editor, a toolchain and a
 * USB cable. This module moves those values to NVS (the chip's small key-value
 * store in flash) so they can be changed at runtime instead. Nothing writes to
 * NVS yet; that arrives with the setup portal in a later stage.
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
 * A false return is what will trigger setup mode once the portal exists. */
bool config_is_complete(const app_config_t *cfg);
