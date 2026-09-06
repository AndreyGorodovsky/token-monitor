# Where this build stands

A running log of what is done, what is verified, and what was learned the
hard way. Read it with `CLAUDE.md` (the brief) and `ARCHITECTURE.md` (the
design) to resume cold.

**One-line status:** stages 1-5 done and verified on hardware; stage 6
(bring the proven GC9A01 driver into `firmware/`) is next.

## Done and verified

| Stage | State |
|---|---|
| 1 — prove the API call | done, cross-checked against `/status` |
| 2 — polling service + LAN endpoint | done, verified from a phone on WiFi and under a real HTTP 429 |
| display bring-up (GC9A01 wiring + init) | **verified on hardware** with a standalone solid-fill test; the driver still has to be brought into `firmware/` at stage 6 |
| 3 — firmware WiFi | **done** — chip gets a lease on the same subnet as the PC |
| 4 — HTTP client sanity check | **done** — 200 from example.com, full body over serial, heartbeat survives |
| 5 — talk to the real `pc_service` | **done** — 200, parsed, values match what the PC serves field for field |
| 6-8 | not started |

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

## Stage 4 as built

One plain-HTTP GET to `http://example.com/`, fired once after the DHCP lease
arrives, printing the response headers and raw body over serial. `esp_http_client`
was added to `REQUIRES` in `main/CMakeLists.txt`. Three decisions worth not
re-litigating:

- **It runs in its own 8 KB task, not in `app_main`.** `CONFIG_ESP_MAIN_TASK_STACK_SIZE`
  is 3584 bytes here, and `esp_http_client` needs appreciably more than that
  once lwIP and the HTTP parser are on the stack. Overflowing it is a stack
  canary panic and a reboot, not an error return. 8 KB is what ESP-IDF's own
  `esp_http_client` example uses.
- **The body is accumulated across `HTTP_EVENT_ON_DATA` callbacks** into a
  4 KB buffer with an explicit truncation flag. The response does not arrive
  in one piece — this is the whole shape stage 5 has to hand to cJSON, which
  needs a complete document.
- **`example.com`, not the real service, and not HTTPS.** A throwaway URL
  keeps a stage-5 failure single-suspect, and also proves DNS. HTTPS would
  drag in TLS and a cert bundle that plain-HTTP `pc_service` will never need.

**Verified on hardware.** Status 200, all eleven response headers logged,
559 bytes of body accumulated intact, and the heartbeat kept printing
afterwards -- so the HTTP task neither overflowed its stack nor blocked
anything else.

## Stage 5 as built

The stage-4 request machinery, pointed at the real service and given a
parser. `json` (IDF's bundled cJSON, 1.7.19 in v5.3.5) joined `REQUIRES` the
same way `esp_http_client` did -- no download, no `managed_components/`.
The accumulator, the task, and the 8 KB stack are unchanged from stage 4.
What is new:

- **The URL is assembled from `secrets.h` at compile time**, not written
  into the source: `"http://" PC_SERVICE_HOST ":" STRINGIFY(PC_SERVICE_PORT)
  "/usage"`. The two-step `STRINGIFY` is required rather than decorative --
  `#` stringifies its argument *before* expanding it, so a one-step version
  would bake in the literal text `PC_SERVICE_PORT`. Verified by grepping the
  built binary for the expanded string, which is also the cheapest way to
  confirm the real host never lands in a tracked file.
- **Only the two percentages are mandatory.** Reset times, the `updated`
  pair and `now_epoch` are all allowed to be absent or JSON null by the
  contract, so they parse into `""` / `0` and print as `--`. Three typed
  accessors (`json_get_int`, `json_get_epoch`, `json_get_str`) do the type
  check, and cJSON's type check handles null for free.
- **200, 503, other statuses and truncation are four separate outcomes**,
  each with its own message. A 503 is parsed for its `error` string by a
  separate three-line function rather than through `parse_usage`, which
  would only report the percentages as missing -- true, but unhelpful.
- **Still one shot, not a loop.** The periodic refresh is stage 8; keeping
  this single-shot means a failure is one request to read, not a scrolling
  log.

**Verified on hardware.** Status 200, 278 bytes, and a parsed block whose
values match what the PC was serving at that moment field for field --
checked against `curl` on the PC rather than eyeballed for plausibility.

### All three response paths verified, not just the happy one

The 200 path is the one that proves the contract, but it is the *degraded*
paths that the definition of done cares about ("degrades visibly, not
silently"), and code that has never run is not evidence of anything. Both
were exercised deliberately on hardware, neither needed a reflash:

- **Service unreachable** — stop `pc_service`, reset the chip. Result:
  `ESP_ERR_HTTP_CONNECT` with the three-suspect hint, heartbeat continuing.
- **503, no data yet** — a throwaway stub on the same port answering the
  documented `{stale, error}` shape (kept at
  `pc_service/tools/stub_503.py`). Result: `pc_service has no data yet
  (503): upstream request failed`, logged as a warning, heartbeat
  continuing. Reusable at stage 8, when this becomes a screen rather than a
  log line.

Two things the failure runs taught, both worth keeping:

- **A closed port costs the full 10s timeout, not an instant refusal.**
  Windows drops packets to a port nothing is listening on rather than
  sending a TCP reset, so the connect attempt black-holes until
  `timeout_ms` expires. Consequence: "`pc_service` isn't running" and "the
  firewall is blocking it" are indistinguishable by timing, which is why
  the error message names both.
- **The stub has to run under the same `python.exe` as `pc_service`.** The
  Windows firewall rule is per-program, so a different interpreter is
  silently blocked and the chip sees a 10s timeout instead of the 503 you
  are trying to test -- a confusing way to lose ten minutes.

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
  a fault.** Four observed boots: ~14s (fail, fail, connect), then 2.2s with
  no retries, then ~20s (fail, fail, connect), then ~22s at stage 5 (fail,
  fail, fail, connect — three retries). Same code, same network.
  So retries are occasional, not characteristic — the delay in the disconnect
  handler is what makes the slow cases recover unattended. Don't chase
  reason-2 lines at startup, and don't read a fast connect as proof they're
  gone.
- **Signal has been marginal: RSSI -64 to -70 dBm** across sessions, drifting
  with where the board sits. Workable, and it has held steady, but it is the
  weak link if the chip ever starts dropping out. Check the XIAO's antenna is
  seated before debugging anything else.
- **example.com answers `Transfer-Encoding: chunked`** (it is served through
  Cloudflare), so there is no `Content-Length` header and
  `esp_http_client_get_content_length()` returns **-1**. This is not a fault,
  and it is the more valuable test: chunked guarantees the body arrives in
  several `HTTP_EVENT_ON_DATA` callbacks, so stage 4 passing proves the
  accumulator genuinely works rather than getting lucky with a single-shot
  response. **The rule for stage 5: the accumulator's own byte count is the
  source of truth about how much body arrived; `content-length` may be -1 and
  must never gate the parse.** (The stage-4 log line prints the raw -1; adding
  a "(chunked)" note to it is a nice one-liner to fold into stage 5, not worth
  a reflash on its own.)
- **Entering download mode needed the buttons, and leaving it needs another
  press.** A flash attempt failed repeatedly with `Failed to connect to
  ESP32-C3: No serial data received` even though the port enumerated fine
  (`VID_303A&PID_1001`, stable, nothing else holding it) -- neither
  `--before default_reset` nor `--before usb_reset` could get the chip into
  download mode. Holding `B`, tapping `R`, releasing `B` fixed it. The
  follow-on trap: the board then boots `rst:0x15 (USB_UART_CHIP_RESET),
  boot:0x0 (USB_BOOT)` and prints `wait usb download`, which looks like a
  failed flash but is just the BOOT strap still low. Tapping `R` alone --
  without `B` -- boots the app normally.
- **The app is using most of a 1 MB partition already.** Stage 4 built to
  0xe3500 with 11% of the app partition free; stage 5 is 0xe64b0 with 10%
  free, so cJSON cost about 12 KB. WiFi plus the HTTP client is most of the
  total, and the display driver itself is small — but if stages 6-7 run out
  of room, a custom partition table is the fix, not code golf.
- **`esp_http_client` sends no `Connection` header at all**, and under
  HTTP/1.1 that means keep-alive. The server therefore held the socket open
  after answering, with a thread parked in a read waiting for a second
  request that was never coming; when the chip closed, that pending read
  surfaced on Windows as an abort (`WinError 10054`) rather than a clean EOF,
  and `pc_service` printed a full traceback for every single fetch. Harmless
  once, but stage 8 would have printed one a minute forever and buried real
  errors. Fixed on both ends: the firmware now sends `Connection: close`
  (right for a once-a-minute poll -- no server holds an idle connection open
  that long anyway), and `handle_one_request` swallows the two
  connection-loss errors for every other client. **The firmware side is the
  actual fix**, proven by a deliberately unguarded stub server taking the
  request and logging zero tracebacks.
- **`pc_service` sends a real `Content-Length`** (Python's `http.server`
  does), so the chunked path stage 4 exercised does not come up against the
  real service — the observed run was `content-length 278, body 278 bytes`.
  The `-1` handling stays anyway: it costs one comparison, and it is the
  difference between working and not if this ever moves behind a proxy.
- **Reset epochs jitter by a second between polls.** Upstream sends
  `resets_at` with sub-second precision and the service truncates to an int,
  so the same reset instant can serve as `...799` on one poll and `...800`
  on the next. The displayed `HH:MM` is rounded and does not move. Two
  consequences: a "resets in 3h12m" countdown built on these epochs must not
  treat them as stable to the second, and a one-second difference between two
  observations is not evidence of a parsing bug (it briefly looked like one).
- **Never read epochs out of cJSON's `valueint`.** cJSON stores every number
  as both a double and an `int` clamped to `INT_MAX`. A Unix epoch (1.79e9)
  still fits a 32-bit int today and stops fitting in 2038, at which point
  `valueint` would silently pin to 2147483647 while `valuedouble` stays
  exact (a double holds every integer to 2^53). `json_get_epoch` uses the
  double deliberately.
- **RSSI readings swing far too much to trust a single sample.** One capture
  showed a steady -86/-87 dBm, and a reset a minute later — same board, same
  position — showed a steady -62 dBm. So the "marginal signal" note above is
  about the trend, not any one number; take several readings before moving
  the board or blaming placement.
- **`idf.py monitor` cannot be driven non-interactively** (it exits on a
  keypress), which makes capturing a boot log awkward from a script. What
  works: open the port with pyserial, set DTR low and pulse RTS — on the
  C3's USB-Serial/JTAG peripheral RTS drives CHIP_PU, so that reboots
  straight into the app rather than into download mode — then read for a
  fixed number of seconds. Without the reset pulse you miss the boot
  entirely, because `idf.py flash` has already reset the chip by the time
  the port is reopened.
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

**Stage 6: display bring-up inside `firmware/`.** The panel, the wiring and
the vendor init sequence are already proven on hardware by the standalone
solid-fill test (see the table above), so this stage is a port, not a
bring-up from scratch: bring that hand-rolled `spi_master` + `gpio` driver
into `firmware/main/`, keep `madctl = 0x08`, and fill the screen with a
solid colour with nothing else in the way. Only then does stage 7 render the
`usage_t` that stage 5 already fills in.

Two things to carry across. The pinout is in `ARCHITECTURE.md` and is
confirmed working — check GND/VCC against the board's silkscreen first if
the screen is dark, since that failure mode is total darkness with clean
serial logs. And the app partition is at 10% free: if the driver does not
fit, the fix is a custom partition table, not shrinking code.

`pc_service` does **not** need to be running for stage 6 — nothing in that
stage talks to it. It is needed again from stage 7 on (see Housekeeping).

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
- **Expressive face on the display, instead of numbers alone.** Requested
  during stage 6 planning: a smiley whose expression tracks usage (happy
  when there is headroom, unhappier as the windows fill), rather than two
  percentages and two reset strings on their own. Deliberately deferred so
  stages 6-8 land as planned first — **revisit once the display works and
  the real free-flash number is known**, which is the whole reason for
  waiting.

  The analysis, so it does not have to be redone:

  | Approach | Cost per face | Notes |
  |---|---|---|
  | Procedural (circles + arcs) | ~0 bytes | Mouth curvature can be a continuous function of usage; animation free |
  | 1-bit sprite, coloured on-chip | ~1.8 KB at 120px | Hand-designed art; a 10-frame blink ≈ 18 KB; needs a PNG→header script |
  | Full-colour RGB565 sprite | ~28.8 KB at 120px | Four faces ≈ the entire remaining budget |

  Constraint that drives the choice: **a single 240x240 RGB565 frame is
  115,200 bytes**, and stage 5 leaves about 103 KB free in the app
  partition — so one full-screen stored frame does not fit at all, and
  literal GIF playback is out unless the partition table changes. A GIF
  *decoder* would be the wrong tool regardless: the assets are fixed at
  build time, so shipping an LZW decoder to unpack something that could
  have been pre-converted is pure overhead.

  Three design notes worth keeping with it. Buckets (happy under 25%,
  neutral under 50%, ...) are the obvious reading of the request, but an
  arc's curvature can vary *continuously* with the percentage for the same
  code, which stays glanceable while also being precise — colour can carry
  the coarse band on top. The face should probably track the **worse of the
  two windows**, since a happy face while the 7-day sits at 90% would be
  actively misleading. And a distinctive "asleep"/"?" face is a better
  visible degrade for "service unreachable" than a text banner, which is
  the definition-of-done requirement anyway — so this idea and stage 8's
  fallback state want designing together.

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
