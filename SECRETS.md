# Secrets checklist (read before pushing this to a public repo)

This project touches two kinds of sensitive data: the Claude account's OAuth
token, and WiFi credentials. Neither should ever end up in git history. This
file is the single source of truth for what's real vs. placeholder — both
for whoever (Claude Code included) writes the code, and as a pre-publish
check before the repository goes public.

## Inventory

| What | Where it lives | Ever touches this repo? | Git status |
|---|---|---|---|
| Claude OAuth token | `~/.claude/.credentials.json` (`%USERPROFILE%\.claude\.credentials.json` on Windows), outside this project entirely | No — `pc_service` reads it at runtime from that external path on every poll, never copies it in | N/A, nothing to gitignore |
| WiFi SSID + password | `firmware/main/secrets.h`, **and/or** the chip's own NVS partition | Only the `secrets.h` half does | **gitignored** — only `secrets.h.example` (fake values) is committed. The NVS copy lives in flash on the chip and never touches the repo |
| PC service's LAN host/port | `firmware/main/secrets.h` (same file — it's "this machine's network config", not just WiFi), **and/or** NVS | Only the `secrets.h` half does | **gitignored**, same as above |
| Setup mode's hotspot password | Nowhere. Generated at random each time setup mode is entered, held in RAM, gone at the reboot that ends it | No | N/A — there is no file to ignore |

Since these four values moved to runtime configuration, `secrets.h` is
**optional**: NVS can supply all of them, and the firmware builds and runs
with no `secrets.h` present at all. When both have a value, NVS wins, per
key. Two consequences worth stating plainly:

- **NVS is not encrypted.** A WiFi password stored there is readable by
  anyone who can plug the board into a USB port and dump its flash. This is
  not a downgrade — a compiled-in `#define` was equally readable in the
  binary — but it is now a second place the value exists, and moving the
  gadget on to someone else means erasing it (`idf.py erase-flash`) rather
  than just not handing over `secrets.h`. Since provisioning stage 4 this is
  the ordinary case rather than the exception: the setup form writes the WiFi
  password to NVS whenever the field is filled in, so most provisioned chips
  hold one there. A blank field leaves the stored key untouched, so a chip can
  still be running on a `secrets.h` password with everything else from NVS.
- **The repo is now clone-and-flash.** Because `secrets.h` is optional,
  nothing about publishing this repository requires a reader to receive, or
  invent, any secret to get a *building* image — and since provisioning stage
  4 the same is true of a *working* one. A chip flashed with no `secrets.h`
  comes up with an empty configuration, raises its setup hotspot on its own,
  and takes the four values from a phone. `secrets.h` and the
  `nvs_partition_gen.py` route both still work; neither is required any more.
  This bullet spent three stages saying the opposite, which is what the
  "revisit when the portal lands" note in it was for.

"Nowhere" in that row takes one deliberate call to stay true, and it is worth
knowing which. `esp_wifi` defaults to `WIFI_STORAGE_FLASH`, so
`esp_wifi_set_config()` does not merely configure the radio — it writes the
network name **and password** into the driver's own unencrypted
`nvs.net80211` namespace, where they would outlive the reboot that ends setup
mode. `provision.c` calls `esp_wifi_set_storage(WIFI_STORAGE_RAM)` first to
prevent exactly that. Anyone touching the AP setup path should leave that call
where it is, or fix this row to match. (Found in review, after the row was
written and before it was true.)

The hotspot password is the one credential this project deliberately
**displays**. It is drawn on the panel in large type and printed to the serial
log, and both are correct: it protects a network that exists for five minutes,
whose only reachable service is the setup form, and it is shown to whoever is
standing in front of the gadget. It is regenerated on every entry rather than
derived from the MAC address, because the MAC is broadcast in every beacon
frame and a derived password would therefore be computable by anyone in range.
Nothing else in this project is ever printed: the WiFi password is logged as a
character count, and the OAuth token never leaves the PC.

Nothing else in this project should hold a real credential. If a future
stage adds one (an API key, a different token, a password), add it to this
table and follow the same pattern before writing the code.

Two more things are gitignored without being *secrets*, because they are
purely local and would be wrong on anyone else's machine:
`firmware/.vscode/` (absolute toolchain paths, the local username, the COM
port) and `firmware/sdkconfig` / `sdkconfig.old` (machine-generated; the
settings that matter are committed in `sdkconfig.defaults`).

## The convention every secret-holding file follows

For any file that holds a real value from the table above:

1. The real file (e.g. `secrets.h`) is **listed in `.gitignore`** — added in
   the same commit/step that creates the file, not as an afterthought.
2. A **committed sibling** with `.example` before the extension (e.g.
   `secrets.h.example`) holds the same structure with obviously-fake values
   (`"your-wifi-name-here"`, not a half-redacted real one).
3. `README.md`/the setup instructions tell a reader to copy the `.example`
   file to the real filename and fill in their own values — this is what
   makes the repo actually buildable by someone else.

Expected content of `firmware/main/secrets.h.example` once it exists:

```c
#pragma once

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// LAN address of the machine running pc_service
#define PC_SERVICE_HOST "192.168.1.50"
#define PC_SERVICE_PORT 8734
```

`secrets.h` (gitignored, real values) has the same shape, filled in for
real.

## Before the repo goes public

A quick pass to run once code actually exists, before flipping the repo to
public:

- `git log --all -- '*secrets.h'` (or the equivalent for whatever the real
  filename ends up being) — should return nothing tracked, ever, in history.
  If it does, the file needs to be scrubbed from history (not just deleted
  in a later commit), not just added to `.gitignore` after the fact.
- Search the whole tree for the real WiFi password and SSID as literal
  strings, in case they got pasted into a README, a comment, or a captured
  terminal-output example. **This has already happened once here:** a real
  SSID ended up in `STATUS.md` as an incidental detail while writing up a
  WPA3 finding, and was caught by a code review rather than by anyone
  noticing at the time. Prose about debugging is the easy leak path, not
  code — a network note should name the property (WPA3-SAE, channel, RSSI)
  and never the identifier.
- Search for `Bearer ` and any long token-looking string, in case a debug
  session's terminal output got copy-pasted into documentation somewhere.
- Check any screenshots or terminal-output snippets included in a write-up
  — a serial monitor capture or a `curl` response example is exactly the
  kind of thing that quietly contains a real LAN IP, a real reset timestamp,
  or (if a debug print was left in) a real token. Redact or fabricate
  example values for anything published.
- Grep for the local username and absolute local paths (e.g.
  `C:\Users\<name>`) — harmless, but not worth publishing either; prefer
  relative paths or `%USERPROFILE%`-style references in anything committed,
  which the docs in this project already use.

None of this blocks starting the build — it's a checklist for the moment
before the repository is made public, not a prerequisite for writing code.
