# Token Monitor

An always-on desk gadget that shows current Claude subscription usage — how
much of the 5-hour rolling window and the 7-day rolling window has been used,
and when each one resets — on a round 1.28" colour display driven by a Seeed
XIAO ESP32-C3. It refreshes itself over WiFi every minute; there are no
buttons and nothing to check manually.

**Status:** the PC-side service is finished and running; the firmware is at
stage 3 of 8 (WiFi verified on hardware). Display rendering is next. See
[`STATUS.md`](STATUS.md) for the detailed log.

## How it works

Anthropic doesn't publish an official API for subscription usage, but Claude
Code reads an internal endpoint to power its own `/status` command, using an
OAuth token it keeps on disk. That token authenticates the entire account, so
the design keeps it on the PC and never lets it near the network:

```
  Anthropic  ──HTTPS──>  pc_service  ──HTTP (LAN)──>  ESP32-C3  ──SPI──>  display
                             ▲             ▲
                    the token        only percentages
                    lives here       and timestamps
                    and nowhere      cross this hop
                    else
```

`pc_service` polls Anthropic once a minute, caches the last good result, and
re-serves a trimmed JSON summary — percentages and reset times only — to the
local network. The chip polls that, parses it, and draws it. If either hop
fails, the chip keeps showing the last known numbers, visibly marked stale,
rather than freezing on a number that looks current.

## Repository layout

```
token-monitor/
├── README.md               you are here
├── ARCHITECTURE.md         the design: components, JSON contract, wiring, layout
├── STATUS.md               build log — what is verified, what is next
├── SECRETS.md              what is sensitive, and the pre-publish checklist
├── CLAUDE.md               project brief for AI coding assistants (see below)
├── .gitignore
│
├── pc_service/             the PC side — Python 3.9+, standard library only
│   ├── README.md           running it, firewall setup, troubleshooting
│   ├── fetch_usage.py      reads the token, makes one call, prints the result
│   └── service.py          polls on a timer, caches, serves GET /usage
│
└── firmware/               the chip side — ESP-IDF project, target esp32c3
    ├── README.md           build, flash, and what a good serial log looks like
    ├── CMakeLists.txt
    ├── sdkconfig.defaults  checked-in build config (4 MB flash, etc.)
    └── main/
        ├── CMakeLists.txt
        ├── token_monitor.c the firmware itself
        └── secrets.h.example  template — copy to secrets.h and fill in
```

`secrets.h` (real WiFi credentials and the PC's LAN address) and
`firmware/.vscode/` are deliberately absent: they are gitignored, being
per-machine. Everything needed to recreate them is in the repo.

## Hardware

| Part | Notes |
|---|---|
| Seeed XIAO ESP32-C3 | 2.4 GHz WiFi only — a 5 GHz-only network will not work |
| 1.28" round TFT, GC9A01 | 240x240, RGB565 colour, SPI |

| Display pin | XIAO pin | GPIO |
|---|---|---|
| GND | GND | — |
| VCC | 3.3V-OUT | — |
| SCL (SCK) | D2 | GPIO4 |
| SDA (MOSI) | D3 | GPIO5 |
| RES | D4 | GPIO6 |
| DC | D5 | GPIO7 |
| CS | D8 | GPIO8 |

Check GND and VCC against the display's own silkscreen before powering it —
swapping those two is the one wiring mistake that can damage the panel. The
signal pins are harmless to mis-wire. Full rationale in
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Setup

### 1. The PC service

Requires Python 3.9 or newer. No dependencies, no virtualenv — deliberately,
since on Windows a firewall rule is often bound to one specific `python.exe`.

```
cd pc_service
python service.py            # serves on 0.0.0.0:8734
```

Check it works locally, then — separately — from another device:

```
curl http://127.0.0.1:8734/usage              # proves the service runs
curl http://<pc-lan-ip>:8734/usage            # proves the chip could reach it
```

Those are different tests: loopback traffic never touches the firewall. A
phone on the same WiFi is a good stand-in for the chip. A normal reply looks
like this:

```json
{
  "five_hour_pct": 42,
  "five_hour_resets_at": "17:00",
  "five_hour_resets_epoch": 1788291600,
  "seven_day_pct": 61,
  "seven_day_resets_at": "Fri 00:00",
  "seven_day_resets_epoch": 1788652800,
  "updated_at": "15:04",
  "updated_epoch": 1788285840,
  "now_epoch": 1788285900,
  "stale": false
}
```

Each reset time is sent twice on purpose: the string is preformatted in the
PC's local timezone so the chip needs no calendar maths, and the epoch is
there for anything the chip would rather compute itself. `stale: true` means
the service is serving its last good result because an upstream poll failed.

Firewall details and troubleshooting are in
[`pc_service/README.md`](pc_service/README.md).

### 2. The firmware

```
cd firmware/main
copy secrets.h.example secrets.h     REM Windows
cp secrets.h.example secrets.h       # Linux
```

Fill in the WiFi credentials and the LAN address of the machine running
`pc_service`. The build fails loudly without this file, on purpose. Then
build and flash with ESP-IDF (target `esp32c3`), opening the editor on
`firmware/` rather than the repository root. See
[`firmware/README.md`](firmware/README.md).

## Security

The OAuth token is the credential for the whole Claude account, so:

- It is read at runtime from Claude Code's own credential file and never
  copied into this repository.
- It is sent to `api.anthropic.com` and nowhere else — never to the chip,
  never to a log file.
- The LAN hop between the PC and the chip carries only integers and
  timestamps; there is nothing on it that could be replayed against
  Anthropic.
- WiFi credentials live in a gitignored `secrets.h`, with a committed
  `.example` template.

[`SECRETS.md`](SECRETS.md) is the full inventory and the checklist to run
before publishing a fork of this.

The usage endpoint is undocumented and could change or disappear without
notice, so both components treat non-200 responses, timeouts and unexpected
JSON as normal conditions to display, not crash on.

## A note on `CLAUDE.md`

This project is built with [Claude Code](https://claude.com/claude-code) as a
pair-programming partner. `CLAUDE.md` is written for it rather than for a
human reader: it holds the brief, the constraints, and the staged build order
the assistant is expected to follow. It is safe to ignore if you are just
reading the code — everything a person needs is in this README,
`ARCHITECTURE.md`, and `STATUS.md`.
