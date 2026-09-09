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

## Finding this machine's address

The gadget needs the PC's address on the local network, and there are three
ways to get it — in increasing order of usefulness.

**Just read it off this service.** Since it knows, it now says so at startup:

```
14:45:32  serving GET /usage on 0.0.0.0:8899  (Ctrl-C to stop)
14:45:32  reachable on this network at 192.168.1.87:8899 -- that is the address to give the gadget
```

That second line is the value to type into the setup form (or into
`firmware/main/secrets.h`). It is worked out by asking the OS which interface
it would use to reach the outside world — not by resolving the hostname, which
on a machine with a VM host network, a VPN or WSL commonly answers with the
wrong adapter. Nothing is sent to reach that answer. Started with
`--host 127.0.0.1`, it says instead that it is local-only and the gadget cannot
reach it at all.

**Windows, without this service running:**

```powershell
# just the adapter that carries internet traffic
(Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway }).IPv4Address.IPAddress

# or the old way, and read the "IPv4 Address" under your active adapter
ipconfig
```

`ipconfig` lists every adapter, including virtual ones from VirtualBox, Hyper-V
or WSL, which is exactly the confusion the first command avoids. Ignore
anything starting `169.254.` — that is a link-local address, meaning the
adapter never got a real one.

**Linux:** `ip -4 addr show` for everything, or
`ip route get 1.1.1.1 | awk '{print $7; exit}'` for the same
which-interface-would-I-use answer the service gives.

The address you want is the one on the same network as the gadget — normally
`192.168.x.y` or `10.x.y.z`.

**But the real answer is not to look it up at all**, because it moves: this
machine was `192.168.1.87`, then `.88`, then `.87` again across two days of
testing. Two ways to stop caring:

- **Change it from the gadget when it moves.** Hold the setup button for three
  seconds, join the hotspot, put the new address in the form. Ninety seconds,
  no cable. This is what the provisioning work exists for.
- **Stop it moving at all** — a DHCP reservation on the router, described next.
  This is the permanent fix, and it is worth doing once.

## Keeping the address stable

The firmware finds this service through `PC_SERVICE_HOST` in
`firmware/main/secrets.h`, and on most home networks that address is
DHCP-assigned — so it can change with no warning and no obvious cause. That is
not a hypothetical: it happened during testing, mid-session. The router
reassigned the PC by one digit, every fetch began failing with
`ESP_ERR_HTTP_CONNECT`, and WiFi was perfectly healthy throughout. The symptom
points at the service or the firewall; the cause was neither.

**The fix is a DHCP reservation** — a router setting that pins one address to
one device permanently. No code, no rebuild, and it holds across reboots on
both sides. Two details decide whether it actually works:

- **Reservations are keyed on MAC address**, and a PC's WiFi adapter has a
  *different* MAC from its Ethernet adapter. Reserve the interface the machine
  will actually be using. If it might use either, reserve both — they will get
  different addresses, and `secrets.h` has to name the one in use.
- **The medium is irrelevant to the router**, so this works the same for a
  wired or a wireless host.

**Why not a hostname instead?** It was tried, and it does not work on the
network this was built on: the chip fails with esp-tls error `32769`,
`CANNOT_RESOLVE_HOSTNAME` — it never gets an address at all. The router
resolves the name correctly when queried directly, so the name is fine; the
likeliest cause is that the chip's DHCP-supplied DNS server is not the router
but a public resolver, which knows nothing of LAN names. No suffixed form
(`.lan`, `.home`, `.local`) resolved either. It may well work on a router that
hands out itself as the resolver, so it is worth one test before doing the
reservation work — but test it *from the chip*, not from the PC. Windows'
`Resolve-DnsName` answers from its own local resolution unless you pass
`-DnsOnly -NoHostsFile`, and will cheerfully tell you the name works when the
chip cannot resolve it at all.

## Moving the service to a different PC

Five things change, and the first is the one people forget:

1. **Claude Code must be installed and logged in on the new machine.** This
   service reads the OAuth token from Claude Code's own credential file on the
   host — no Claude Code, no token, no data. See the platform table above for
   where that file lives.
2. **The address changes.** Update `PC_SERVICE_HOST` in `secrets.h`, rebuild
   and reflash.
3. **The firewall rule does not come with you.** It is per-program on Windows,
   so the new machine needs its own rule for its own `python.exe` — or a port
   rule, which survives interpreter changes.
4. **Check the subnet**, and if the new host is on WiFi rather than cable,
   check AP isolation before anything else. Chip and host both being wireless
   is exactly the case client isolation blocks, and nothing in software works
   around it. A phone on the same WiFi loading
   `http://<new-pc-ip>:8734/usage` is the quickest test.
5. **Reserve the new address** once it works, per the section above.

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

## Testing the firmware's "no data" path (`tools/stub_503.py`)

The firmware has to handle a **503** — `pc_service` up, but no successful
poll yet, so there is no data to serve even as stale. That state is real but
awkward to catch on purpose, since the service leaves it within a poll or
two of starting.

`tools/stub_503.py` stands in for the service and answers every request with
the documented 503 shape, so the firmware's branch can be exercised on
demand. Stop `service.py` first — the stub binds the same port:

```
python tools/stub_503.py
```

Run it with the **same `python.exe`** the real service uses. The Windows
firewall rule that lets the ESP32 reach this machine is per-program, so a
different interpreter is silently blocked and the chip sees a 10-second
timeout instead of the 503 you meant to test.
