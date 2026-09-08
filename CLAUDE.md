# Project brief: Claude usage monitor on the XIAO ESP32-C3

Read this whole file, and `ARCHITECTURE.md` next to it, before writing any
code.

## How to work on this project

This is a beginner-friendly firmware project: assume the person following
along is comfortable with programming but new to embedded work, and has a
working ESP-IDF toolchain and a Seeed XIAO ESP32-C3.

So: build it in small, individually-runnable stages, explain what each stage
does and how to verify it before moving to the next one, and don't silently
skip the verification steps even if the code is obviously correct. The goal
is a project that can be *understood*, not just a working binary.

The display is a round 1.28" TFT — driver chip GC9A01, 240x240 pixels, color
(RGB565), SPI interface. See `ARCHITECTURE.md`'s "Display hardware" section
for the exact characteristics and the pinout.

## The goal

A little always-on desk gadget: the ESP32-C3 shows current Claude
subscription usage on the round display — percentage used of the 5-hour
rolling window, percentage used of the 7-day rolling window, and when each
resets — refreshed automatically, no button presses needed.

Scope for this build: **one machine only** (the PC running `pc_service`).
No multi-machine aggregation, no cloud relay, no historical graphs. Get one
chip reliably showing one machine's live usage first.

## Why there are two components

Anthropic doesn't publish an official API for this, but Claude Code itself
calls an internal endpoint (`GET https://api.anthropic.com/api/oauth/usage`)
to power its own `/status` command, and that endpoint can be called directly
with the OAuth token Claude Code already has saved on disk. That token is
sensitive (it's the same one that authenticates the whole account) and lives
on the PC — it must never reach the ESP32 or leave the PC except in calls to
`api.anthropic.com`. So the design splits into:

1. **`pc_service/`** — a small, always-running local process on the PC.
   Reads the token, polls the usage endpoint, and re-serves a *stripped-down*
   JSON summary (percentages and reset times only — never the token) on the
   local network.
2. **`firmware/`** — the ESP-IDF project. Joins WiFi, polls the PC service
   (not Anthropic directly), parses the small JSON reply, and draws it on
   the display.

Full detail — exact endpoints, JSON shapes, error handling, display layout —
is in `ARCHITECTURE.md`. Don't improvise the contract between the two
components; follow what's written there so they agree with each other.

## Hard constraints

This project is meant to live in a public repository, so treat every secret
as if it will leak the moment you get it wrong.

- **Read `SECRETS.md` before creating any file that holds a credential or
  network config.** It's the exact inventory of what's sensitive, where it
  lives, and the `.example`-sibling + `.gitignore` convention to follow. A
  starter `.gitignore` already exists in this folder covering the expected
  filenames (`firmware/main/secrets.h`, `pc_service/.env`) — if you name
  things differently, update `.gitignore` and `SECRETS.md` to match, in the
  same step, not after.
- **Never commit the OAuth token, or anything derived from it 1:1, to a
  tracked file.** The PC service reads it at runtime from Claude Code's own
  credential storage; it doesn't get copied into this project's source.
- **Never send the token anywhere except `https://api.anthropic.com`.** Not
  to the ESP32, not logged to a file, not printed except in an explicit
  local-only debug mode that has to be turned on deliberately.
- **No WiFi credentials or other secrets in tracked source files.** Follow
  `SECRETS.md`'s convention: the real file (e.g. `secrets.h`) gitignored,
  a `secrets.h.example` with fake placeholder values committed alongside
  it, and setup instructions telling a reader to copy one to the other.
  Whenever you create a new file that holds a real secret, create its
  `.example` sibling and confirm it's covered by `.gitignore` in that same
  step — don't leave it for later.
- **This endpoint is undocumented.** Anthropic can change or remove it
  without notice. Code defensively: handle non-200 responses, timeouts, and
  unexpected JSON shapes without crashing the PC service or the firmware.
  If the endpoint is unreachable, show a clear "stale" or "no data" state on
  the display rather than a frozen or wrong-looking number.

## Build order

Build and verify each stage before moving to the next. Don't jump ahead.

1. **PC service, step 1 — prove the API call works.** A short script that
   reads the token from Claude Code's credential file, makes one request to
   the usage endpoint, and prints the parsed JSON to the console. Get this
   right and confirmed against what `/status` shows in Claude Code before
   building anything else on top of it.
2. **PC service, step 2 — turn it into a small local server.** Add polling
   on an interval, in-memory caching of the last good result, and a minimal
   HTTP endpoint that serves the trimmed JSON contract from
   `ARCHITECTURE.md`. Verify by hitting it with a browser or `curl` from
   another device on the LAN.
3. **Firmware, step 1 — WiFi only.** Connect to the network, print the
   assigned IP over serial, nothing else. Verify over serial monitor before
   adding anything else.
4. **Firmware, step 2 — HTTP client sanity check.** GET some trivial known
   endpoint (doesn't have to be the real service yet) and print the raw
   response body over serial, to confirm `esp_http_client` works on this
   chip/IDF version before pointing it at the real service.
5. **Firmware, step 3 — talk to the real PC service.** GET the PC service's
   endpoint, parse the JSON with cJSON, and print the parsed values over
   serial (no display yet). Confirms the network path and the JSON contract
   both work end-to-end.
6. **Firmware, step 4 — display bring-up.** Before rendering real data, get
   the GC9A01 initialized and filling the whole screen with a solid color —
   a standalone sanity check that the panel, wiring, and init sequence work,
   with nothing else in the way to debug. See `ARCHITECTURE.md` for the
   driver approach (plain `spi_master` + `gpio`, no external components).
7. **Firmware, step 5 — put it on the screen.** Combine steps 5 and 6:
   render the real values from the PC service using the display code proven
   in step 6. Land on a layout (see `ARCHITECTURE.md`'s suggestions) rather
   than over-engineering it up front.
8. **Polish.** Periodic refresh loop, WiFi reconnect handling, and a visible
   fallback state when the PC service or WiFi is unreachable.

## Definition of done

The chip, sitting on a desk connected to WiFi, shows current 5-hour and
7-day usage percentages and their reset times, refreshing on its own every
30–60 seconds, and degrades visibly (not silently) if the PC service or
network drops.

## Follow-on work: WiFi provisioning

The definition of done above is met, and the eight-stage build order is
finished. A follow-on feature is being built on the `wifi-provisioning`
branch: a button that puts the chip into its own WiFi hotspot serving a setup
form, so changing networks or PC address no longer needs an editor, a
toolchain and a USB cable.

It has its own five-stage order and its own set of decisions that should not
be re-argued. **`STATUS.md`'s "WiFi provisioning" section is the authority on
both** — read it before touching that branch. The same working rules apply:
small stages, each verified on hardware before the next one starts.

The hard constraints above still hold, and one gains a second home: the WiFi
password can now live in the chip's NVS as well as in `secrets.h`. **NVS is
not encrypted**, so it is readable over USB — no worse than a compiled-in
`#define`, but it is a second place the value exists. `SECRETS.md`'s
inventory covers it.
