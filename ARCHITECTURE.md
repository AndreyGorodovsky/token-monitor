# Architecture

## Data flow

```
                       HTTPS (internet)
  ┌────────────┐   GET /api/oauth/usage    ┌──────────────────┐
  │  Anthropic │ <───────────────────────  │   pc_service      │
  │  servers   │  ───────────────────────> │   (host PC,        │
  └────────────┘   { five_hour, seven_day } │   always running)  │
                                            └─────────┬─────────┘
                                                       │ HTTP (LAN only)
                                                       │ GET /usage
                                                       │ { pct, resets_at, ... }
                                                       ▼
                                            ┌─────────────────────┐
                                            │  ESP32-C3 firmware    │
                                            │  (WiFi station)        │
                                            └─────────┬─────────────┘
                                                       │ SPI
                                                       ▼
                                            ┌─────────────────────┐
                                            │  GC9A01 round TFT      │
                                            │  (240x240, color)      │
                                            └─────────────────────┘
```

The token that authenticates to Anthropic exists only inside `pc_service`,
on the PC. Everything past the first hop carries only already-computed
percentages and timestamps — nothing that could be replayed against
Anthropic's servers.

## Component 1: `pc_service`

Runs continuously on the PC (started manually for now — a background/
scheduled-start setup can come later once the manual version works).

**Credential source.** Claude Code stores its OAuth token locally. On
Windows that's a JSON file at:

```
%USERPROFILE%\.claude\.credentials.json
```

Read it read-only. Don't assume the exact key name inside without checking
the file's actual contents on the target machine first — inspect it before
writing the parsing code.

**Upstream call.**

```
GET https://api.anthropic.com/api/oauth/usage
Authorization: Bearer <oauth_access_token>
anthropic-beta: oauth-2025-04-20
```

This is an undocumented endpoint reverse-engineered from Claude Code's own
behavior — it's not in Anthropic's public API docs. Treat its response
schema as best-effort: parse defensively, and don't let an unexpected field
or a non-200 response take the whole service down.

Known response shape, confirmed against the live endpoint during the
`pc_service` build (see `STATUS.md`):

```jsonc
{
  "five_hour": { "utilization": 24.0, "resets_at": "2026-09-01T17:00:00.123456Z" },
  "seven_day": { "utilization": 61.0, "resets_at": "2026-09-05T00:00:00.123456Z" },
  "seven_day_opus": { ... } | null,
  "seven_day_sonnet": { ... } | null
}
```

Two things that weren't obvious up front: `utilization` is a **float**, not
an int (round it, don't assume it's already whole), and `resets_at` is UTC
with microseconds, which is both awkward to hand a microcontroller and the
wrong value to show a person directly — convert it before it leaves this
service (see the contract below).

**Polling.** Poll on an interval (60s is a reasonable default — this is a
subscription usage counter, not something that needs sub-minute freshness,
and polling less often is politer to an undocumented endpoint). Keep the
last successfully-fetched result in memory. If a poll fails, keep serving
the last good result but mark it stale (see contract below) rather than
blanking it.

**Local endpoint (LAN-facing, what the ESP32 talks to).** Bind to
`0.0.0.0` on a chosen port (suggest `8734`, or anything unused) so the
ESP32 — on the same WiFi network — can reach it by the PC's LAN IP. No
authentication on this endpoint for v1: it only ever serves already-public-
to-this-LAN derived numbers, never the token, and the trust boundary is
"same home network." Note this as a known simplification, not something to
silently harden without discussing it — if the network ever changes (e.g.
the PC ends up on an untrusted network), that assumption should be
revisited.

**JSON contract served to the firmware** (`GET /usage`) — this is the
*actual, implemented* contract as of the `pc_service` build (it changed
once from the original plan below, by agreement — see `STATUS.md` for why):

```jsonc
// HTTP 200 — normal response
{
  "five_hour_pct": 42,                    // int 0-100, already rounded
  "five_hour_resets_at": "17:00",         // PC-local "HH:MM", or "Fri 17:00"
  "five_hour_resets_epoch": 1788291600,   // same instant, as a Unix epoch int
  "seven_day_pct": 61,
  "seven_day_resets_at": "Fri 00:00",
  "seven_day_resets_epoch": 1788652800,
  "updated_at": "15:04",                  // PC-local time of the last good poll
  "updated_epoch": 1788285840,            // same instant, as a Unix epoch int
  "now_epoch": 1788285900,                // PC's current time, for on-chip math
  "stale": false                          // true = serving last-known data
}

// HTTP 503 — never polled successfully yet (no last-known data to fall back to)
{
  "stale": true,
  "error": "human-readable reason"
}
```

Why both a display string and an epoch for each reset time: `resets_at` is
already formatted for showing on screen (PC-local, rounded to the minute,
with a weekday prefix only when the reset isn't today) so the firmware
doesn't need to do timezone or calendar math; `resets_epoch` is the same
instant as a plain integer, in case the firmware ever wants to compute
against it (e.g. "resets in 3h12m") rather than just display the string.
`now_epoch` is included so the firmware has a reference point for that kind
of math without needing its own reliable clock.

`updated_at` and `updated_epoch` are the same pairing, for the same reason:
the string is for showing, the epoch is for deciding. `updated_at` alone
cannot express *how* stale stale is — three-day-old data renders as "15:04",
which reads as this afternoon. With `updated_epoch` against `now_epoch` the
firmware can compute the real age and choose when data is too old to show at
all rather than only flagging it.

The `error` string in a 503 is a deliberately coarse category ("upstream
request failed", "rate limited upstream"), not the underlying exception text.
The detailed reason — which can include upstream request IDs and account
detail from a 401/403 body — is logged on the PC only. This endpoint is
unauthenticated on the LAN, so it gets the category and nothing more.

A `503` is a real, expected response shape — it means nothing has ever been
fetched successfully (e.g. right after `pc_service` starts), so there's no
last-known payload to serve even as stale. The firmware should treat this
the same as a connection failure: show the "no data" state, not a crash or
a parse error.

Keep this contract small and stable — the firmware's parser depends on it
exactly. If a field needs to change, update both sides together.

**Language/runtime.** Python is a reasonable default (simple HTTP client +
server, JSON handling, and it's likely already installed) but pick whatever
is actually available and simplest to keep running on the host machine.

## Component 2: `firmware`

A standard ESP-IDF project, target `esp32c3`.

**WiFi.** Station mode via `esp_wifi` + `esp_event`, credentials from a
gitignored config (not committed). Log connection state and IP address over
serial — this chip has no display feedback for WiFi state by default, so
serial is the only debugging channel until the display path is working.

**HTTP client.** `esp_http_client`, GET to `pc_service`'s LAN IP:port
(itself from the same gitignored config — it'll change if the PC's IP
changes, so make it easy to update, not buried deep in code). Poll on an
interval matching or slightly looser than `pc_service`'s own polling (e.g.
every 60s) — no need to poll faster than the data actually changes.

**JSON parsing.** `cJSON` (ESP-IDF's `json` component) against the contract
above.

**Display hardware.** A round 1.28" TFT, driver chip GC9A01, 240x240 pixels,
16-bit color (RGB565), SPI interface. Things about it that shape the code:

- **Bus.** SPI, not I2C. Each SPI device is wired point-to-point (see pinout
  below), so there is no bus address to scan for — a silent display is a
  wiring or init problem, never an addressing one.
- **Color depth.** 16 bits/pixel (RGB565). A full 240x240 framebuffer is
  240x240x2 = 115,200 bytes — fine for this chip's ~400KB SRAM, but not
  free, and worth keeping in mind for RAM budgeting. Partial-window updates
  are an option if it ever becomes tight.
- **Init sequence.** GC9A01's power-on/init command sequence is long and
  display-specific. `esp_lcd` plus a GC9A01 panel driver from the ESP
  Component Registry is one route; this project instead uses a hand-rolled
  driver on plain `spi_master` + `gpio`, which is confirmed working on this
  panel (vendor init table, and `madctl = 0x08` for the correct color
  order). Benefits of the hand-rolled route: no `managed_components/`, no
  internet needed for a first build, and no external dependency to track.
  Revisit `esp_lcd` only if a concrete need appears (e.g. DMA-backed
  double-buffering or LVGL).
- **Board notes (from the module's silkscreen):** it has an onboard
  pull-down on CS (`R8`), and the board text explicitly says CS and RST
  don't have to be wired to a GPIO to work — they can be left disconnected
  and it'll still function, since this display is the only SPI device on
  the bus. Wiring them anyway (see pinout) is still recommended: it gives
  firmware explicit control (e.g. issuing a clean reset), and this chip has
  spare GPIOs to spare.

**GC9A01 pinout on the XIAO ESP32-C3:**

| Display pin | XIAO pin | GPIO |
|---|---|---|
| GND | GND | — |
| VCC | 3.3V-OUT | — |
| SCL (SCK/CLK) | D2 | GPIO4 |
| SDA (MOSI/DIN) | D3 | GPIO5 |
| RES (RST) | D4 | GPIO6 |
| DC (A0/RS) | D5 | GPIO7 |
| CS | D8 | GPIO8 |

No MISO wire needed — this display is write-only from the chip's
perspective (the firmware never reads pixel data back). Confirm this exact
pin order against your board's own silkscreen labels before powering it —
particularly GND vs. VCC, since swapping those two is the one mistake that
can actually damage it; the signal pins (SCL/SDA/RES/DC/CS) are safe to
mis-map and just won't work until corrected in code.

**Layout.** 240x240 color and round is a canvas worth designing *for*,
rather than dropping a rectangular block of text into. Some options, not
prescriptive: a ring/arc gauge around the edge for each utilization
percentage (visually natural on a round display), color as a signal (e.g.
green → yellow → red as usage climbs toward 100%), large scalable text now
that there's real pixel budget. Land on something in the build itself rather than over-designing it
here — the important constraint is just the stale/unreachable state below,
which needs to stay visible in whatever layout is chosen.

**Failure states to show, not hide:**

- WiFi not connected yet → explicit "connecting..." state, not a blank or
  frozen screen.
- `pc_service` unreachable (connection refused/timeout) → keep showing the
  last known values but visibly marked stale (e.g. a "!" or dimmed/inverted
  indicator), so it's clear at a glance whether the numbers on screen are
  live or last-known.
- `pc_service` reachable but `"stale": true` in the response (its own
  upstream call to Anthropic failed) → same stale treatment.

## Security summary

- The OAuth token lives only in `pc_service`'s process memory and the
  credential file it reads from. It is read, never written or copied
  elsewhere.
- The only outbound use of the token is the HTTPS call to
  `api.anthropic.com`.
- The LAN endpoint between `pc_service` and the firmware never carries the
  token — only integers and timestamps.
- WiFi credentials and the PC's LAN address live in a gitignored config on
  the firmware side, with a committed `*.example` template so the pattern
  is visible without the real values being tracked.
