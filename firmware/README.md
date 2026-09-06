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

## Where this is in the build order

**Stage 7: the usage numbers, on the screen** (`CLAUDE.md`'s build order) —
written, flashed, and verified on hardware. This is where the project does the
thing it exists for: the chip initializes the panel, joins WiFi, fetches the
usage JSON, and renders it, so the gadget is readable with no serial monitor
attached.

Stages 3 (WiFi), 4 (HTTP to a throwaway URL), 5 (the real service, parsed with
cJSON) and 6 (panel bring-up) are all done and verified on hardware.

It still draws **once**, at boot. The refresh loop, reconnect handling and
considered failure states are stage 8 — the failure screens here are the
minimum that keeps the display from showing something untrue, not the finished
design.

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
I (…) token_monitor: stage 7: usage on the display
I (…) gc9a01: hardware reset...
I (…) gc9a01: sending 42 vendor init commands...
I (…) gc9a01: init done
I (…) token_monitor:   fill: RED
I (…) token_monitor:   fill: GREEN
I (…) token_monitor:   fill: BLUE
I (…) token_monitor: display ready
I (…) token_monitor: stage 5: wifi + one fetch of the real usage JSON
I (…) token_monitor: wifi started, connecting to "…"...
I (…) token_monitor: got IP: 192.168.1.42
I (…) token_monitor: netmask: 255.255.255.0, gateway: 192.168.1.1
I (…) token_monitor: connected.
I (…) token_monitor: stage 5: GET http://192.168.1.50:8734/usage
I (…) token_monitor:   header | Content-Type: application/json
I (…) token_monitor:   header | Content-Length: 278
I (…) token_monitor: status 200, content-length 278, body 278 bytes
---8<--- parsed ---8<---
  5-hour :  12%   resets 17:10      (epoch 1788703799)
  7-day  :   2%   resets Thu 19:00  (epoch 1789055999)
  updated 12:29   stale: no
  data age: 20s by the PC's clock
---8<--- end ------8<---
I (…) token_monitor: stage 7: rendered to the display
I (…) token_monitor: stage 5 complete. heartbeat continues; power-cycle to re-run.
I (…) token_monitor: still connected, rssi -62 dBm
```

Five things to actually check, not just glance at:

1. **The screen shows the readings, and shows them the right way round.**
   This is the only check here the serial log cannot make for you: SPI writes
   are unacknowledged, so every line above prints identically into a panel that
   is not plugged in — or into one drawing everything mirrored, which is
   exactly what happened the first time stage 7 ran. Expect:

   ```
           5-HOUR          dim grey label
            63%            large, colour-banded
           17:10           reset time
         ──────────
           7-DAY
             7%
         THU 19:00
   ```

   Colour bands are green under 50%, amber to 80%, red above. A `STALE` badge
   appears under the second reading only when the data is not current.
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

The request runs **once**, at startup. Power-cycle or reset the board to run
it again; stage 8 is what turns it into a repeating poll.

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
firmware logs the reason and carries on; from stage 7 this becomes a "no
data" screen.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Screen completely dark, no backlight glow | Power, not signal — check GND/VCC against the board's silkscreen first. The backlight is tied straight to VCC with no control pin, so a power fault is *total* darkness, and the serial log stays clean regardless |
| Screen lit but colours wrong or inverted | Calibration, not wiring: the BGR bit (`0x08`) of `madctl` in `gc9a01.c`, or the inversion command `0x21` |
| Text mirrored, upside down, or rotated | Also `madctl`, but the scan-direction bits rather than colour: `0x40` MX flips horizontally, `0x80` MY vertically, `0x20` MV rotates 90°. Solid-colour tests cannot reveal this — only asymmetric content can |
| Screen lit but streaky, noisy, or partial | Signal integrity — most likely the 10 MHz SPI clock over long jumper wires, or a loose SCL/SDA/DC line |
| Build error naming `secrets.h` | You haven't copied `secrets.h.example` to `secrets.h` yet |
| `disconnected (reason 201)` repeating | AP not found — wrong SSID, or the network is 5 GHz only |
| `disconnected (reason 15)` or `(reason 2)` | Handshake failed — wrong password. A few `reason 2` retries *at startup* are normal and recover on their own |
| `disconnected (reason 205)`, intermittent | Weak signal; check the antenna is attached to the XIAO |
| Got an IP, but on the wrong subnet | Joined the wrong network (guest WiFi / neighbouring AP) |
| Monitor totally silent | Wrong COM port, or the chip is in bootloader mode — reset it |
| `request failed: ESP_ERR_HTTP_CONNECT` | `pc_service` is not running, the PC firewall is blocking the port, or `PC_SERVICE_HOST` is stale. Stage 4 already proved the chip's own networking. Expect this to take the **full 10s**: Windows drops packets to a closed port rather than refusing them, so there is no fast "connection refused" — and no way to tell "not running" from "blocked" by timing alone |
| `request failed: ESP_ERR_HTTP_EAGAIN` | Timed out after 10s with the connection already open — the PC answered, then stopped mid-response |
| `body is not valid JSON` | Something other than `pc_service` answered on that port; the raw body printed underneath says what |
| `five_hour_pct/seven_day_pct are missing` | The two sides disagree about the contract — change both together, per `../ARCHITECTURE.md` |
| `pc_service has no data yet (503)` | Expected right after starting the service, or while it is rate-limited upstream. Check `pc_service`'s own log |
| `(TRUNCATED at BODY_CAP)` | The reply outgrew the 4 KB buffer, so it is deliberately not parsed — a partial document is a broken one |
| Panic naming a stack overflow in `usage_fetch` | The 8 KB task stack was reduced; put it back |
| `Failed to connect … No serial data received` while flashing | Auto-reset into download mode did not take. Hold `B`, tap `R`, release `B`, then flash |
| Boots to `boot:0x0 (USB_BOOT)` and `wait usb download` | Still in download mode after a manual `B`+`R` flash. Tap `R` alone — do not hold `B` — to boot the app |

## Display code (`main/gc9a01.c`)

Hand-rolled on plain `spi_master` + `gpio` — no `esp_lcd`, no Component
Registry dependency, nothing to download. The init sequence and
`madctl = 0x48` are confirmed correct on this panel. See
`../ARCHITECTURE.md` for the pinout and the reasoning.

The interface is two functions wide on purpose (`gc9a01_init`,
`gc9a01_fill_screen`); stage 7 adds rectangle fills and text on top of the
same address-window mechanism.

**What stage 6 puts on the screen**, in the first two and a half seconds
after reset:

```
I (422) gc9a01: hardware reset...
I (582) gc9a01: sending 42 vendor init commands...
I (722) gc9a01: init done
I (722) token_monitor:   fill: RED
I (1312) token_monitor:   fill: GREEN
I (1902) token_monitor:   fill: BLUE
I (2582) token_monitor: display ready (screen should now be solid BLUE and stay that way)
```

Three colours rather than one, and it rests **lit** rather than black. Both
are deliberate. A single fill could be a screen stuck on a colour from a
previous run, whereas a sequence proves the chip is genuinely driving the
panel — and that red, green and blue arrive as red, green and blue, which is
what confirms the BGR half of `madctl`. Resting on blue matters because a black screen
and a *dead* screen look identical, so the resting state would otherwise
prove nothing to anyone who missed the cycle.

The display is initialized **before** WiFi starts: the panel does not need the
network, the screen lights within a third of a second instead of after a
20-second join, and a dark screen cannot be blamed on WiFi that has not
started yet.

If the screen stays completely dark, **check GND and VCC against the board's
silkscreen before anything else.** This panel ties its backlight straight to
VCC with no control pin, so a power fault shows as total darkness — and the
serial log stays perfectly clean throughout, because SPI writes are
unacknowledged. A clean log is not evidence the panel is connected.
