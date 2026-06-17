# Installation

`ipsc2hbpc` is a single C binary with **no external dependencies** — it links
only against the C standard library.  Tested on Debian/Ubuntu; adapt paths for
other distros.

## Requirements

- A C11 compiler (`gcc` or `clang`) and `make`
- git

That's it.  There is no Python, no venv, no `pip`, and no third-party libraries.
The TOML parser, SHA-1/SHA-256/HMAC, the event loop, and the DMR DSP/FEC code
are all included in the repository and compiled into the binary.

## 1 — Clone and build

```
git clone https://github.com/n0mjs710/ipsc2hbpc.git
cd ipsc2hbpc
make
```

This produces the `ipsc2hbpc` binary in the repo root.

## 2 — Run the self-tests (optional but recommended)

```
make test
```

This validates the DMR DSP/FEC core against golden vectors and checks the
translator output byte-for-byte against the reference implementation.

## 3 — Install (as root)

```
sudo make install
```

This follows the usual system conventions:

| Item    | Destination                          |
|---------|--------------------------------------|
| binary  | `/usr/local/bin/ipsc2hbpc`           |
| config  | `/etc/ipsc2hbp/ipsc2hbp.toml`        |
| sample  | `/etc/ipsc2hbp/ipsc2hbp.toml.sample` |
| service | `/lib/systemd/system/ipsc2hbpc.service` |

The service runs as **root**.  Install paths are overridable
(`make install PREFIX=/usr SYSCONFDIR=/etc UNITDIR=/etc/systemd/system`).

**Your live config is never clobbered.** `make install` writes
`/etc/ipsc2hbp/ipsc2hbp.toml` only if it does not already exist; on every run it
refreshes `ipsc2hbp.toml.sample` next to it. Re-running `sudo make install` after
a `git pull` updates the binary and unit while leaving your config untouched.

## 4 — Configure

Edit `/etc/ipsc2hbp/ipsc2hbp.toml` for your IPSC and HBP settings.  The file
format is **identical** to the original Python `ipsc2hbp` — an existing
`ipsc2hbp.toml` can be copied in unchanged.

You can test against the config manually before enabling the service:

```
/usr/local/bin/ipsc2hbpc -c /etc/ipsc2hbp/ipsc2hbp.toml --log-level DEBUG
```

Hit Ctrl-C to stop.

> **IMPORTANT — if the original Python `ipsc2hbp` is running on this host:**
> The two cannot share the IPSC UDP port or the upstream HBP connection.  Stop
> and disable the Python service before starting `ipsc2hbpc`:
>
> ```
> sudo systemctl stop ipsc2hbp
> sudo systemctl disable ipsc2hbp
> ```
>
> The C unit declares `Conflicts=ipsc2hbp.service`, so systemd will also refuse
> to run both bridges at once.

## 5 — Enable the service

```
sudo systemctl enable --now ipsc2hbpc
```

## 6 — Check the logs

```
journalctl -u ipsc2hbpc -f
```

## Updating

```
git pull
make
sudo make install          # updates binary + unit, preserves your config
sudo systemctl restart ipsc2hbpc
```

## Uninstalling

```
sudo make uninstall        # removes binary + unit; leaves /etc/ipsc2hbp intact
```

## Development runs

When working in the cloned repo, run the freshly built binary against a local
config without installing:

```
make
./ipsc2hbpc -c ipsc2hbp.toml --log-level DEBUG
```
