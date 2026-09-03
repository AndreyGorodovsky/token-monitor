# Token Monitor — Claude usage on a round display

A little desk gadget built on a Seeed XIAO ESP32-C3: it shows the current
Claude subscription usage of one machine (5-hour window and 7-day window,
plus reset times) on a round 1.28" GC9A01 color TFT, refreshed automatically
over WiFi.

## How it works

Anthropic doesn't publish an official API for subscription usage, but Claude
Code reads an internal endpoint to power its own `/status` command, using an
OAuth token it keeps on disk. That token authenticates the whole account, so
it never leaves the PC:

- **`pc_service/`** — a small always-running local process. Reads the token,
  polls the usage endpoint, and re-serves a stripped-down JSON summary
  (percentages and reset times only, never the token) on the LAN.
  **Working** — see `pc_service/README.md`.
- **`firmware/`** — the ESP-IDF project for the chip. Joins WiFi, polls the
  PC service, parses the small JSON reply, and draws it on the display.
  **In progress**, currently at the WiFi stage — see `firmware/README.md`.

## Documentation

- **`CLAUDE.md`** — the brief: what's being built, the constraints
  (especially around the OAuth token — read it before touching anything
  credential-related), and the staged build order.
- **`ARCHITECTURE.md`** — the design: the two components, how they talk to
  each other, the exact JSON contract between them, the display wiring, and
  the layout.
- **`SECRETS.md`** — the inventory of what's a real secret vs. a
  placeholder, the `.example` + `.gitignore` convention the project follows,
  and a checklist to run before publishing the repository. A starter
  `.gitignore` already covers the expected secret filenames.
- **`STATUS.md`** — the live progress log: what's verified, what's next, and
  the facts established along the way. Read it before resuming work.

## Getting started

1. Copy `firmware/main/secrets.h.example` to `firmware/main/secrets.h` and
   fill in the WiFi credentials and the LAN address of the machine that will
   run `pc_service`. The real file is gitignored; the build fails loudly
   without it.
2. Run `python pc_service/service.py` on that machine and confirm
   `curl http://<its-lan-ip>:8734/usage` works *from another device* — see
   `pc_service/README.md` for the firewall details.
3. Build and flash `firmware/` with ESP-IDF (target `esp32c3`).

This project is written to be built and extended with Claude Code (the CLI).
Opening a terminal in this folder, running `claude`, and asking it to read
`CLAUDE.md` and `ARCHITECTURE.md` will pick the work up where `STATUS.md`
leaves off.
