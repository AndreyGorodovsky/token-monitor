/* Setup mode: the chip becomes its own WiFi hotspot serving a settings form.
 *
 * The problem this solves: changing WiFi networks, or the PC getting a
 * different address, used to mean editing secrets.h and rebuilding and
 * reflashing. A three-second press on the D1 button now puts the gadget into
 * its own network, with a form you fill in from a phone.
 *
 * A submission that passes validation is written to NVS and applied by
 * rebooting -- there is no attempt to reconfigure a running radio, because a
 * reboot is the one path already known to produce a correctly configured chip.
 * What is NOT done is checking the credentials before keeping them:
 * verify-before-commit is stage 5, and it cannot be done from inside the
 * request that would have to be answered afterwards.
 *
 * Stage 3 built this without the save, on purpose -- a form that does not
 * render and a save that does not stick are two different bugs, and finding
 * them one at a time was much faster than finding them together.
 *
 * WHAT THE CALLER MUST DO FIRST. Becoming an AP means leaving station mode,
 * and esp_wifi_stop() on an associated station emits WIFI_EVENT_STA_DISCONNECTED
 * on the way out. token_monitor.c's handler treats every one of those as "ask
 * again" and arms a retry timer -- so unless the caller has suppressed that
 * logic BEFORE calling in here, the reconnect machinery spends the whole of
 * setup mode fighting the mode switch. See s_setup_mode in token_monitor.c.
 *
 * WHAT THIS MODULE WILL NOT DO. It never touches the display. The panel
 * belongs to usage_task (see the note at the top of token_monitor.c), so this
 * module reports the network name, password and URL back to its caller and
 * lets the caller decide what they look like. It is the same rule the button
 * follows, and for the same reason.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "config.h"

/* The generated AP password: 8 characters plus the NUL.
 *
 * Eight is WPA2's minimum, and also the most that is comfortable to read off a
 * round 240px panel and type on a phone. It is random per entry into setup
 * mode rather than derived from the MAC address -- the MAC is broadcast in
 * every beacon frame, so a derived password would be computable by anyone in
 * range, which is barely different from leaving the network open. */
#define PROV_PASS_CAP  9

/* "http://192.168.4.1" and a NUL, with room to spare if the AP's subnet is
 * ever configured differently. */
#define PROV_URL_CAP   32

/* What the caller has to put on the screen for any of this to be usable.
 *
 * The URL is separate from the SSID and password because it is the one value
 * that is NOT a secret and NOT random -- it is fixed by the AP's subnet -- but
 * it is still the thing a person cannot guess, because this device has no
 * captive-portal DNS hijack to volunteer it (a decision, not an omission: the
 * gadget has a screen, which is exactly what a headless device lacks). */
typedef struct {
    char ssid[CFG_SSID_CAP];      /* e.g. "TOKEN-MON-A3F2"   */
    char password[PROV_PASS_CAP]; /* e.g. "K7QMX4RD"          */
    char url[PROV_URL_CAP];       /* e.g. "192.168.4.1"       */
} provision_info_t;

/* Leave station mode, raise the hotspot, and start serving the form.
 *
 * `current` supplies the values the form is pre-filled with: the SSID, host
 * and port. On an unconfigured chip those are empty, which is fine and is the
 * expected case -- the form comes up blank and waits to be told.
 *
 * The password in `current` is deliberately NOT used. The form's password
 * field is always blank, with "leave blank to keep current" under it, for the
 * same reason the token never leaves the PC. Nothing that can echo a stored
 * credential back over the network gets to exist.
 *
 * On success `out` is filled with what the caller must display, and this
 * returns ESP_OK with the AP and the HTTP server both running on their own
 * tasks -- it does not block. On failure it undoes whatever it managed to
 * start and returns the error; the caller is then holding a chip with the
 * radio in an unknown state, and rebooting is the only sane response.
 *
 * Do not call twice. There is one radio and one server.
 */
esp_err_t provision_start(const app_config_t *current, provision_info_t *out);

/* True once a submission has been accepted and written to NVS.
 *
 * The caller polls this rather than the server calling back, because the two
 * things that have to happen next -- putting something on the panel, and
 * rebooting -- both belong to the task that owns the display, and neither may
 * happen on the server's task while it is still finishing a response. The flag
 * is set only after the reply has gone out, so by the time the caller sees it
 * the phone already has its confirmation page.
 *
 * Never clears. A save is the end of setup mode; the reboot is what resets it.
 */
bool provision_saved(void);
