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

Then open `main/secrets.h` and fill in your real WiFi name and password.
That file is gitignored and never committed; `secrets.h.example` (fake
values) is the committed template. See `../SECRETS.md`.

Note the ESP32-C3 is **2.4 GHz only**. If the router publishes 2.4 and
5 GHz under one name, that is the first thing to suspect on a failure.

## Where this is in the build order

**Stage 4: WiFi + one HTTP sanity check** (`CLAUDE.md`'s build order) —
written, flashed, and verified on hardware. The chip joins the network, then
makes a single plain-HTTP GET to `http://example.com/` and prints the raw
response body over serial.

Stage 3 (WiFi only) is done and was verified on hardware. JSON parsing, the
real `pc_service` endpoint, and the display come in stages 5–8.

The throwaway URL is the point of the stage: it proves `esp_http_client`
works on this chip and IDF version *before* `pc_service` is in the picture,
so a stage-5 failure has one suspect instead of two. It also proves DNS,
which the real service — reached by raw IP — never would.

`main/token_monitor.c` is commented to be read start to finish; its header
comment maps the file and explains the one thing that trips people up, which
is that control flow is *not* top-to-bottom. `app_main` sets everything up
and then sleeps — the interesting work happens in event handlers, on a
different task.

## What "correct" looks like right now

Build, flash, and open the serial monitor:

```
idf.py -p COM3 flash monitor      REM adjust the port
```

Expect roughly this, in this order:

```
I (…) token_monitor: stage 4: wifi + one http sanity check
I (…) token_monitor: wifi started, connecting to "…"...
I (…) token_monitor: got IP: 192.168.1.42
I (…) token_monitor: netmask: 255.255.255.0, gateway: 192.168.1.1
I (…) token_monitor: connected.
I (…) token_monitor: stage 4: GET http://example.com/
I (…) token_monitor:   header | Content-Type: text/html
I (…) token_monitor:   header | Transfer-Encoding: chunked
I (…) token_monitor: status 200, content-length -1, body 559 bytes
---8<--- body ---8<---
<!doctype html>
<html>
… example.com's HTML …
</html>
---8<--- end ----8<---
I (…) token_monitor: stage 4 complete. heartbeat continues; power-cycle to re-run.
I (…) token_monitor: still connected, rssi -54 dBm
```

Four things to actually check, not just glance at:

1. **The IP is on the same subnet as the PC running `pc_service`.** A
   different subnet means the chip landed on a guest network or a second AP,
   and no firewall rule will fix that.
2. **`status 200`**, and the body between the markers is real HTML rather
   than empty or a fragment.
3. **The body is complete** — for example.com, it ends in `</html>` — and
   the line does not end in `(TRUNCATED at BODY_CAP)`, which would mean it
   outgrew the 4 KB buffer. This matters because stage 5 hands the same
   buffer to a JSON parser, which needs a whole document, not a fragment.

   **`content-length -1` is not an error.** example.com is served through
   Cloudflare with `Transfer-Encoding: chunked`, so there is no
   `Content-Length` header at all and `esp_http_client_get_content_length()`
   correctly reports -1. That is useful rather than annoying: chunked is the
   case where the body definitely arrives in several pieces, so a passing run
   proves the accumulator actually works. Treat the accumulator's own byte
   count as the source of truth, never `content-length`.
4. **The heartbeat keeps printing afterwards.** A monitor that goes silent
   right after the request means the HTTP task crashed — most likely a stack
   overflow, which prints a `***ERROR*** A stack overflow in task` panic just
   before the reboot.

The request runs **once**, at startup. Power-cycle or reset the board to run
it again; stage 5 is what turns it into a repeating poll.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Build error naming `secrets.h` | You haven't copied `secrets.h.example` to `secrets.h` yet |
| `disconnected (reason 201)` repeating | AP not found — wrong SSID, or the network is 5 GHz only |
| `disconnected (reason 15)` or `(reason 2)` | Handshake failed — wrong password |
| `disconnected (reason 205)`, intermittent | Weak signal; check the antenna is attached to the XIAO |
| Got an IP, but on the wrong subnet | Joined the wrong network (guest WiFi / neighbouring AP) |
| Monitor totally silent | Wrong COM port, or the chip is in bootloader mode — reset it |
| `request failed: ESP_ERR_HTTP_CONNECT` | No route off the LAN, or DNS failed — the chip has an IP, so this is not the WiFi join |
| `request failed: ESP_ERR_HTTP_EAGAIN` | Timed out after 10s; slow or captive-portal network |
| A 301/302 instead of 200 | Something is redirecting plain HTTP — try `http://neverssl.com` instead |
| Panic naming a stack overflow in `http_sanity` | The 8 KB task stack was reduced; put it back |
| `Failed to connect … No serial data received` while flashing | Auto-reset into download mode did not take. Hold `B`, tap `R`, release `B`, then flash |
| Boots to `boot:0x0 (USB_BOOT)` and `wait usb download` | Still in download mode after a manual `B`+`R` flash. Tap `R` alone — do not hold `B` — to boot the app |

## Display code (stages 6-7)

The GC9A01 driver is hand-rolled on plain `spi_master` + `gpio` — no
`esp_lcd`, no Component Registry dependency. The init sequence and
`madctl = 0x08` are confirmed correct on this panel. Stage 6 brings it up
standalone (solid colour fill) before stage 7 renders real data through it.
See `../ARCHITECTURE.md` for the pinout and the reasoning.
