# firmware — the ESP32-C3 side

ESP-IDF project for the Seeed XIAO ESP32-C3. Standard ESP-IDF workflow —
target `esp32c3`, build/flash/monitor from the command line or the VS Code
ESP-IDF extension. Nothing outside ESP-IDF is required: no external
components, no `managed_components/`.

Read `../CLAUDE.md` for the brief and `../ARCHITECTURE.md` for the design
before changing anything here.

**Open VS Code on this folder (`firmware/`), not the project root.** The
ESP-IDF extension treats the open workspace folder as the project root and
looks for `CMakeLists.txt` there; the parent folder holds only docs.

## First-time setup

The build **will not compile** until you create your own `secrets.h` — this
is deliberate, so a missing config fails loudly at build time instead of
mysteriously at runtime:

```
copy main\secrets.h.example main\secrets.h     REM Windows
cp main/secrets.h.example main/secrets.h       # Linux
```

Then open `main/secrets.h` and fill in your real WiFi name and password, plus
`PC_SERVICE_HOST` / `PC_SERVICE_PORT` — the LAN address of the machine running
`pc_service`. That file is gitignored and never committed; `secrets.h.example`
(fake values) is the committed template. See `../SECRETS.md`.

Note the ESP32-C3 is **2.4 GHz only**. If the router publishes 2.4 and
5 GHz under one name, that is the first thing to suspect on a failure.

## Where the configuration comes from

As of the `wifi-provisioning` branch the four settings — WiFi SSID and
password, and `pc_service`'s host and port — are read at **runtime**, not
compiled in. Each one is taken from NVS if present, and otherwise from
`secrets.h`, **per value rather than all-or-nothing**. The `config:` lines
printed at boot say where each one came from, and are the first thing to read
when the chip is talking to the wrong place:

```
I (404) config:   ssid     "your-network"   (from secrets.h)
I (414) config:   host     "192.168.1.50"   (from nvs)
```

`secrets.h` is therefore **optional** — the firmware builds and links without
it. It is not yet possible to provision a chip that has no `secrets.h`,
though: the setup portal serves its form but does not write to NVS yet, so
until that lands the only alternative is a hand-generated NVS image
(`nvs_partition_gen.py`). The password is never logged, only its length.

## Setup mode (the button on D1)

Hold the **setup button on D1** for three seconds (see `../ARCHITECTURE.md`
for the wiring). Short taps are ignored deliberately — a desk object that
reconfigures itself when brushed is a bad desk object.

The chip then leaves your network and becomes its own hotspot. The screen
shows everything needed to reach it:

```
        SETUP
        WIFI
   TOKEN-MON-XXXX          <- join this network (XXXX identifies the board)
        PASS
      XXXXXXXX             <- with this password (new every time)
     192.168.4.1           <- then open this in a browser
    HOLD TO EXIT
        5 MIN              <- it ends by itself after this
```

The form comes up pre-filled with the current settings, with the WiFi password
field blank — leave it blank to keep the one already stored. **This build does
not save anything yet**: it parses the submission, reports it back, and says so
in bold on the page. Writing the values is the next stage.

To leave: hold the button for three seconds again, or do nothing for five
minutes. Both reboot the chip. **Entering setup mode deletes nothing**, so an
accidental press, a timeout, or pulling the power all leave the gadget exactly
as it was.

Two things worth knowing before you try it:

- **The hotspot password is new every time**, while its name is not. A phone
  that has joined before will try the old password first and be rejected; it
  then tells you the password is no longer valid and offers the field to type
  the new one, so this costs a tap rather than a detour. If your phone instead
  just fails, tell it to forget the network and join again.
- **The password is deliberately shown**, on screen and in the serial log. It
  guards a network that exists for five minutes and serves nothing but this
  form, and it is shown to whoever is standing in front of the gadget. Nothing
  else in this project is ever printed — your WiFi password is logged as a
  character count.

## Where this is in the build order

**Stage 8: polish** (`CLAUDE.md`'s build order) — written, flashed, and
verified on hardware. This is the last stage: every stage in the build order is
now done, and the chip meets the definition of done.

Stages 3 (WiFi), 4 (HTTP to a throwaway URL), 5 (the real service, parsed with
cJSON), 6 (panel bring-up) and 7 (rendering) were each verified on hardware
before this one started.

Stage 7 drew **once**, at boot. Stage 8 makes it a device rather than a demo:

- **It refreshes itself** every 45 seconds, and repaints only the lines whose
  contents actually changed. Repainting all 240×240 once a minute is a black
  flash you cannot help watching, and it happens whether or not a digit
  changed — which is what makes a working gadget feel broken.
- **It recovers from a dropped network** on its own. The reconnect attempts
  back off (2s while a boot is still settling, then 5s, then 30s) rather than
  probing a router that is off for the night eighteen hundred times an hour,
  and a dropped link now reaches the screen instead of only the serial log.
- **It degrades visibly.** Once a reading has been seen, a failed fetch keeps
  the numbers on screen and puts a banner over them saying what went wrong and
  how old they are. The words-only screens are reserved for having nothing to
  show at all.

### What the screen says, and when

The rule underneath all of it: the display must never be able to lie by
omission. A number with no banner under it is a promise that the number is
current, so every branch below exists to keep that promise.

| Situation | What you see |
|---|---|
| Fresh data | Two percentages, colour-banded, with their reset times. No banner |
| `pc_service` flagged its own data stale, or the data is over 10 min old | Same numbers, plus an amber `STALE 15M` banner giving the real age |
| Data over 30 min old | Numbers go flat grey, banner turns red. They are still the last thing known to be true — just no longer a claim about now |
| `pc_service` unreachable | Numbers stay, `NO LINK 3M` banner |
| WiFi dropped | Numbers stay, `NO WIFI 3M` banner |
| `pc_service` answered 503, or sent something unparseable | Numbers stay, `NO DATA` / `BAD DATA` banner |
| Any of the above, but nothing has *ever* been fetched | A words-only screen — `CONNECTING`, `NO LINK`, `NO DATA`, `BAD DATA` — because there is genuinely nothing better to show |

Losing the colour at 30 minutes is the strongest signal on the screen, and it
is deliberate: a red 94% and a grey 94% mean genuinely different things, and
the grey one has no business alarming anybody.

**Why 10 and 30 minutes**, rather than something tighter. `pc_service` backs
off when Anthropic rate-limits it — 120s, then 240s, then 480s, capped at 900s
— so a single upstream 429 lets the data reach 6 minutes old, and two in a row
14 minutes, with nothing actually wrong. Thresholds of 5 and 15 would fire on
both. An indicator that is wrong a third of the time is one you learn to
ignore, and then it cannot tell you the thing it exists for. Neither threshold
is the primary alarm anyway: a PC that has really gone away also fails the
fetch, which puts `NO LINK` up within one 45-second cycle. These two are the
backstop for the quieter failure — a service still answering, politely, with
data from an hour ago.

Stage 4's throwaway URL was the point of *that* stage: it proved
`esp_http_client` works on this chip and IDF version before `pc_service` was
in the picture, so a failure here has one suspect instead of two. That is why
a failed request at this stage points at the PC — not at WiFi, which already
has an IP, and not at the HTTP client, which already worked.

**New prerequisite: `pc_service` has to be running** on the machine named by
`PC_SERVICE_HOST`. Stage 4 needed nothing on the PC; this stage needs a
listener, a firewall that permits it, and an address that DHCP has not moved
since you wrote it into `secrets.h`. See `../pc_service/README.md`.

`main/token_monitor.c` is commented to be read start to finish; its header
comment maps the file and explains the one thing that trips people up, which
is that control flow is *not* top-to-bottom. `app_main` sets everything up
and then sleeps — the interesting work happens in event handlers, on a
different task.

## What "correct" looks like right now

Start `pc_service` on the PC first. Then build, flash, and open the serial
monitor:

```
idf.py -p COM3 flash monitor      REM adjust the port
```

Expect roughly this, in this order:

```
I (…) token_monitor: stage 6: display bring-up
I (…) gc9a01: hardware reset...
I (…) gc9a01: sending 42 vendor init commands...
I (…) gc9a01: init done
I (…) token_monitor:   fill: RED
I (…) token_monitor:   fill: GREEN
I (…) token_monitor:   fill: BLUE
I (…) token_monitor: display ready
I (…) token_monitor: stage 8: wifi, then refresh every 45 s forever
I (…) token_monitor: wifi started, connecting to "…"...
I (…) token_monitor: got IP: 192.168.1.42
I (…) token_monitor: netmask: 255.255.255.0, gateway: 192.168.1.1
I (…) token_monitor: polling http://192.168.1.50:8734/usage every 45 s
I (…) token_monitor:   header | Content-Type: application/json
I (…) token_monitor:   header | Content-Length: 278
I (…) token_monitor: status 200, content-length 279, body 279 bytes
---8<--- parsed ---8<---
  5-hour :  17%   resets 15:30      (epoch 1788784199)
  7-day  :  10%   resets Thu 19:00  (epoch 1789055999)
  updated 10:51   stale: no
  data age: 68s by the PC's clock
---8<--- end ------8<---
I (…) token_monitor: free heap 191868 bytes; next refresh in 45 s
I (…) token_monitor: still connected, rssi -63 dBm
```

…and then the same block again, 45 seconds later, for as long as it is
powered — everything from the `header` lines down. The banner above them is
printed once: the `polling …` line in particular is the one to read carefully
if the chip cannot reach the service, since it spells out exactly what address
`secrets.h` sent it to.

Six things to actually check, not just glance at:

1. **The screen shows the readings, and shows them the right way round.**
   This is the only check here the serial log cannot make for you: SPI writes
   are unacknowledged, so every line above prints identically into a panel that
   is not plugged in — or into one drawing everything mirrored, which is
   exactly what happened the first time stage 7 ran. Expect:

   ```
          . - - - - - - - .        5-hour arc, on the rim of the
      .  '  ___________  ` .       top half
    '      /           \    `
   |        5 - H O U R      |     dim grey label
   |           6 3 %         |     large, colour-banded
   |          1 7 : 1 0      |     reset time
   |       - - - - - - -     |     divider (or the status banner)
   |         7 - D A Y       |
   |            7 %          |
   |        T H U  1 9 : 0 0 |
    .      \___________/    ,      7-day arc, on the rim of the
      .  ,               . '       bottom half
          ' - - - - - - '
   ```

   Colour bands are green under 50%, amber to 80%, red above — and **the arc
   uses the same colour as its own number**, so if you ever see a green arc
   beside an amber figure, that is a bug and not a subtlety.

   The arcs are gauges, not decoration: each is anchored where its half of
   the ring begins and sweeps **clockwise**, 1.8° per percent. The 5-hour
   starts at nine o'clock and fills over the top; the 7-day starts at three
   o'clock and fills under the bottom. They meet at nine and three only when
   both read 100%. There is no track behind them, so an empty rim is zero.

   The **status banner shares the middle slot with the divider** — when
   something is wrong, the hairline is replaced by text such as `NO LINK 3M`.
   It lives there rather than at the bottom because the ring took that space;
   see `BANNER_MAX_CHARS` in the source for the arithmetic.
2. **The IP is on the same subnet as the PC running `pc_service`.** A
   different subnet means the chip landed on a guest network or a second AP,
   and no firewall rule will fix that.
3. **`status 200`, and a `parsed` block rather than a raw body.** If the
   parse fails, the firmware prints the raw bytes between `body` markers
   instead — that dump *is* the diagnosis: an HTML error page means something
   other than `pc_service` answered on that port.
4. **The numbers match what the PC serves.** Hit
   `http://<PC_SERVICE_HOST>:<port>/usage` from a browser on the same network
   and compare. They should agree field for field — this stage exists to prove
   the contract, not just the connection.

   Two details that look like bugs and are not. **`content-length -1` is not
   an error**: it means the reply came back chunked, with no `Content-Length`
   header, which is what stage 4 saw from example.com. The accumulator's own
   byte count is the source of truth and is what the parser is given.
   (Python's `http.server`, which `pc_service` uses, does send a real
   `Content-Length` — so expect a number here and `-1` only from other
   servers.) And **the reset epochs can differ by a second between polls**:
   upstream sends sub-second precision that gets truncated, so the epoch
   jitters while the displayed `HH:MM` string does not.
5. **The heartbeat keeps printing afterwards.** A monitor that goes silent
   right after the request means the HTTP task crashed — most likely a stack
   overflow, which prints a `***ERROR*** A stack overflow in task` panic just
   before the reboot.
6. **`free heap` is flat across refreshes.** This is the first code in the
   project that runs forever, and that changes which bugs matter: a leak of a
   few hundred bytes per fetch is invisible in one request and fatal within a
   day. Expect it to sit around 191–192 KB and stay there, wobbling by a few
   hundred bytes as lwIP buffers come and go. A number that walks steadily
   downward over successive refreshes is a leak, and the first suspects are a
   missing `esp_http_client_cleanup` or a missing `cJSON_Delete`.

The request now repeats **every 45 seconds**, forever. Note that the interval
is measured from the end of the previous attempt, not the start — so a cycle
that has to time out takes 55 seconds (45 + the 10-second HTTP timeout) rather
than 45. That is intended: it paces retries against a dead service instead of
letting them bunch up.

### Exercising the failure states on purpose

The interesting branches are the ones that are hard to catch by waiting, so
`../pc_service/tools/` has two throwaway stand-ins that produce them on demand.
Stop the real service first — both bind the same port, and the second to start
fails loudly rather than sharing it.

```
python tools/stub_503.py            # the "no data yet" branch
python tools/stub_stale.py 900      # 15 min old  -> amber STALE badge
python tools/stub_stale.py 2400     # 40 min old  -> numbers go grey
python tools/stub_stale.py 60 --flag  # fresh, but flagged stale by the PC
```

That last form is worth running at least once: the `stale` flag and the
computed age are independent signals, and the flag alone has to be enough to
raise the badge. Forgetting to stop the real service is safe — the second
process fails at bind time with `WinError 10048` rather than quietly sharing
the port, which would otherwise split the chip's polls between the two and
look like the firmware flapping. Run them with the same `python.exe` the real
service uses — the Windows firewall rule is per-program, so a different
interpreter is silently blocked and the chip sees a timeout instead of a
reply.

For `NO LINK`, just stop the service and watch: the numbers stay, the banner
appears within about a minute, and both clear on their own when it comes back.

## The font (`tools/make_font.py`)

Text needs glyph data, and glyph data written straight into C as hex is
unreviewable — a wrong bit is invisible until it is on the glass. So the font
lives as ASCII art in `tools/make_font.py`, which generates `main/font5x7.h`.
Both are committed, so **building never requires Python**; run the script only
when changing a glyph:

```
python tools/make_font.py
```

It refuses to emit a malformed glyph and prints a preview of the whole set. The
table is uppercase-only (the draw code folds lowercase) and covers ASCII 32-90
in 295 bytes; anything it cannot represent draws as a conspicuous box, so a
text problem looks like a text problem rather than a gap.

A **503** is a normal answer, not a failure of the firmware: it means
`pc_service` is up but has never completed a poll (just started, or being
rate-limited upstream), so it has no data to serve even as stale. The
firmware logs the reason and carries on; on screen it is a `NO DATA` banner
over the last known numbers, or a `NO DATA` screen if there are none yet.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Screen completely dark, no backlight glow | Power, not signal — check GND/VCC against the board's silkscreen first. The backlight is tied straight to VCC with no control pin, so a power fault is *total* darkness, and the serial log stays clean regardless |
| Screen lit but colours wrong or inverted | Calibration, not wiring: the BGR bit (`0x08`) of `madctl` in `gc9a01.c`, or the inversion command `0x21` |
| Text mirrored, upside down, or rotated | Also `madctl`, but the scan-direction bits rather than colour: `0x40` MX flips horizontally, `0x80` MY vertically, `0x20` MV rotates 90°. Solid-colour tests cannot reveal this — only asymmetric content can |
| Screen lit but streaky, noisy, or partial | Signal integrity — most likely the 40 MHz SPI clock over long jumper wires, or a loose SCL/SDA/DC line. `SPI_CLOCK_HZ` in `gc9a01.c` was 10 MHz through stage 7 and is the first thing to put back |
| Build error naming `secrets.h` | You haven't copied `secrets.h.example` to `secrets.h` yet |
| Phone will not join the setup hotspot | It is auto-reconnecting with the password from a previous session — the network name stays the same but the password does not. Most phones then say the password is wrong and let you type the new one; if yours does not, tell it to forget the network and join again. The chip's log shows the failure as `station ... leave, reason = 15` |
| Setup form loads, but submitting it does nothing | Check the log for `431` / "request URI/header too long". `CONFIG_HTTPD_MAX_REQ_HDR_LEN` (in `sdkconfig.defaults`, raised to 2048) has to be large enough for a mobile browser's POST headers — the GET of the form fits in the 512-byte default and the POST does not |
| The chip reboots whenever you close the serial monitor | Not a fault. This board is native USB-CDC, and the DTR/RTS toggle on opening *or* closing the port resets it. A test that spans a timeout needs one unbroken capture |
| `disconnected (reason 201)` repeating | AP not found — wrong SSID, or the network is 5 GHz only |
| `disconnected (reason 15)` or `(reason 2)` | Handshake failed — wrong password. A few `reason 2` retries *at startup* are normal and recover on their own |
| `disconnected (reason 205)`, intermittent | Weak signal; check the antenna is attached to the XIAO |
| Got an IP, but on the wrong subnet | Joined the wrong network (guest WiFi / neighbouring AP) |
| Monitor totally silent | Wrong COM port, or the chip is in bootloader mode — reset it |
| Chip ignores a `secrets.h` that is clearly on disk | You built once *before* creating it. ESP-IDF compiles through ccache, which keys its cache on the headers the previous compile opened — `secrets.h` was not among them, so creating it invalidates nothing and the stale object comes back. `idf.py fullclean` does **not** help; the cache lives outside `build/`. Two layers have to be defeated together, and each obvious single remedy fails: `CCACHE_RECACHE=1` alone does nothing because ninja never invokes the compiler, and `idf.py fullclean` alone does nothing because ccache returns the same stale object. Run **`idf.py fullclean`** and then **`CCACHE_RECACHE=1 idf.py build`** — RECACHE rather than `--no-ccache`, which would bypass the bad entry and leave it to be served again later. After that, normal builds are correct. The `config:` lines at boot are how you spot it — they print each value and where it came from |
| `request failed: ESP_ERR_HTTP_CONNECT` | `pc_service` is not running, the PC firewall is blocking the port, or `PC_SERVICE_HOST` is stale — DHCP moving the PC has happened in practice, and a router reservation is the fix (see `../pc_service/README.md`). Stage 4 already proved the chip's own networking. Expect this to take the **full 10s**: Windows drops packets to a closed port rather than refusing them, so there is no fast "connection refused" — and no way to tell "not running" from "blocked" by timing alone |
| `request failed: ESP_ERR_HTTP_EAGAIN` | Timed out after 10s with the connection already open — the PC answered, then stopped mid-response |
| `body is not valid JSON` | Something other than `pc_service` answered on that port; the raw body printed underneath says what |
| `five_hour_pct/seven_day_pct are missing` | The two sides disagree about the contract — change both together, per `../ARCHITECTURE.md` |
| `pc_service has no data yet (503)` | Expected right after starting the service, or while it is rate-limited upstream. Check `pc_service`'s own log |
| `(TRUNCATED at BODY_CAP)` | The reply outgrew the 4 KB buffer, so it is deliberately not parsed — a partial document is a broken one |
| Panic naming a stack overflow in `usage` | The 8 KB task stack was reduced; put it back |
| `free heap` falling a little more on every refresh | A leak on the once-a-request path — suspect a missing `esp_http_client_cleanup` or `cJSON_Delete` |
| Screen flashes black on every refresh | The partial-redraw model is being bypassed — something is calling `gc9a01_fill_screen` or forcing a screen-mode change each cycle |
| Arcs correct at boot, then develop gaps as numbers change | Something is drawing outside `CONTENT_R` and erasing the ring. This exact bug shipped once: the text row clears ran the full 240 px panel width, cutting both arms of the ring on every row that repainted. It looks fine until the first update, because the arcs are drawn last on a full repaint |
| An arc is a different colour from its own number | `set_arc` and the number are being passed different colours; they should both come from `usage_colour` (or both `COL_DEAD` when the data is stale) |
| An arc runs more than halfway round | `gc9a01_fill_arc` caps `sweep_deg` at 180 for a reason — past a half turn its two half-plane tests describe the complement of the wedge, and the arc inverts. Draw anything larger as two calls |
| Text truncated with `>` earlier than expected | Text is fitted to `CONTENT_R`, not the panel edge, because the ring owns the rim. The display is deliberately smaller than it looks |
| Screen frozen while the serial log keeps refreshing | Drawing happens on one task by convention, with nothing enforcing it. A second task touching `gc9a01` is the thing to look for |
| `Failed to connect … No serial data received` while flashing | Auto-reset into download mode did not take. Hold `B`, tap `R`, release `B`, then flash |
| Boots to `boot:0x0 (USB_BOOT)` and `wait usb download` | Still in download mode after a manual `B`+`R` flash. Tap `R` alone — do not hold `B` — to boot the app |

## Display code (`main/gc9a01.c`)

Hand-rolled on plain `spi_master` + `gpio` — no `esp_lcd`, no Component
Registry dependency, nothing to download. The init sequence and
`madctl = 0x48` are confirmed correct on this panel. See
`../ARCHITECTURE.md` for the pinout and the reasoning.

The interface stayed two functions wide (`gc9a01_init`, `gc9a01_fill_screen`)
for as long as that was all stage 6 needed; stage 7 added rectangle fills and
text on top of the same address-window mechanism, and stage 8 leans on
`gc9a01_fill_rect` for the partial redraws that keep a once-a-minute refresh
from flashing.

`gc9a01_fill_arc` came last, for the gauges. It fills the wedge as a
**region** rather than stroking it as a path, which is what makes it both
simple and gapless — sweeping an angle and plotting points needs sub-degree
steps at this radius (one degree is two pixels at r=118) and still leaves
ragged ends, whereas testing each pixel of the annulus for membership has no
step size to get wrong. The test itself is two integer cross products per
pixel: for a sweep of at most 180°, a point is inside exactly when it is
clockwise of the start ray and anticlockwise of the end ray. No `atan2`, no
floating point, and trigonometry only twice per call — from a 91-entry
fixed-point sine table — to turn the two angles into direction vectors.

`gc9a01_chord_half` is the other half of drawing on a round panel: how wide a
circle of a given radius is on a given row. The app asks it two things —
where the visible edge is, and where the ring's inner edge is — so that
knowledge lives in the driver rather than being re-derived by each caller.

The SPI clock is **40 MHz**, raised from 10 at stage 8 once the screen was
known-good — deliberately on its own, so that a failure would have one suspect
rather than two. Two things to know about that number: it is a widely-used
**overclock, not a spec figure** (the GC9A01 datasheet's 100 ns minimum write
cycle is 10 MHz), and it has no headroom above it, because `PIN_SCLK`/
`PIN_MOSI` are not the SPI2 IOMUX pins for CLK/MOSI and so route through the
GPIO matrix, whose master ceiling is exactly 40 MHz. If the panel ever goes
streaky after a rewiring, put `SPI_CLOCK_HZ` back to 10 MHz first: long jumper
wires are the usual reason a display that works at 10 does not work at 40.

**The panel self-test**, in the first two and a half seconds after reset:

```
I (422) gc9a01: hardware reset...
I (582) gc9a01: sending 42 vendor init commands...
I (722) gc9a01: init done
I (722) token_monitor:   fill: RED
I (1312) token_monitor:   fill: GREEN
I (1902) token_monitor:   fill: BLUE
I (2582) token_monitor: display ready
```

Three colours rather than one, deliberately: a single fill could be a screen
stuck on a colour from a previous run, whereas a sequence proves the chip is
genuinely driving the panel — and that red, green and blue arrive as red,
green and blue.

That confirms the BGR bit of `madctl` and **nothing else**. A solid fill is
symmetric, so it is blind to orientation — which is why this panel drew
everything mirrored for six stages with no test able to notice, until stage 7
rendered text. Worth remembering as a general habit: ask what a passing test
is blind to, not only what it covers.

Through stage 6 the cycle ended on solid blue, because a black screen and a
*dead* screen look identical and the resting state had to carry the proof.
From stage 7 it ends on `CONNECTING` instead — still black, but with text on
it, which makes the same point while saying something true about what the
device is doing during the WiFi join. Stage 8 keeps `CONNECTING` for exactly
as long as the chip has never associated; after that a lost link says
`NO WIFI`, because those are different situations and only one of them is a
normal boot.

The display is initialized **before** WiFi starts: the panel does not need the
network, the screen lights within a third of a second instead of after a
20-second join, and a dark screen cannot be blamed on WiFi that has not
started yet.

If the screen stays completely dark, **check GND and VCC against the board's
silkscreen before anything else.** This panel ties its backlight straight to
VCC with no control pin, so a power fault shows as total darkness — and the
serial log stays perfectly clean throughout, because SPI writes are
unacknowledged. A clean log is not evidence the panel is connected.
