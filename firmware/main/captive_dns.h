/* The captive-portal half of setup mode: a DNS server that lies.
 *
 * WHAT IT IS FOR. A phone that joins a WiFi network immediately asks the
 * internet a question it already knows the answer to -- Android fetches
 * http://connectivitycheck.gstatic.com/generate_204, Apple fetches
 * http://captive.apple.com/hotspot-detect.html -- purely to find out whether
 * this network really reaches the internet. If the expected answer comes back,
 * the phone goes quiet. If something *else* comes back, the phone decides it
 * is behind a sign-in page and opens it, which is how hotel WiFi works.
 *
 * This module is the first half of that trick: it answers every DNS query,
 * whatever the name, with the gadget's own address. The phone's probe then
 * lands on our HTTP server rather than on Google, and the redirect it gets
 * there (see provision.c) is what makes the setup form appear on its own.
 *
 * WHY IT EXISTS AT ALL, given the project decided against it. The original
 * reasoning stands: the captive-portal trick exists because headless devices
 * cannot tell you their address, and this gadget has a screen that does. The
 * decision was explicitly "add the DNS responder later only if typing
 * 192.168.4.1 proves annoying" -- and after several rounds of hardware testing
 * it had. The screen still shows the URL, and everything still works if this
 * never fires; that is the point of it being additive.
 *
 * WHAT IT IS NOT. It is not a DNS server. It parses just enough of a query to
 * find where the question ends, and answers every A-record lookup with one
 * fixed address. It has no cache, no recursion, no upstream, and it is
 * deliberately unreachable outside setup mode -- during which the only network
 * it is attached to is one this device created, with one client on it.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Start answering DNS on the AP interface.
 *
 * `ip` is the address to hand out for every name, in network byte order --
 * i.e. straight from esp_netif_ip_info_t.ip.addr, which is where the caller
 * gets it. Passing the AP's own address is the only sane value; passing
 * anything else would point phones somewhere this device cannot serve.
 *
 * Runs on its own small task. Safe to call when a DNS server is already
 * running: the second call does nothing and says so.
 *
 * A failure here is not a reason to abandon setup mode. The form is still
 * reachable by typing the address from the screen, which is how it worked
 * before this existed -- so the caller should log and carry on rather than
 * tear the hotspot down.
 */
esp_err_t captive_dns_start(uint32_t ip);

/* Stop the responder and free its socket.
 *
 * Only needed on the paths that abandon setup mode without rebooting, since a
 * reboot takes the task with it. Safe to call when nothing is running.
 */
void captive_dns_stop(void);
