# Where this build stands

A running log of what is done, what is verified, and what was learned the
hard way. Read it with `CLAUDE.md` (the brief) and `ARCHITECTURE.md` (the
design) to resume cold.

**One-line status:** stages 1-3 done and verified on hardware; all code
reviewed, fixed, and commented; stage 4 is next and deliberately not started.

## Done and verified

| Stage | State |
|---|---|
| 1 — prove the API call | done, cross-checked against `/status` |
| 2 — polling service + LAN endpoint | done, verified from a phone on WiFi and under a real HTTP 429 |
| display bring-up (GC9A01 wiring + init) | **verified on hardware** with a standalone solid-fill test; the driver still has to be brought into `firmware/` at stage 6 |
| 3 — firmware WiFi | **done** — chip gets a lease on the same subnet as the PC |
| 4 — HTTP client sanity check | **not started — next** |
| 5-8 | not started |

`pc_service/` is complete: `fetch_usage.py` (one-shot check) and
`service.py` (poll + cache + `GET /usage`). Pure stdlib, no venv.
See `pc_service/README.md` for running it and troubleshooting.

The display is a round 1.28" GC9A01 TFT over SPI (240x240, color). The
panel, the wiring and the vendor init sequence have all been confirmed
working on hardware by a standalone solid-colour-fill test — raw
`spi_master`, no external components. That hand-rolled driver is what
`firmware/` uses at stages 6-7, rather than `esp_lcd` plus a Component
Registry driver: the point of the `esp_lcd` route was to avoid *writing* a
GC9A01 driver, and a working one now exists. Revisit that only if a concrete
need shows up (DMA-backed double-buffering, LVGL).

## Facts established so far (don't re-derive)

- **Token lives at** `~/.claude/.credentials.json`, key `claudeAiOauth.accessToken`.
  `expiresAt` is a millisecond epoch. Re-read every poll — Claude Code
  refreshes it, and caching it causes 401s after a few hours.
- **The PC's LAN address is DHCP-assigned**, and it is baked into the
  firmware's `secrets.h`. If the chip can't connect weeks from now, check
  that first — a router DHCP reservation would make it permanent.
- **The network path works.** A phone on WiFi reached the wired PC, proving
  both same-subnet and no firewall block. On Windows that reachability can
  rest on a firewall *program* rule for one specific `python.exe` — moving
  the service into a venv or a different Python install breaks it silently.
- **The JSON contract changed** from what `ARCHITECTURE.md` originally
  specified, deliberately: reset times are sent as PC-local display strings
  *plus* raw epochs. Upstream sends UTC with microseconds, which is both
  awkward on-chip and the wrong number to display. `ARCHITECTURE.md` has
  been corrected to match the actual implemented contract — both sides
  must still change together if it changes again.
- **The endpoint rate-limits.** Two service instances polling at once earned
  a 429. It sends `Retry-After: 0`, which must never be obeyed literally.
  Backoff is 120s -> 240s -> 480s -> capped 900s, reset on success.
- **`utilization` is a float** (`24.0`), not the int the doc predicted.
  Percentages are rounded in `build_payload()`.
- **macOS is unsupported** — Claude Code appears to use the Keychain there,
  not a file. `read_access_token()` raises an explanatory error. Windows and
  Linux share one code path via `pathlib.Path.home()`.
- **GC9A01 wiring gotcha: GND and VCC got swapped once during bring-up,
  producing a total-black, zero-backlight screen** (this board's backlight
  is tied straight to VCC with no separate control pin, so a power problem
  shows as *complete* darkness, not a bad picture). Firmware ran cleanly
  the whole time — clean serial logs prove nothing about physical wiring,
  since basic SPI transfers have no acknowledgment. Diagnosed by checking
  for backlight glow specifically (none = suspect power, not code) rather
  than trusting the clean log. Fixed by re-checking GND/VCC against the
  board's own silkscreen labels. Worth re-verifying GND/VCC first, before
  anything else, if the display ever goes dark again after rewiring.
- **GC9A01 pinout in use (confirmed working):** GND→GND, VCC→3.3V-OUT,
  SCL→D2/GPIO4, SDA→D3/GPIO5, RES→D4/GPIO6, DC→D5/GPIO7, CS→D8/GPIO8. Full
  table and rationale in `ARCHITECTURE.md`.
- **GC9A01 color order confirmed correct as-shipped** — a red/green/blue
  cycle showed true colors, no swap. `madctl = 0x08` is right for this
  board; carry it into `firmware/` unchanged, no calibration step needed.
- **Watch out for more than one ESP-IDF install on the same machine.** This
  build has been developed against ESP-IDF **v5.3.5**, pinned locally through
  the VS Code extension's `idf.currentSetup` setting (`firmware/.vscode/` is
  gitignored, being machine-specific). If a build ever fails with unfamiliar
  API errors, check which install the environment is actually using before
  suspecting the code. The authoritative version check is
  `components/esp_common/include/esp_idf_version.h`, not `version.txt`
  (which is empty in some trees).
- **The WiFi network used here is WPA3-SAE H2E.** Two consequences. First,
  `sae_pwe_h2e = WPA3_SAE_PWE_BOTH` in `token_monitor.c` is load-bearing,
  not boilerplate — a station that doesn't offer hash-to-element cannot
  associate at all. Second, the driver ignores the `WIFI_AUTH_OPEN`
  threshold anyway: with a password set it logs "authmode threshold changes
  from OPEN to WPA2" and raises it itself. Harmless, since WPA3 outranks
  WPA2 in that ordering.
- **WiFi auth threshold is deliberately `WIFI_AUTH_OPEN`** in
  `token_monitor.c`. That is a *minimum* security level, not "connect to open
  networks" — it means WPA2, WPA3 and mixed-mode routers all work without the
  config knowing which one is in use. Don't "tighten" it to `WPA2_PSK` without
  checking what the router actually runs; that is how WPA3-only networks stop
  connecting.
- **Connect time varies a lot, and `reason 2` (AUTH_EXPIRE) at startup is not
  a fault.** Three observed boots: ~14s (fail, fail, connect), then 2.2s with
  no retries, then ~20s (fail, fail, connect) again. Same code, same network.
  So retries are occasional, not characteristic — the delay in the disconnect
  handler is what makes the slow cases recover unattended. Don't chase
  reason-2 lines at startup, and don't read a fast connect as proof they're
  gone.
- **Signal has been marginal: RSSI -64 to -70 dBm** across sessions, drifting
  with where the board sits. Workable, and it has held steady, but it is the
  weak link if the chip ever starts dropping out. Check the XIAO's antenna is
  seated before debugging anything else.
- **Flash size was wrong and is now fixed.** IDF defaulted to 2MB; the XIAO
  ESP32-C3 has 4MB, so the bootloader logged "Detected size(4096k) larger
  than the size in the binary image header(2048k)" and stranded half the
  chip. Set in `firmware/sdkconfig.defaults` (not just `sdkconfig`, which is
  machine-generated and does not survive a fullclean or a fresh clone).
  **Confirmed on hardware:** the boot log now reads `SPI Flash Size : 4MB`
  and the warning is gone.
- **VS Code must be opened on `firmware/`, not the project root.** The
  ESP-IDF extension treats the open workspace folder as the project root and
  looks for `CMakeLists.txt` there; the root holds only docs, so any ESP-IDF
  command run from it fails with "CMakeLists.txt not found in project
  directory". A failed `set-target` from the wrong level also litters an
  empty `build/` and a stray `.vscode/settings.json` at that level.

## Code review — all findings fixed

A full review of `pc_service/` and `firmware/` turned up 8 real issues. All
are fixed and verified; none are outstanding. Recorded because several were
latent (correct on the development machine, wrong elsewhere) and would
otherwise look like unexplained churn later.

| # | Where | Issue |
|---|---|---|
| 1 | `STATUS.md` | A real WiFi SSID written into a tracked file bound for a public repo. Scrubbed. |
| 2 | `service.py` `_parse_iso` | `fromisoformat` rejects trailing `Z` before Python 3.11. Silent: reset times became `null` with `stale: false`. |
| 3 | `service.py` `UsageHandler` | HTTP/1.1 keep-alive with no `timeout`; one leaked thread per chip reboot. |
| 4 | `token_monitor.c` | `vTaskDelay` inside an event handler blocked the shared event loop. |
| 5 | `token_monitor.c` | `sizeof-1` truncated a 32-char SSID / 64-char PSK, presenting as `reason 201`. |
| 6 | `service.py` `store_failure` | 503 body forwarded upstream error text to any LAN client. |
| 7 | `service.py` `_format_reset` | Minute-rounding rolled past midnight; `now` took the reset instant's DST offset. |
| 8 | contract | `updated_at` had no epoch, so the chip couldn't tell 5-minutes-stale from 5-days-stale. |

Worth keeping in mind from this round:

- **#4 is the one to remember.** ESP-IDF event handlers run on a shared task;
  blocking one can overflow the 32-deep event queue and silently drop
  `IP_EVENT_STA_GOT_IP`, hanging `app_main` forever on a chip that holds a
  valid lease. Reconnect delays now go through `esp_timer_start_once`. Never
  sleep in an event handler — this will matter more at stages 4-8.
- **#8 changed the JSON contract.** `updated_epoch` is now in
  `ARCHITECTURE.md` and served by `pc_service`. The firmware parser doesn't
  exist yet, so nothing needed changing on the chip side — but that is the
  last free moment to have made this change.
- **#2 and #7 were both silent failures**, not crashes: wrong or missing data
  served with `stale: false`. Verified by test, not by inspection — a reset
  15s before midnight now renders `"00:00"` (today), not `"Thu 00:00"`.

Verification run: timestamp cases and boundaries asserted in Python; the
`_Static_assert` proven to fail at 33 characters and pass at exactly 32 by
compiling both through the real toolchain; firmware builds clean; the service
started live and served a correct payload including `updated_epoch`.

**Flashed and confirmed on hardware.** The boot after the fixes hit the
reconnect path for real — two `reason 2` disconnects, both routed through the
new `esp_timer` one-shot instead of blocking the event loop, recovering to a
valid lease. So #4 is verified in the field, not merely compiled.

## Commenting pass

All source is commented to be *read*, not just annotated — the intent being
that the code is followable without firmware background.

- `firmware/main/token_monitor.c` — a file map up top warning that control
  flow is not top-to-bottom (`app_main` sets up, then sleeps; the real work
  happens in handlers on another task); what each `#include` is for; what a
  FreeRTOS event group is; the full esp_event handler signature including why
  `void *data` must only be cast inside its `id` check; `IPSTR`/`IP2STR` as a
  matched pair; why `ESP_ERROR_CHECK` suits setup but not network calls; the
  four `xEventGroupWaitBits` arguments; why `= { 0 }` is load-bearing.
- `pc_service/service.py` — the threading model (main / one poller / one per
  connection), which is why `UsageCache` is the only thing holding a lock;
  why `RateLimited` must be caught before `RuntimeError` (it subclasses it);
  why `bool` is excluded in `_percent` (in Python `bool` subclasses `int`, so
  a stray `true` would render as 1%).
- `pc_service/fetch_usage.py` — why `HTTPError` precedes `URLError`; why
  urllib rather than `requests` (a venv would break the firewall rule).

Both Python files open with a "read it in this order" line.

Verified as comments-only: stripping comments leaves 104 lines of C,
statement-for-statement identical to before, with one deliberate exception —
`wifi_config_t wifi_cfg = { 0 };` moved above the `_Static_assert`s so the
comment claiming "zero-initialized above" is actually true. `_Static_assert`
emits no code, so this is semantically identical. Firmware rebuilt clean and
the Python assertions re-run green afterwards.

Note for next time: **binary hashes are not a valid reproducibility check
here.** ESP-IDF embeds a build timestamp, and whether it is regenerated
depends on what ninja decides to rebuild — identical source produced three
different hashes across runs in one session. Compare stripped source, not
file hashes.

## Next session — start here

**Stage 4: HTTP client sanity check — not started, and deliberately so.**
Stage 4 waits until it is explicitly asked for. Don't start it off the back
of a passing stage-3 log or a finished review.

When it is asked for: GET a trivial known endpoint with `esp_http_client` and
print the raw body over serial, *before* pointing it at the real service, so
an `esp_http_client` problem cannot be confused with a `pc_service` problem.
Concretely — add `esp_http_client` to `REQUIRES` in `main/CMakeLists.txt`,
use a plain-HTTP URL (HTTPS would drag in TLS and a cert bundle, none of
which stage 5 needs since `pc_service` is plain HTTP on the LAN), and
accumulate the body across `HTTP_EVENT_ON_DATA` callbacks — the response does
not arrive in one piece, and that same shape is what stage 5 hands to cJSON.

Everything stage 4 builds on is already proven: the chip connects and holds a
lease on the same `/24` as the PC, and `pc_service` was reached from a phone
on that WiFi. The network path needs no further proving.

`pc_service` is **not running** (see Housekeeping) — it isn't needed until
stage 5, not stage 4.

## Deferred, deliberately

- **Cross-platform preflight** (print LAN IPs, the exact `secrets.h` URL,
  firewall diagnosis on startup). Proposed, not built — `pc_service/README.md`
  covers the same ground in prose. Revisit at the polish stage if the chip is
  hard to debug.
- **`POLL_INTERVAL_SECONDS` stays 60.** 2/min is known to trigger a 429, but
  the real limit isn't; 1/min ran fine. Raise it only if 429s recur with a
  single instance.
- **The PC must be on.** Known v1 limitation; a future server/cloud relay
  would replace only the credential-reading layer. See `ARCHITECTURE.md`.

## Housekeeping

**Near-miss worth remembering:** a real WiFi SSID was written into this
tracked file as an incidental detail while documenting the WPA3 finding, and
`SECRETS.md`'s pre-publish checklist is the only thing that would have caught
it. Secrets don't only leak through code — prose notes about *debugging* are
the easy path. When writing up a network finding here, describe the property
(WPA3-SAE, channel, RSSI) and never the identifier. The same goes for real
LAN addresses and local filesystem paths: name the role, not the value.

`pc_service` is **not running** — it is stopped at the end of each session.
Nothing consumes it until the firmware talks to it. Restart with:

```
cd pc_service && python service.py
```

Confirm only one instance runs; a duplicate launch fails loudly at bind time
rather than silently double-polling.
