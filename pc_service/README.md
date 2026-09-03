# pc_service

Polls Anthropic's usage endpoint and re-serves a trimmed summary to the LAN
for the ESP32 to read. See `../ARCHITECTURE.md` for the design and the exact
JSON contract, `../SECRETS.md` for what is and isn't sensitive here.

Pure Python standard library — no `pip install`, no venv required.
(See the firewall note below before you decide to add a venv anyway.)

## Files

- `fetch_usage.py` — reads the OAuth token, makes one call, prints the
  result. Run it standalone to check the upstream endpoint still works.
- `service.py` — the actual service: polls on a timer, caches the last good
  result, serves `GET /usage`.

## Running it

```
python service.py                  # serves on 0.0.0.0:8734
python service.py --port 9000      # different port
python service.py --host 127.0.0.1 # local only, not reachable by the chip
```

Check it locally, then from another device:

```
curl http://127.0.0.1:8734/usage           # on this machine
curl http://<this-machine-lan-ip>:8734/usage   # from anywhere else
```

Those two are **different tests**. Loopback traffic never passes through the
firewall, so a working `127.0.0.1` result proves the service runs — it proves
nothing about whether the ESP32 can reach it. Always confirm with the second
form. A phone on the same WiFi is a good stand-in for the chip before any
firmware exists.

## Platform support

| Platform | Token source | Status |
|---|---|---|
| Windows | `%USERPROFILE%\.claude\.credentials.json` | supported |
| Linux | `~/.claude/.credentials.json` | supported |
| macOS | system Keychain, not a file | **not implemented** |

`pathlib.Path.home()` covers Windows and Linux in one code path. macOS fails
with an explanatory error rather than a misleading "file not found" — see
`read_access_token()`.

## Network setup (the part that isn't code)

The service binds `0.0.0.0`, so it is listening on every interface. Whether
anything can actually *reach* it is up to the host firewall, which no
program should be silently reconfiguring for you.

**Windows.** Inbound is denied by default, especially on networks classified
"Public". Two ways it can end up permitted:

- a *program* rule allowing the specific `python.exe` binary (this is what
  Windows creates when you click "Allow" on its prompt), or
- a *port* rule allowing TCP 8734.

A program rule often already exists — an ESP-IDF install, for instance,
ships its own `python.exe` and Windows may already have been told to allow
it. Be aware that program rules are tied to an exact binary path: running
the service from a **venv**, a different Python install, or a relocated
toolchain means a different `python.exe` with no rule, and it silently stops
being reachable. For a rule that survives that, add a port rule scoped to
the local subnet (elevated PowerShell):

```powershell
New-NetFirewallRule -DisplayName 'token_monitor pc_service' `
  -Direction Inbound -Protocol TCP -LocalPort 8734 `
  -RemoteAddress LocalSubnet -Profile Any -Action Allow
# undo: Remove-NetFirewallRule -DisplayName 'token_monitor pc_service'
```

**Linux.** Depends on the distro — many desktops ship with no active
firewall and need nothing:

```bash
# ufw (Debian/Ubuntu/Raspberry Pi OS) -- only if ufw is active
sudo ufw status
sudo ufw allow from 192.168.1.0/24 to any port 8734 proto tcp

# firewalld (Fedora/RHEL/openSUSE) -- active by default, will block
sudo firewall-cmd --permanent --add-port=8734/tcp --zone=home
sudo firewall-cmd --reload
```

On a cloud VM the provider's security group is an additional layer above
the OS firewall.

**Same-subnet requirement.** The chip and this machine must be on the same
network segment. A PC on Ethernet and a chip on WiFi behind one home router
normally are. Guest WiFi networks, mesh systems in certain modes, and
"client isolation" / "AP isolation" settings deliberately break this, and no
firewall rule fixes it.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `curl` works locally, phone/chip times out | firewall, or different subnet |
| HTTP 503 with `"error"` | never polled successfully; read the message |
| `"stale": true` in a 200 response | serving last-known data; upstream poll is failing |
| `401` in the service log | token expired — run any Claude Code command to refresh it |
| `rate-limited -- backing off` in the log | see below; usually two instances running |
| `Could not bind ...:8734` on startup | an instance is already running — that error is intentional |
| Worked yesterday, not today | Python binary path changed (venv/IDF upgrade) → firewall program rule no longer matches |

## Rate limiting

The usage endpoint returns **HTTP 429** if polled too often, and two
behaviours of it are worth knowing because both were hit during development:

1. It sends `Retry-After: 0` with the 429. Obeying that literally produces a
   hot loop that hammers the server several times a second. `Retry-After` is
   therefore treated as a value the service may only *extend*, never
   shorten — the
   service's own exponential backoff (120s → 240s → 480s → capped at 900s)
   is always the floor.
2. The single most likely cause of an unexpected 429 is **two copies of the
   service running at once**, each polling on its own timer. Check before
   assuming the limit is low:

```powershell
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -like '*service.py*' } |
  Select-Object ProcessId, CommandLine
```

```bash
pgrep -af service.py
```

The backoff resets to its starting value as soon as a poll succeeds. While
rate limited, the service keeps serving the last good data with
`"stale": true`, so the display shows real numbers marked stale rather than
going blank.

The service re-reads the credential file on every poll, so a token refreshed
by Claude Code is picked up automatically without a restart.
