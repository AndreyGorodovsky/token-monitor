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

**Stage 3: WiFi only** (`CLAUDE.md`'s build order) — written, flashed, and
verified on hardware. The chip joins the network, prints its IP, and does
nothing else. HTTP, JSON, and the display come in stages 4–8.

`main/token_monitor.c` is commented to be read start to finish; its header
comment maps the file and explains the one thing that trips people up, which
is that control flow is *not* top-to-bottom. `app_main` sets everything up
and then sleeps — the interesting work happens in event handlers, on a
different task.

## What "correct" looks like right now

Build, flash, and open the serial monitor. Expect:

```
I (…) token_monitor: stage 3: wifi only
I (…) token_monitor: wifi started, connecting to "…"...
I (…) token_monitor: got IP: 192.168.1.42
I (…) token_monitor: netmask: 255.255.255.0, gateway: 192.168.1.1
I (…) token_monitor: connected. nothing else to do at this stage.
I (…) token_monitor: still connected, rssi -54 dBm
```

Two things to actually check, not just glance at:

1. **The IP is on the same subnet as the PC running `pc_service`.** A
   different subnet means the chip landed on a guest network or a second AP,
   and no firewall rule will fix that.
2. **The heartbeat keeps printing.** A monitor that goes silent means the
   chip reset or crashed; an idle chip still logs every 10 seconds.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Build error naming `secrets.h` | You haven't copied `secrets.h.example` to `secrets.h` yet |
| `disconnected (reason 201)` repeating | AP not found — wrong SSID, or the network is 5 GHz only |
| `disconnected (reason 15)` or `(reason 2)` | Handshake failed — wrong password |
| `disconnected (reason 205)`, intermittent | Weak signal; check the antenna is attached to the XIAO |
| Got an IP, but on the wrong subnet | Joined the wrong network (guest WiFi / neighbouring AP) |
| Monitor totally silent | Wrong COM port, or the chip is in bootloader mode — reset it |

## Display code (stages 6-7)

The GC9A01 driver is hand-rolled on plain `spi_master` + `gpio` — no
`esp_lcd`, no Component Registry dependency. The init sequence and
`madctl = 0x08` are confirmed correct on this panel. Stage 6 brings it up
standalone (solid colour fill) before stage 7 renders real data through it.
See `../ARCHITECTURE.md` for the pinout and the reasoning.
