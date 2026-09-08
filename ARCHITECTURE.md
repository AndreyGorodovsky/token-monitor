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

**Polling.** Poll on an interval (**120s** as implemented — this is a
subscription usage counter, not something that needs sub-minute freshness,
and polling less often is politer to an undocumented endpoint. 60s was the
original default and proved too fast: it drew a steady stream of 429s even
from a single instance. See `STATUS.md`). Keep the
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
interval matching or slightly looser than `pc_service`'s own polling — no
need to poll faster than the data actually changes, and nothing new appears
between the service's own polls anyway. The brief asks for a 30-60s refresh
on the display, which is fine: the chip is reading a local cache, not the
upstream endpoint, so its interval and the service's are independent.

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
  panel (vendor init table, and `madctl = 0x48` — the BGR bit for correct
  colour order, plus MX, without which everything draws mirrored; see
  `STATUS.md` for why that took until stage 7 to notice). Benefits of the hand-rolled route: no `managed_components/`, no
  internet needed for a first build, and no external dependency to track.
  Revisit `esp_lcd` only if a concrete need appears (e.g. DMA-backed
  double-buffering or LVGL). The SPI clock is 40 MHz, raised from the
  bring-up value of 10 MHz once the panel was known-good; long jumper wires
  are the usual reason to have to put it back.

- **Redrawing.** A full-screen repaint on every refresh is a visible black
  flash once a minute, so the firmware keeps a model of what it last drew and
  repaints only the rows whose text or colour changed. Rows rather than
  glyphs: every line is centred, so a shorter string starts further right and
  would strand the tail of the previous one unless the whole row band is
  cleared first.
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

**Setup button on the XIAO ESP32-C3:**

| Button | XIAO pin | GPIO |
|---|---|---|
| one leg | D1 | GPIO3 |
| other leg | GND | — |

No resistor of your own. The firmware enables the chip's internal pull-up, so
a released button reads 1 and a pressed one reads 0 — pressed is LOW, which is
worth knowing before reading the driver.

D1 was chosen over the alternatives for two reasons, both about what happens
at reset rather than during normal operation:

- **It has no strapping role.** GPIO2, GPIO8 and GPIO9 are sampled at reset to
  decide how the chip boots, so a button holding one of them at a fixed level
  can change the boot mode. GPIO2 is the trap for this particular wiring: it
  must be high at reset, and a button to GND idles it low.
- **It is not the serial console.** GPIO20/21 are UART0, i.e. the serial
  monitor used to debug everything else.

GPIO9 is the onboard BOOT button and needs no wiring at all, which is
tempting — but held across a reset it drops the chip into the bootloader, so
it is a poor choice for the control you reach for when something is confusing
you. It is also a surface-mount button an enclosure would hide.

A **3-second hold** is what counts; short taps are ignored, deliberately,
because a desk object that reconfigures itself when brushed is a bad desk
object. The same 3-second hold is also how you leave setup mode again.

**Layout.** 240x240 color and round is a canvas worth designing *for*,
rather than dropping a rectangular block of text into. Some options, not
prescriptive: a ring/arc gauge around the edge for each utilization
percentage (visually natural on a round display), color as a signal (e.g.
green → yellow → red as usage climbs toward 100%), large scalable text now
that there's real pixel budget. Land on something in the build itself rather than over-designing it
here — the important constraint is just the stale/unreachable state below,
which needs to stay visible in whatever layout is chosen.

**As built.** Text first (stage 7), gauge arcs added afterwards — in that
order deliberately, so the numbers were proven readable before anything
decorative went near them.

```
         . - - - - - - - - .        5-hour arc: the rim of the top half,
     .  '   _______________  ` .    clockwise from nine o'clock
   '      /                 \    `
  |          5 - H O U R          |
  |             7 0 %             |   large, colour-banded
  |            1 5 : 3 0          |
  |        - - - - - - - -        |   divider, or the status banner
  |           7 - D A Y           |
  |             1 6 %             |
  |          T H U  1 9 : 0 0     |
   .      \_________________/    ,   7-day arc: the rim of the bottom half,
     .  ,                    . '     also clockwise, from three o'clock
         ' - - - - - - - - '
```

Three things about it are load-bearing rather than taste:

- **The arcs tile the ring as two halves and never overlap.** The 5-hour
  gauge is anchored at nine o'clock and sweeps clockwise over the top; the
  7-day gauge is anchored at three o'clock and sweeps clockwise under the
  bottom. Each is capped at a half turn (1.8° per percent), so at 100% and
  100% they meet at nine and three and close the circle exactly. There is no
  unfilled "track" behind either: an empty rim reads as zero perfectly well,
  and a track would compete with the numbers for attention.
- **An arc takes its colour from the same function as its number**, including
  the flat grey of data too old to be a claim about now. An arc still green
  beside a grey number would be the display contradicting itself.
- **Text and ring have separate territory, and the boundary is enforced in
  one place.** Everything that is not the ring — glyphs, the opaque
  background behind them, and every row clear — stays inside `CONTENT_R`,
  two pixels inside the ring's inner edge. This is not fussiness: the row
  clears originally ran the full panel width, which quietly cut two bites out
  of the ring on every text row that changed. It looked correct until the
  first percentage moved, because the arcs are drawn last on a full repaint.

The status banner sits in the middle, sharing a slot with the divider, and
that is a consequence of the arcs rather than a preference — see the failure
states above. At the bottom of a round panel there is only room for eleven
characters, and `BAD DATA 12M` is twelve; the ring then took that space
entirely. In the middle the circle is at its widest, and the banner
*displacing* the divider makes it harder to overlook than adding a line would.

**Failure states to show, not hide:**

- WiFi not connected yet → explicit "connecting..." state, not a blank or
  frozen screen.
- `pc_service` unreachable (connection refused/timeout) → keep showing the
  last known values but visibly marked stale (e.g. a "!" or dimmed/inverted
  indicator), so it's clear at a glance whether the numbers on screen are
  live or last-known.
- `pc_service` reachable but `"stale": true` in the response (its own
  upstream call to Anthropic failed) → same stale treatment.

**As built (stage 8).** All three are implemented as a banner on the bottom
line over the retained numbers — `NO WIFI`, `NO LINK`, `NO DATA`, `BAD DATA`,
or `STALE`, each followed by the real age of the data (`NO LINK 3M`). The
words-only screen is reserved for the case where nothing has ever been fetched
and there is genuinely nothing better to show.

One thing was added that the list above doesn't ask for, because "marked
stale" turns out to be two different states rather than one. Staleness is
graded by the data's age, computed from `now_epoch - updated_epoch` plus the
time elapsed on the chip since that fetch — the second half being what keeps
the age honest once the service stops answering at all, since no fresh
`now_epoch` arrives then:

| Age | Treatment |
|---|---|
| under 10 min | no banner (unless `"stale": true`, which always shows one) |
| 10–30 min | amber banner with the age; numbers keep their usage colours |
| over 30 min | red banner; numbers go flat grey |

The grey-out is the part worth keeping deliberately. A badge says "this might
be old"; removing the colour says "this is not a statement about now", which
is a different and stronger claim — and it stops a red 94% from alarming
someone about a number that stopped being true an hour ago.

The thresholds are set against this service's own backoff rather than picked
round: a single upstream 429 delays the next successful poll to t=360s and two
in a row to t=840s, so data can legitimately reach 6 and 14 minutes old with
nothing wrong. Anything tighter than 10/30 would fire on normal, recovering
conditions — and an indicator that cries wolf is one that gets ignored.

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
