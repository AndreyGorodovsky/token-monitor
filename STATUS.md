# Where this build stands

A running log of what is done, what is verified, and what was learned the
hard way. Read it with `CLAUDE.md` (the brief) and `ARCHITECTURE.md` (the
design) to resume cold.

**One-line status:** **complete.** All eight stages done and verified on
hardware. The chip refreshes itself every 45s, recovers from a dropped network
unattended, and degrades visibly when it cannot get fresh numbers. Two gauge
arcs around the rim were added afterwards — see "The gauge arcs" below.

## Done and verified

| Stage | State |
|---|---|
| 1 — prove the API call | done, cross-checked against `/status` |
| 2 — polling service + LAN endpoint | done, verified from a phone on WiFi and under a real HTTP 429 |
| display bring-up (GC9A01 wiring + init) | **verified on hardware** with a standalone solid-fill test, before the driver came near this project |
| 3 — firmware WiFi | **done** — chip gets a lease on the same subnet as the PC |
| 4 — HTTP client sanity check | **done** — 200 from example.com, full body over serial, heartbeat survives |
| 5 — talk to the real `pc_service` | **done** — 200, parsed, values match what the PC serves field for field |
| 6 — display inside `firmware/` | **done** — red/green/blue cycle correct on the panel, alongside a running WiFi radio |
| 7 — real values on the screen | **done** — both readings, reset times and colour bands legible on the panel |
| 8 — polish | **done** — 45s refresh, backoff + reconnect, graded staleness and failure banners, all exercised on hardware |
| gauge arcs (post-stage-8) | **done** — one per window around the rim, 1,568 bytes, no RAM; forced the banner to the middle and exposed a full-width-clear bug |

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

## Stage 6 as built

The driver moved into `firmware/main/` as its own module — `gc9a01.c` +
`gc9a01.h`, not pasted into `token_monitor.c`, which is already long and has
nothing to do with pixels. `esp_driver_spi` and `esp_driver_gpio` joined
`REQUIRES` (the specific IDF 5.x components, not the legacy `driver`
umbrella). The public surface is two functions, `gc9a01_init()` and
`gc9a01_fill_screen()`; stage 7 adds the drawing primitives.

- **Nothing that touches the panel changed.** The register table, the reset
  timings, `madctl` and the 10 MHz clock were byte-for-byte what had been
  proven on this board. (`madctl` was `0x08` at this point; stage 7 corrected
  it to `0x48` — see below. Everything else still stands.) That is the entire point: stage 6 changes only the
  *surroundings* — a real project with a WiFi radio running — so a dark
  screen would have one suspect, the environment.
- **The clock stays at 10 MHz for now.** Many GC9A01 boards run at 40 MHz and
  stage 7 will want the speed once it is redrawing regions, but raising it in
  the same step as the move would have given a failure two suspects. Raise it
  later, on its own, with the screen already working.
- **It runs before `wifi_start()`.** The panel does not depend on the
  network, so doing it first means the screen lights within a third of a
  second of power-on instead of after a 20-second WiFi join — and a dark
  screen cannot be blamed on WiFi that has not started yet.
- **It rests on blue, not black.** Black was the obvious choice, being the
  background stage 7 will draw onto. It is also exactly what a *dead panel*
  looks like, so the resting state would have proved nothing to anyone who
  missed the two-second colour cycle. Ending lit means the screen keeps
  answering the question minutes later. Stage 7 clears to black as its first
  act. (Caught after flashing, by trying to describe what to look for.)

**Verified on hardware.** Red, green and blue each filled the whole round
panel in the right order, then it settled on solid blue and stayed there,
with WiFi associating and the usage fetch completing on the same boot. So
SPI and the WiFi radio coexist, which was the actual open question.

### Review pass on the driver — three fixes

A review of the stage-6 diff found three real issues, all fixed:

- **Every SPI transfer's return value was discarded.** `gc9a01.h` promised
  `ESP_ERROR_CHECK` semantics, but that only covered bus setup; the two
  functions that actually put bytes on the wire ignored their `esp_err_t`.
  Not theoretical: `spi_device_polling_transmit` → `spi_device_polling_start`
  → `setup_priv_desc`, and that last one bounce-allocates with
  `heap_caps_aligned_alloc` whenever the source is not DMA-capable. The init
  table is `static const`, so it lives in flash-mapped rodata, which is *not*
  DMA-capable — **all 42 init payloads take that path**. Under heap pressure
  a parameter silently never reaches the panel and the driver still logs
  "init done" over a half-configured display. Both writers now funnel through
  one checked `lcd_transmit`, which records the first failure (sticky, so a
  broken bus cannot print 240 identical lines during one fill), and
  `gc9a01_init` ends with `ESP_ERROR_CHECK(s_err)` — which is what makes the
  header's promise true. Drawing calls deliberately log instead of aborting:
  a desk gadget that panics over one bad row is worse than one showing a
  partial screen.
- **`gc9a01_init()` was not idempotent.** `spi_bus_initialize` returns
  `ESP_ERR_INVALID_STATE` for an already-initialized host, inside
  `ESP_ERROR_CHECK` — so a second call panicked and rebooted rather than
  no-opping. Nothing calls it twice today, but stage 8 adds recovery paths
  where re-initializing the panel is an easy mistake, and the cost of that
  mistake was a boot loop. Now a logged no-op. **Verified by deliberately
  calling it twice on hardware**: warning printed, no panic, display
  unaffected.
- **The `max_transfer_sz` comment was misleading.** It read as a cap of one
  480-byte row; `spi_bus_initialize` actually passes it to
  `spicommon_dma_desc_alloc`, which rounds up to whole DMA descriptors and
  writes the larger value back. Corrected, because stage 7 will size
  multi-row transfers and — before the fix above — exceeding the real cap
  would have failed silently.

**One finding was investigated and deliberately not acted on.** CS sits on
GPIO8, which is an ESP32-C3 strapping pin that must read high to enter
download mode, and this module has an onboard pull-down on CS. The prediction
was that flashing would need the manual button dance. It does not: **five
consecutive `idf.py flash` runs with the panel wired all entered download
mode normally**, so the pull-down is too weak to win at reset. The facts are
still worth having, because they are a credible explanation for the one
`No serial data received` failure recorded at stage 4 — and would explain why
it was intermittent. Documented at the `#define`, with the two cheap outs
(move CS to D10/GPIO10, or leave it unwired — the module works without it,
being the only device on the bus) noted as available if it ever recurs.
Rewiring a proven-good setup to fix a symptom that is not occurring would be
the wrong trade.

## Stage 7 as built

The join: `usage_t` from stage 5, drawn with the panel from stage 6. Three
pieces of new machinery, all small.

- **A font, as reviewable art rather than hex.** `tools/make_font.py` holds
  each glyph as seven rows of five characters and generates
  `main/font5x7.h`; both are committed, so building never needs Python. The
  reason for the indirection is that a wrong bit in hand-written hex is
  invisible until it is on the glass and you are wondering why the 8 looks
  odd — as art, it is wrong in a way you can see. The generator refuses to
  emit a malformed glyph and prints a preview.
- **`gc9a01_fill_rect` and text**, both built on the same address-window
  idea as `fill_screen` — set a rectangle, stream pixels into it. A
  character is one window per glyph, magnified by an integer `scale`, which
  is why one 295-byte table serves every size on screen. `fill_screen` is
  now just `fill_rect` over the whole panel, so there is a single fill path.
- **A layout in named constants**, because the recurring question on a round
  display is "does this still fit inside the circle", and that is far easier
  to answer from a list of rows than from numbers buried in draw calls.

Every fetch outcome now reaches the screen, not just the good one: 200
renders the readings, 503 shows `NO DATA`, a connection failure shows
`NO LINK`, and a bad body shows `BAD DATA`. These are the minimum that stops
the display from *lying* — stage 8 makes them considered states rather than
one-line messages. `CONNECTING` replaces stage 6's resting blue, since a
twenty-second WiFi join behind a blank screen looks broken.

**Cost: 1,552 bytes** for the font, the primitives and the layout together.
Worth recording because font data was flagged earlier as a threat to the
flash budget, and at this size it plainly is not. The budget pressure is
real for *stored images*, which is the deferred face idea, and not for text.

**Verified on hardware, by eye** — the only way it can be. Both readings,
both reset times, the divider and the colour bands, legible across a desk.

### The one real bug: everything was mirrored

The first render came out mirrored left to right. The cause was `madctl`,
corrected from `0x08` to `0x48` (adding the MX column-order bit), and the
interesting part is why it survived six stages undetected: **a solid-colour
fill is symmetric, so no amount of it can reveal orientation.** The
bring-up test proved the colour order — that half was and remains correct —
and was structurally incapable of proving anything about scan direction. The
note in this file claiming `0x08` was "right for this board, no calibration
step needed" has been corrected accordingly.

Two things to take from it. A test proves what it exercises and nothing
adjacent, and it is worth asking what a passing test is *blind* to rather
than only what it covers. And the staged build worked exactly as intended
here: the bug was found the moment the first stage capable of exposing it
ran, with one suspect and a one-byte fix.

### Review pass on stage 7

A review found no correctness bugs in the new drawing logic -- clipping,
buffer sizing, the address-window maths and the centring were all checked and
were right, and regenerating `font5x7.h` from the script produced a
byte-identical file. What it did find was one latent API trap and a cluster of
documentation that this very commit had made false. All fixed:

- **Text that did not fit vanished silently.** `draw_char` skips any glyph
  whose full cell would fall off the panel, so an over-long string lost its
  leading *and* trailing characters and displayed a confidently centred
  fragment. Reachable from network data: reset times are `char[24]`, and the
  contract explicitly anticipates a longer non-English weekday. `draw_centred`
  now truncates deliberately and marks it with `>`. **Verified by modelling
  the arithmetic in Python** rather than by another flash cycle: for every
  length 0-40 at every scale in use, no glyph cell can now fall off the panel,
  and normal strings are positioned exactly as before.
- **`gc9a01_text_width` reports ink; `draw_char` needs the whole cell.** So
  right-aligning at `x = 240 - text_width(...)` silently loses the last glyph
  -- the helper says it fits and the renderer disagrees. Documented at the
  function, since it is a trap rather than a bug.
- **`GC9A01_CHAR_H` must equal `FONT5X7_H`** and nothing said so, which is
  easy to miss because the *width* pair is deliberately unequal (6 vs 5, for
  the spacer). Raising the cell height for line spacing would leave every
  glyph's window under-filled, and the next draw would land inside the
  previous character's window. Now a `_Static_assert`.
- **Four stale documentation claims**, all created by the stage 7 commit
  itself: the driver header still said only `app_main` draws (the fetch task
  draws too, and the safety rests on an unenforced ordering convention);
  `app_main`'s comment still credited the colour cycle with confirming
  `madctl = 0x08`; and `firmware/README.md` kept two copies of the old
  "resting blue" behaviour plus an expected log line that never existed.

The pattern worth noting: the bugs were in the *documentation the change
invalidated*, not in the change. Editing one copy of a claim and missing the
other two is the recurring failure mode in this repo, and it is what a review
catches cheaply.

## Stage 8 as built

The last stage. Stage 7 drew once, at boot; stage 8 is what turns that into
something you can leave on a desk. Three things, per `CLAUDE.md`'s definition
of done, plus one change that only became reachable now.

**A refresh loop, at 45s.** `usage_fetch_task` (one-shot, deleted itself)
became `usage_task` (runs forever). The three traps flagged before starting
were all real and all avoided: the body accumulator is a local inside
`fetch_once`, so `len`/`truncated` cannot survive into the next iteration;
`esp_http_client_cleanup` still runs on every path; and drawing is still
confined to one task.

**WiFi reconnect that reaches the screen.** The retry timer already retried
forever without blocking the event loop — that part needed nothing. What was
missing is that a disconnect was invisible on the glass. Now the loop checks
`WIFI_CONNECTED_BIT` before spending a ten-second HTTP timeout discovering
what the radio already knows, and shows `NO WIFI` rather than `NO LINK` —
which matters, because those two send you to debug different machines.

Retries now back off: 2s for the first three, then 5s, then 30s, reset on
success. The first three stay fast deliberately — the observed boot pattern is
up to three `reason 2` retries before associating, and backing off early would
turn a normal 20-second boot into a minute.

`s_ever_connected` separates "has not associated yet" from "was connected and
lost it". They look identical to the radio, but showing `NO WIFI` during every
ordinary boot would make a working gadget look broken for its first twenty
seconds, so the first case says `CONNECTING`.

**Failure states that degrade instead of blanking.** This is the biggest
behavioural change, and it reverses what stage 7 did. Stage 7 replaced the
whole screen with `NO LINK` on any failure; `ARCHITECTURE.md` asks for the
opposite — keep the last known values, visibly marked. An old number under a
banner saying how old it is stays useful; a screen reading only `NO LINK` has
thrown away the last thing it knew. The words-only screens now appear only
when nothing has ever been fetched.

**Staleness became a decision rather than a printed number.** Graded in three
steps: no banner under 10 minutes, an amber banner with the real age from 10
to 30, and past 30 the numbers themselves go flat grey. The grey-out is the
part worth keeping — a badge says "this might be old", removing the colour
says "this is not a statement about now", and it stops a red 94% alarming
someone about a number that stopped being true an hour ago.

The thresholds are derived, not picked round, and the derivation is the
interesting part. `pc_service` backs off 120s → 240s → 480s → capped 900s on
an upstream 429, so *one* 429 delays the next successful poll to t=360s and
two in a row to t=840s. Data can therefore reach 6 and 14 minutes old with
nothing whatsoever wrong. The first thresholds proposed for this stage were 5
and 15 minutes, which would have fired on both — the same cry-wolf failure
that pushed the poll interval from 60s to 120s at stage 6. Working the backoff
through *before* writing the code is what caught it.

Note also which half of the age computation does the work during an outage.
`now_epoch - updated_epoch` needs no clock on the chip, but it freezes the
moment the service stops answering — no new `now_epoch` arrives, so a dead PC
would look eternally fresh. Adding `esp_timer` elapsed-since-fetch is what
keeps the age honest, and it is the only reason the banner can count upward
while the link is down.

**No more full-screen repaints.** Repainting 240x240 every 45 seconds is a
black flash you cannot help watching, and it happens whether or not a digit
changed. The firmware now keeps a model of what it last drew (`s_slots`) and
touches only rows that differ; in steady state a refresh writes zero pixels.
Rows rather than glyphs, because every line is centred — a shorter string
starts further right and would strand the tail of the previous one unless the
whole row band is cleared first.

**SPI clock raised 10 MHz → 40 MHz**, in its own commit. The comment on
`SPI_CLOCK_HZ` had said since stage 6 to do exactly this: later, on its own,
with the screen already working, so a failure would have one suspect. It works
at 40 with no visible artefacts on this wiring.

### Layout change: the banner row moved from y=205 to y=202

Small, but it is the kind of thing that is invisible until it bites. The
banner is the one line that says whether to believe the numbers above it, and
it sits low on a *round* panel where width runs out fast. At y=205 the bottom
row of pixels falls where the circle is 135px wide — eleven characters at
scale 2. `"BAD DATA 12M"` is twelve. Three pixels of headroom (y=202 puts the
bottom row where the circle is 146px) bought the longest string the screen
needs to say, and `BANNER_MAX_CHARS` now records the budget rather than
leaving it to be rediscovered.

Worth noting the general shape: on a round display, "does it fit" has a
different answer at every row, and the answer at the *bottom* of a glyph is
the one that matters.

### Verified on hardware

Four runs, all against the real chip and real `pc_service`.

**Steady state, 150s.** Fetches at t=23s, 68s, 113s — 45 seconds apart to the
second. Free heap 191868 → 191884 → 191884 bytes: flat, which is the number
that matters now that this code runs forever. The 7-day reading changed 10% →
11% mid-run, so the partial-redraw path was exercised on a real change rather
than only on unchanged data.

**Reconnect backoff.** Two `reason 2` disconnects at boot, both logged
`retrying in 2000 ms` (retries 1 and 2, inside the fast band), associated at
t=22.3s. Behaves as before, which was the intent — the backoff only changes
the slow cases.

**Service outage and recovery, 185s.** Good fetch at t=4.9s (age 111s), then
`pc_service` stopped. Failures at t=60s and t=115s, both
`ESP_ERR_HTTP_CONNECT`, both keeping the numbers and raising the banner.
Service restarted; recovered at t=160s with fresh data (5h 17% → 19%) and the
banner cleared on its own. Heap across the whole run: 191996 → 192556 →
192552 → 192224. No leak on the failure path either.

One timing detail worth writing down: a failing cycle takes **55 seconds, not
45** — the interval is measured from the end of the previous attempt, and a
connection to a dead port burns the full 10-second timeout first. That is the
intended pacing (it spaces retries out rather than bunching them), but it does
mean the banner's age counts up in ~55s steps during an outage.

**Both staleness tiers, 235s**, using the new `stub_stale.py`. Three fetches
at a declared age of 900s (badge tier: amber banner, numbers keep their
colours), then two at 2400s (dead tier: red banner, numbers grey). Heap stable
across all five. This is the branch that would otherwise take half an hour of
waiting to reach honestly, which is exactly why the stub exists.

**What none of the above can prove** is what is actually on the glass. SPI
writes are unacknowledged, so every log line here prints identically into a
panel that is unplugged — and identically into one drawing everything
mirrored, which is precisely how stage 7's bug survived six stages. The visual
check is a human one, and it is the only check in this stage that a machine
cannot make.

**Since confirmed by eye, and the partial redraw is the part that needed it.**
Watched live across a 65% → 66% change: the digit changed with no flash, no
flicker on the surrounding lines, and no disturbance to the 7-day block.

Worth recording precisely what that demonstrates, because the observation is
easy to over-read. The reported impression was "only the 5 changed to 6", but
the model works at *row* level, not digit level: the whole "66%" string was
repainted, and the leading 6 simply landed in the same pixels as the old one,
which is indistinguishable from not touching it. The 7-day rows wrote genuinely
zero pixels, because nothing about them differed. There is also a real black
clear of the changed row band before the text goes down — 240x35 at scale 5,
about 3.4 ms at 40 MHz — and it was not perceptible, which is exactly why the
row is the right unit. Stage 7 would have blanked all 240x240 and redrawn seven
lines for that one digit.

Getting a changed value to observe at all meant deliberately spending tokens
against the account until the 5-hour window moved, which turned out to be the
only practical way to exercise this on demand: across most polls the served
numbers are byte-identical (see the 120s note in the deferred list), so the
interesting path never runs by itself.

**The boot colour cycle was confirmed by eye in the same session**, and the
pairing is the point. Neither observation is sufficient alone: the colour cycle
proves the BGR bit -- red, green and blue arrive as red, green and blue -- but
a solid fill is symmetric and therefore blind to orientation, which is exactly
how the mirroring survived to stage 7. The digits prove orientation (a mirrored
"66" does not read as a number) but say little about colour fidelity. Together
they cover both halves of `madctl = 0x48` with tests actually capable of
exposing a fault in each.

**The failure banners were then confirmed on the glass too, completing the
list.** Three states, watched live:

- **`NO LINK`.** The real service was stopped. The banner appeared within a
  cycle with the numbers untouched above it, and the age incremented on the
  following cycle while nothing else on screen moved. Restarting the service
  cleared the banner unattended; the recovery landed 24 s later, because the
  chip's 45 s cycle happened to fall there. The 503 window on restart was one
  second wide and the chip did not land in it, so `NO DATA` never appeared --
  correct, if slightly disappointing.
- **The two staleness tiers**, using `stub_stale.py`, and the method is worth
  keeping because it is what made the result unambiguous. Rather than showing
  the stale state alone, the *same payload* was served twice with only its age
  changed: at 60 s, 73% drew amber and 88% drew red with no banner; at 2400 s,
  the identical digits drew flat grey under a red `STALE 40M`. Every visible
  difference is therefore attributable to the age and to nothing else.

One detail from that run is a better proof than it looks: the stub reported
`stale flag: False` both times. So the badge and the grey-out were driven
purely by the chip's own `now_epoch - updated_epoch` arithmetic, with the
service making no claim of staleness at all. The two signals really are
independent, and the age half works on its own.

**All six visual checks are now done.** Nothing on stage 8's display
behaviour rests on inference.

The gauge arcs added later were verified the same way, and are now complete
too. Confirmed on the panel: the first paint, the 6-pixel thickness, both arcs
in the right halves with the right colours, and that an update no longer breaks
the ring after the full-width-clear fix.

The two hard cases were then forced with `stub_stale.py` rather than waited
for, and both passed:

- **100%**, the exact 180° cap in `gc9a01_fill_arc`. Confirmed twice: once from
  a stub, and once from real data when the account genuinely reached 100%. The
  half closes cleanly with no inversion and no spill past nine or three. Both
  gauges at 100% together close the full ring, meeting at nine and three
  without overlapping — which is the claim that the two halves tile the circle.
- **The shrink path**, taking both gauges from 100% straight to 0%. The entire
  ring is erased in one update, with no surviving fragments, and the numbers
  re-band to green. This also exercises the full-repaint branch rather than the
  delta one, since 100% → 0% crosses two colour bands.

Nothing about the display now rests on inference.

### New tool: `pc_service/tools/stub_stale.py`

A companion to the existing `stub_503.py`, and for the same reason: the
interesting branches are the ones that are hard to reach by waiting. It serves
a well-formed 200 whose `updated_epoch` is however far in the past you ask
for, so the firmware takes exactly the path it would take against a genuinely
stalled service.

```
python tools/stub_stale.py 900        # 15 min old  -> amber STALE badge
python tools/stub_stale.py 2400       # 40 min old  -> numbers go grey
python tools/stub_stale.py 60 --flag  # fresh, but flagged stale by the PC
```

The `--flag` form covers the case the age cannot: `stale: true` and the
computed age are independent signals, and the flag alone has to be enough to
raise the badge — that is the case where `pc_service` is alive and its own
upstream call failed.

### Review pass on stage 8

Seven findings, all verified against the code before being accepted, all
fixed. The state machine itself came through clean — boot, association
failure, mid-run drop, recovery, outage and both staleness tiers were traced
and were right, as were every new buffer size and the row-band geometry. What
the review found instead was one operational bug in the new tooling, two
latent ways for the WiFi recovery to stop recovering, and three claims that
were not true.

**The one that mattered: `stub_stale.py` could bind beside a running
service.** `service.py` sets `allow_reuse_address = sys.platform != "win32"`
for a documented reason — on Windows `SO_REUSEADDR` lets a second process bind
a port that is *already actively listening*. The new stub used a plain
`HTTPServer` and inherited the flag, so it would have bound alongside the real
service and Windows would have split incoming connections between the two.
The chip would then show real data on some polls and stubbed data on others,
which reads as the firmware flapping rather than as the obvious mistake it is.

Worse, the stub's docstring asserted the opposite — "the second one to start
fails loudly rather than sharing it" — and that claim was copied into
`STATUS.md` and `firmware/README.md`. True of `service.py`, false of the stub.
Both stubs now use a `StubServer` subclass carrying the same platform-
conditional flag, which makes all three statements true; **verified by
starting the stub beside the running service and getting WinError 10048.**
`stub_503.py` had the same flaw and was fixed with it.

This is the third time in this repo the bug has been in *documentation the
change invalidated* rather than in the change, and the second time a review
caught it. The stage 7 notes above say the same thing. Evidently it is the
characteristic failure here, not a one-off.

**`++s_retry_count` was a side effect inside `ESP_LOGW`.** It expands to
`if (LOG_LOCAL_LEVEL >= ESP_LOG_WARN) ...`, so lowering the build's log level
past WARN deletes the increment along with the message. Harmless while the
counter only fed a printed number — which is why it survived from stage 3 —
and *not* harmless from the moment stage 8's `reconnect_delay_ms()` started
reading it: the tally would sit at zero, the backoff would flatten to a fixed
2s, and an absent router would be probed eighteen hundred times an hour. The
backoff would have disappeared silently, from a config change in a different
file. Latent today (`CONFIG_LOG_DEFAULT_LEVEL_INFO`), and now hoisted to its
own line.

Worth generalising: the fix at stage 8 did not introduce this line, it made an
existing line load-bearing. Adding a reader to an old variable is a change to
that variable's requirements.

**A synchronous `esp_wifi_connect()` failure ended the retry chain.** Every
retry is driven by `WIFI_EVENT_STA_DISCONNECTED` re-arming the timer — but
that event only exists if an association was actually attempted. If the call
failed outright, nothing was posted, nothing re-armed, and the chip would sit
on `NO WIFI` until power-cycled, with nothing in the log. That is a direct
contradiction of the one thing stage 8 promises about WiFi. The callback now
logs and re-arms itself, bumping the tally so a persistent failure backs off
rather than spinning.

**A link dropping mid-request was reported as `NO LINK`.** WiFi is checked at
the top of the loop, but the request that follows can spend a full ten seconds
in timeout. A drop inside that window landed on the connect-failure branch and
blamed the PC — the exact misdiagnosis the `NO WIFI` / `NO LINK` split was
added to prevent. That branch now re-checks the bit before naming a culprit.

**`draw_centred`'s truncation guard measured the square, not the circle.**
`GC9A01_WIDTH / cell` is the right answer on a rectangular panel. On this one
the usable width depends on the row: at `ROW_7D_RESET` about 190px are inside
the bezel, so the guard permitted 20 characters where 15 are visible. The
failure it allowed is the one the guard exists to prevent — the `">"` marking
a deliberate truncation would itself have been drawn outside the circle, so an
over-long string would have lost glyphs off *both* ends with nothing on screen
to say so. Reachable from network data, since reset times come from the PC and
the contract allows a longer non-English weekday.

Two things to keep from this. Stage 8 did the circle arithmetic carefully for
the banner row and left every other row on the square assumption — doing the
sum once does not mean it has been done. And the correct measurement is not
simply "the bottom of the glyph": for text *above* centre the top row is the
narrow one, so it is whichever edge is further from the middle. The fix takes
the max of the two, needs no floating point (a 120-iteration integer square
root), and independently reproduces `BANNER_MAX_CHARS = 12`, which is a
pleasant check on both pieces of arithmetic.

**The 40 MHz comment claimed spec compliance it does not have.** It said 40
MHz "is what most GC9A01 boards are specified for". The controller's datasheet
gives a 100 ns minimum serial write cycle — that is 10 MHz. 40 MHz is a
widely-used *overclock* that works on this board and has been checked, not a
guarantee about any board. In a project whose stated goal is being understood,
that was the one claim a reader would carry away wrong.

The reviewer added a genuinely useful second point: `PIN_SCLK`/`PIN_MOSI` are
GPIO4/5, which are SPI2 IOMUX pins for HD and WP but **not** for CLK and MOSI,
so these signals route through the GPIO matrix — whose documented ceiling for
an SPI master is exactly 40 MHz. So this sits at the limit with no headroom:
going faster would mean rewiring to the IOMUX pins, not editing a constant.

**Also fixed, from the same review:** both stubs are now `ThreadingHTTPServer`
with a 30s handler timeout. They serve HTTP/1.1, so keep-alive is the default,
and on a single-threaded server one parked connection blocks every other
client — meaning the obvious way to check a stub (open it in a browser) would
have held its only thread, starved the ESP32's next poll, and produced
`NO LINK`: the opposite of the branch the tool exists to exercise.

**Re-verified on hardware after the fixes.** Clean build, no warnings, 7% of
the app partition still free. Three refreshes at t=4.3s, 49.4s, 94.5s — the
45s cadence intact — with the heap flat at ~192 KB and the live readings
moving (5h 56→57%, 7d 14→15%), so the partial-redraw path ran against real
changes rather than only against identical payloads. The reworked stub was
confirmed serving a 2400s age with `stale: true`, and confirmed refusing to
start beside the running service.

## The gauge arcs (after stage 8)

Requested from a photo of the working display with two arcs drawn on it in
marker: one per window, following the rim, top half for the 5-hour and bottom
half for the 7-day. The question asked was whether there was room for it.

There was, easily, and the honest answer was that space was never the
constraint. This is the procedural option from the deferred face analysis,
costed there at roughly zero: **1,568 bytes of flash and no RAM at all**,
against ~70 KB free. What it actually cost was a layout change and one real
bug.

### Geometry, as specified

Both arcs sweep **clockwise**, each anchored where its half of the ring
begins: the 5-hour at nine o'clock filling over the top, the 7-day at three
o'clock filling under the bottom. Each is capped at a half turn — 1.8° per
percent — so they tile the ring exactly and can never overlap. At 100% and
100% they meet at nine and three and close the circle.

No unfilled track behind either arc: an empty rim reads as zero perfectly
well, and a track would compete with the numbers. Each arc takes its colour
from the same `usage_colour()` its number does, including the flat grey of
data too old to be a claim about now — an arc still green beside a grey number
would be the display contradicting itself.

Six pixels thick, radii 112–118. It was twelve first, which read as a pie
chart rather than an instrument.

### Filled as a region, not stroked as a path

The implementation choice worth keeping. Sweeping an angle and plotting points
along it needs sub-degree steps to avoid gaps at this radius — one degree is
two pixels at r=118 — and still leaves ragged ends where the arc stops.

Testing each pixel of the annulus for membership instead cannot leave a gap,
because there is no step size to get wrong. And the test is cheap: for a sweep
of at most 180°, a point is inside the wedge exactly when it is **clockwise of
the start ray and anticlockwise of the end ray**, and "clockwise of" is the
sign of a 2D cross product. So it is two integer multiplies per pixel — no
`atan2`, no floating point, and trigonometry used exactly twice per call to
turn the two angles into direction vectors. Pixels are emitted as horizontal
runs, so a half-ring is a few hundred short transfers rather than thousands of
single-pixel ones.

The 180° cap is not incidental and is enforced in the driver: past a half turn
the two half-plane tests describe the *complement* of the wedge, and the arc
inverts. Anything larger has to be two calls.

Trig comes from a 91-entry fixed-point sine table (sin × 1024, 0–90°), with
quadrants handled by sign. Unlike the font, it is written out rather than
generated: a wrong glyph is a design mistake you catch by eye, a wrong sine is
arithmetic you catch by the arc landing in the wrong place.

### The banner had to move

The arcs took the space it was in. At the bottom of a round panel the circle
has narrowed to 140 px on the banner's last row, and the ring reaches x=50–61
there — straight through text spanning 49–191. Thinning the ring does not
escape it, because what matters is the ring's *inner* edge: at 6 px it sits at
x=61, at the original 12 px it sat at x=73, and both are well inside the
banner. The ring must be near the edge to read as a gauge and the banner must
be wide to say anything useful. Shortening the banner to
clear it caps it at **seven characters**, which loses the age, and the age is
the part that earns its place.

So the banner moved to the vertical centre, where the circle is widest and
twelve characters clear the ring by 34 px each side, and it now **shares a
slot with the divider**: hairline when the data is fine, banner text when it
is not. That turned out better than the original, not merely acceptable — the
banner now *displaces* something familiar rather than appearing below
everything, which is harder to overlook.

### The one real bug: the arcs were correct until the first update

Reported from the panel: right after a reboot it rendered correctly, then
developed gaps. That symptom names the cause almost by itself — **correct on
first paint, wrong on first update** points at the incremental path.

`set_slot` cleared its row band with a full-width `fill_rect`, all 240 px.
The ring is an annulus at the rim, so that clear cuts through **both** of its
arms on every row it covers. On a full repaint the arcs are drawn last, so
nothing has cut them yet and the first frame is perfect. On an update, one
text row repaints, takes two bites out of the ring, and the delta-wedge redraw
never repairs them — it only ever paints the newly-swept degrees. The tall
`85%` band alone is 35 px, which was the large break in the photo.

The fix is that the clear should never have been full width. Row clears now
stop at `CONTENT_R`, two pixels inside the ring, and text is fitted to the same
radius — so as far as anything but the ring is concerned, the display is
smaller than it looks. Thinning the arc to 6 px helped rather than cost:
`5-HOUR` now gets 10 characters of room where it had 8.

Two things worth generalising from it:

- **A full-screen repaint hides ordering bugs that an incremental one
  exposes.** Everything was drawn in the right order on the path that redraws
  everything; only the partial path could reveal that two subsystems disagreed
  about who owns which pixels.
- **Radii that are only correct relative to each other belong next to each
  other.** `ARC_R_IN` and `CONTENT_R` are now adjacent in the source with the
  reason written between them, because the failure mode is someone widening a
  clear later and reintroducing exactly this.

## The 93-minute soak

Run 13:59–15:32 with the serial port logged to a file with wall-clock
timestamps, which is the only way to line the chip's own millisecond stamps up
against pc_service's log and against the window reset.

**Health: clean.** 71 successful fetches; free heap between 190,312 and 192,344
bytes with first 191,980 and last 192,220 — flat across successful fetches,
failed fetches, a full disconnect/reconnect cycle and a service outage. No
panics, no reboots. The single `rst:0x` in the log is `USB_UART_CHIP_RESET` at
the moment the logger opened the port, which is expected rather than a fault.
That is the closest this build has come to answering the "flat over four
minutes is not flat over four days" caveat.

**Three failures, none of them firmware bugs, and each one instructive.**

*A real WiFi collapse, unplanned.* RSSI fell from a steady -65 dBm to -87 over
about two minutes, producing three `ESP_ERR_HTTP_EAGAIN` timeouts and then
fourteen disconnects (reason 201) as association was lost entirely. The backoff
behaved exactly as designed — 5s tier, then 30s tier — and it reassociated
unattended with no intervention. Signal later recovered to -63 dBm on its own.

This exposed a genuine limitation worth fixing one day: **the `NO WIFI` /
`NO LINK` split keys off association, not usability.** A link that is
associated but too weak to carry data keeps `WIFI_CONNECTED_BIT` set, so the
chip reports `NO LINK` and the log sends you to debug the PC — the wrong
machine. The RSSI is already being read in the heartbeat ten seconds later;
including it in that diagnostic would have said "wifi is up but at -87 dBm"
and pointed straight at the antenna.

*The PC's address moved, live.* During the same disturbance DHCP reassigned the
PC from .88 to .87, and every fetch failed with `ESP_ERR_HTTP_CONNECT` while
WiFi was perfectly healthy. This is the exact failure the docs predicted would
be the mystery one months later, and the firmware's own error message named the
cause correctly — third in its list of suspects. **A DHCP reservation is no
longer a nice-to-have.**

*The window reset fired* at 15:31:23: 5h 100% → 0%, and `resets_at` came back
as `None` — the contract's documented null case occurring naturally in the
wild, rendered as `--`. The chip-side shrink was not captured because fixing
the address required a reflash, which reboots; the shrink was reproduced
deliberately with the stub instead.

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
- **`madctl` is `0x48` (MX | BGR), and the story of how it got there is the
  useful part.** It was `0x08` from bring-up through stage 6, on the evidence
  of a red/green/blue fill test that showed true colours — so the colour half
  of that finding was right and still stands. But **a solid fill is symmetric,
  and therefore blind to orientation.** The first asymmetric thing ever drawn
  on this panel — text, at stage 7 — came out mirrored left to right, which is
  the MX bit (`0x40`). The earlier note here said "right for this board, no
  calibration step needed"; that overstated what the test could possibly have
  shown, which is the thing worth remembering. A test proves what it exercises,
  and nothing adjacent.

  The full byte: `0x80` MY flips vertically, `0x40` MX flips horizontally,
  `0x20` MV rotates 90°, `0x08` selects BGR. If orientation ever needs
  revisiting, those four bits are the whole search space.
- **The panel runs fine at 40 MHz SPI** on this wiring, raised from the
  bring-up value of 10 MHz at stage 8. No tearing, snow or colour faults over
  the jumper wires in use. If it ever misbehaves after a rewiring, putting
  `SPI_CLOCK_HZ` in `gc9a01.c` back to 10 MHz is the one-line thing to rule
  out first — long jumpers are the usual reason a panel that works at 10 does
  not work at 40.

  Two caveats, both from the stage 8 review and both worth keeping. **40 MHz
  is an overclock, not a spec figure:** the GC9A01 datasheet gives a 100 ns
  minimum serial write cycle, i.e. 10 MHz, so this is a measurement about this
  board rather than a guarantee about any board. And **there is no headroom
  above it** — `PIN_SCLK`/`PIN_MOSI` are GPIO4/5, which are SPI2 IOMUX pins
  for HD and WP but not for CLK and MOSI, so these signals go through the GPIO
  matrix, whose documented maximum for an SPI master is exactly 40 MHz. Going
  faster is a rewiring job, not a constant.
- **Free heap sits at roughly 191-192 KB with the refresh loop running**, and
  stays there across successful fetches, failed fetches and recoveries alike.
  That is the number to watch: this is the first code in the project that runs
  forever, so a leak of a few hundred bytes per fetch is invisible in one
  request and fatal within a day. A steadily falling number means a missing
  `esp_http_client_cleanup` or `cJSON_Delete`.
- **A failing refresh cycle takes 55 seconds, not 45.** The interval is
  measured from the end of the previous attempt, and connecting to a dead port
  burns the full 10-second HTTP timeout first. Intended — it spaces retries
  rather than bunching them — but it does mean the banner's age counts up in
  ~55s steps during an outage, not 45s ones.
- **On a round panel, "does it fit" has a different answer at every row**, and
  the answer at the *bottom* of a glyph is the one that decides. The banner row
  moved from y=205 to y=202 at stage 8 for exactly three pixels of headroom:
  at 205 the budget is eleven characters and `"BAD DATA 12M"` is twelve.
  `BANNER_MAX_CHARS` records the budget so it does not have to be rediscovered.
- **A hostname in `secrets.h` does not work on this network, and it was
  tested.** The chip fails with esp-tls error `32769`
  (`CANNOT_RESOLVE_HOSTNAME`) — it never gets an address at all. The router
  *does* resolve the bare name correctly when queried directly, so the name is
  fine; the likeliest cause is that the chip's DHCP-supplied DNS server is not
  the router but a public resolver, which knows nothing of LAN names. No
  suffixed form (`.lan`, `.home`, `.local`, …) resolves either. Beware
  `Resolve-DnsName` without `-DnsOnly -NoHostsFile`: it answers from Windows'
  own local resolution and will tell you the name works when it does not.
- **If the host PC ever moves from cable to WiFi, check AP isolation first.**
  Today the chip is wireless and the PC is wired, so traffic crosses the
  router's bridge. Wireless-to-wireless is the case client isolation blocks,
  and no firewall rule or address fix works around it. Test with a phone on the
  same WiFi hitting `http://<pc-ip>:8734/usage`. A DHCP reservation still works
  over WiFi, but it is keyed on the **WiFi adapter's MAC**, which is a
  different MAC from the Ethernet one — reserve the interface actually in use.
- **Text on this panel is fitted to `CONTENT_R`, not the panel edge.** The
  gauge ring owns the rim, so anything that is not the ring — glyphs, the
  opaque background behind them, and every row clear — must stay inside that
  radius. Widening a clear back to the full 240 px reintroduces the bug that
  ate the arcs. `ARC_R_IN` and `CONTENT_R` are deliberately adjacent in the
  source because they are only correct relative to each other.
- **`gc9a01_fill_arc` caps its sweep at 180°, and that is load-bearing.** Its
  membership test is two half-plane comparisons, which describe a wedge only
  up to a half turn; past that they describe its complement and the arc
  inverts. Anything larger has to be drawn as two calls. The two gauges are
  half-turns by design, so this never binds in practice.
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
- **The app is using most of a 1 MB partition, and the margin is shrinking.**
  Stage 4 built to 0xe3500 (11% free); stage 5 to 0xe64b0 (10% free), cJSON
  costing about 12 KB; stage 6 to 0xed7e0 (**7% free, about 74 KB**), the
  display driver costing about 29 KB — nearly all of that being the
  `spi_master` driver rather than our own code, which is a few hundred lines.
  WiFi plus the HTTP client is still most of the total. Stage 7 adds font
  data and drawing primitives on top of this. **If it stops fitting, the fix
  is a custom partition table, not code golf** — there is a spare ~1 MB of a
  4 MB flash sitting unused in the default table.
- **A 429 now recurs with a single service instance at one poll per minute.**
  This contradicts the earlier note that 1/min ran fine, and it met the
  written trigger for revisiting `POLL_INTERVAL_SECONDS`. Observed over one
  session: roughly one poll in three or four came back rate-limited, each
  time backing off 120-240s and recovering. Plausible reason it appeared now
  and not before: Claude Code itself calls this same endpoint to power its
  own `/status`, so during an active session the service is not the only
  caller competing for that limit. Nothing broke — the backoff did its job
  and the chip was served real numbers flagged `stale: true` — but the poll
  interval should probably go up (120-180s), since the data moves a couple
  of percentage points over ten minutes and nothing is gained by asking
  more often.
- **The `stale: true` path has now been seen end to end on hardware**, as a
  side effect of those 429s rather than by design: the chip parsed
  `stale: YES` with `data age: 164s` and printed it correctly. That is the
  third of the three response shapes, so all of them have now been exercised
  for real rather than only in code review.
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

**The build order is finished.** Every stage in `CLAUDE.md` is done and
verified on hardware, and the definition of done is met: the chip shows both
percentages and their reset times, refreshes itself every 45 seconds, and
degrades visibly rather than silently when the service or the network drops.

So there is no next stage — only candidates, none of them required:

- **The expressive face.** Deferred since stage 6 with the condition "revisit
  once the display works and the real free-flash number is known". Both are now
  true: the display works, and about **68 KB** remains (`0x110b0`, 7% of the
  partition). But the arcs have since answered most of what made the idea
  attractive — a continuous quantity carrying precision while colour carries
  the band is now built and working — so what is left to decide is whether an
  expressive character adds anything on top. Worth answering before building.
  See the deferred list below.
- **A DHCP reservation for the PC. This is now the top item, not a
  precaution.** DHCP moved the PC from .88 to .87 during the soak and broke the
  link exactly as predicted. Putting a hostname in `secrets.h` instead was
  tested and does not work here — see the facts below. A reservation is a
  router change, not a code one, and it is the only zero-code fix left.
- **Starting `pc_service` automatically** on login or as a service. It is
  started by hand today, which is fine while the project is being worked on and
  is the main reason the screen says `NO LINK` between sessions.
- **A long soak.** The longest continuous run so far is a few minutes.
  Nothing suggests a problem — the heap is flat and the failure paths recover
  — but "flat over four minutes" and "flat over four days" are different
  claims, and only one of them has been made.
- **Skipping the row clear when the new text covers the old.** `set_slot`
  always clears the full row band before drawing, because a shorter centred
  string would otherwise leave the tail of a longer one beside it. When the two
  strings are the same length — which is the common case, "65%" to "66%" — the
  clear is unnecessary: `gc9a01_draw_text` paints an opaque background, so it
  overwrites in place. Skipping it would remove the ~3.4 ms blank entirely.
  Deliberately not done: that blank is already imperceptible (confirmed by eye),
  so this would add a special case to optimise something nobody can see, and
  the failure mode if the condition is ever wrong is a stranded fragment on
  screen — exactly the class of bug this project keeps trying to design out.

`pc_service` must be **running** for any firmware work.

## Deferred, deliberately

- **Cross-platform preflight** (print LAN IPs, the exact `secrets.h` URL,
  firewall diagnosis on startup). Proposed, not built — `pc_service/README.md`
  covers the same ground in prose. The trigger written here was "revisit at the
  polish stage if the chip is hard to debug"; it wasn't, so this stays unbuilt.
  The one real gap it would close is a stale `PC_SERVICE_HOST` after DHCP moves
  the PC, and a router reservation closes that more cheaply.
- **Raising the SPI clock from 10 MHz toward 40** — listed as a candidate
  before stage 8 and **done during it**, in its own commit, per the instruction
  the code comment had carried since stage 6: on its own, with the screen
  already working, so a failure would have one suspect rather than two. No
  artefacts at 40 MHz on this wiring.
- **`POLL_INTERVAL_SECONDS` was raised from 60 to 120** — its written trigger
  ("raise it only if 429s recur with a single instance") fired at stage 6.
  Measured over an hour at 60s: 13 of 45 polls refused, in a metronomic
  two-ok-then-one-429 cycle. The next step if they reappear is 180s; the real
  limit remains unknown and undocumented.

  Worth keeping the reasoning, because the obvious motive is the wrong one.
  The problem was never freshness — the 120s backoff after each 429 meant the
  real cadence was *already* one successful poll every ~111s, so 120s changes
  almost nothing about how current the data is. The problem was that a 429
  makes the service serve its last good payload with `"stale": true`, so a
  third of responses flagged perfectly good two-minute-old data as stale.
  Stage 7 draws that flag. An indicator that cries wolf a third of the time
  is one you learn to ignore, and then it fails to tell you the thing it
  exists for — that the PC is asleep or the service is dead.

  **Verified after the change: 7 consecutive polls over 13 minutes, zero
  429s**, against roughly 2 rejections that the old cadence would have
  produced in the same span. The run also settled the freshness question by
  accident — the served numbers were identical across six of those seven
  polls (5h 53%, 7d 6% for twelve minutes straight), so even 120s is asking
  faster than the data changes. Caveat kept deliberately: this is one
  account over one hour during an active Claude Code session, so it is a
  step that fits the evidence, not a measured limit.
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
  115,200 bytes**, and free space shrank as stages landed — about 103 KB after
  stage 5, about 74 KB after stage 6, about 70 KB after stage 8, and **about
  68 KB now that the gauge arcs have landed** (`0x110b0`, 7% of the app
  partition). So one full-screen stored frame does not fit at all, and literal
  GIF playback is out unless the partition table changes. A GIF
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

  Stage 8 has now built those fallbacks *as* text banners (`NO LINK 3M` and
  friends), deliberately, so that stage stayed verifiable against the brief
  rather than mixing a redesign into it. That makes the face a layout revision
  rather than an addition, and the banners are the thing it would replace.

  **The gauge arcs have since taken part of this idea's ground, and settled
  its main open question.** "Colour can carry the coarse band while a
  continuous quantity carries the precision" is now built and working — that
  is exactly what the arcs do, and they do it without a face. So what is left
  of the face proposal is the *expressive* part only: whether a character
  reading the worse of the two windows adds anything the arcs and colour bands
  do not already say. Worth answering honestly before building it, because the
  arcs have made the glanceability argument on their own.

  The procedural route the analysis recommended is also no longer theoretical.
  `gc9a01_fill_arc` and the fixed-point sine table exist, cost 1.5 KB
  together, and would be most of what a procedural face needs.

## Housekeeping

**Near-miss worth remembering:** a real WiFi SSID was written into this
tracked file as an incidental detail while documenting the WPA3 finding, and
`SECRETS.md`'s pre-publish checklist is the only thing that would have caught
it. Secrets don't only leak through code — prose notes about *debugging* are
the easy path. When writing up a network finding here, describe the property
(WPA3-SAE, channel, RSSI) and never the identifier. The same goes for real
LAN addresses and local filesystem paths: name the role, not the value.

`pc_service` is **not running** — it is stopped at the end of each session.
Restart it before any firmware work from stage 5 onward, which is now all of
it:

```
cd pc_service && python service.py
```

Confirm only one instance runs; a duplicate launch fails loudly at bind time
rather than silently double-polling. It polls every **120s** (raised from 60s
after persistent 429s — see the deferred list), so the first reading can take
up to two minutes to appear; until then `/usage` answers 503 and the chip
shows `NO DATA`, both of which are correct behaviour rather than faults.

**The chip keeps running whether or not the service does.** With the service
stopped it keeps the last numbers it fetched and raises a `NO LINK` banner
under them, with the age counting up in ~55-second steps; past thirty minutes
the numbers themselves go grey. That is the expected picture between sessions
and not something to debug — and it is the honest reason the screen is not
blank overnight: those numbers are hours old, and the display says so.

`pc_service/tools/` holds two throwaway stand-ins for reaching the failure
branches on demand rather than by waiting: `stub_503.py` (the "no data yet"
state) and `stub_stale.py <seconds> [--flag]` (data of any chosen age, and the
`stale` flag independently). Stop the real service first — both bind 8734, and
the second to start fails loudly (`WinError 10048`) rather than sharing the
port. That is not automatic: it works because the stubs carry the same
`allow_reuse_address = sys.platform != "win32"` that `service.py` does, without
which Windows lets a second process bind an already-listening port and splits
connections between them. Run them with the same `python.exe` the real service
uses, since the Windows firewall rule is per-program.
