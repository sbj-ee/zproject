# zproject

**0.4.0**

Current release: none yet

Project management for the z-suite: projects with a WBS, milestones,
dependencies, a critical-path schedule, baselines, resources and cost, a RAID
log, reports and alerts, with a Gantt chart at its centre. Three programs from
one repository (C++20, Qt 6):

| Program | What it is |
|---|---|
| `zprojectd` | Headless server (Qt Core/Network, SQLite). Listens on `127.0.0.1:8788` only and is published on the tailnet with `tailscale serve`. Runs on a home server (Pop!_OS 24.04, amd64). |
| `zproject` | Desktop client (Qt Widgets) for Linux amd64 and macOS on Apple Silicon. A placeholder window for now. |
| `zproject-cli` | Command-line client of the `/v1` API, shipped with the desktop app (Linux `.deb`, inside `zproject.app` on macOS). |

The full design is in [SPEC.md](SPEC.md).

## Status

Phase 1c: `zprojectd` serves the first slice of the `/v1` API (projects,
tasks, WBS, history, the change feed, tokens, the audit log, backups) and an
MCP endpoint at `/mcp`, with scoped tokens, rate limits and a hash-chained
audit log. `zproject-cli` covers all of it from Linux and macOS. The release
workflow builds signed `.deb` and `.dmg` packages (see [Install](#install)).
Dependencies, the scheduling engine, resources, baselines, the RAID log,
reports and the desktop client follow (SPEC.md §24).

## Running the server

The `zprojectd` package installs the binary, `/etc/zproject/zprojectd.conf`,
and three systemd units; its postinst creates the `zproject` user and
`/var/lib/zproject`, and enables nothing. On the server host:

```sh
sudoedit /etc/zproject/zprojectd.conf        # allowed_hosts, notify_hub_url
sudo -u zproject zprojectd --config /etc/zproject/zprojectd.conf --create-owner-token
sudo systemctl enable --now zprojectd.service zprojectd-backup.timer
tailscale serve --bg --https=443 http://127.0.0.1:8788
```

`allowed_hosts` must list the server's MagicDNS name
(`<server-host>.<tailnet>.ts.net`); the server refuses any other `Host`. The
owner token is printed once. Rather than putting it on screen, pipe it
straight into the CLI on the server host (see [Install](#install)), and keep
a copy in a password manager. Other tokens are made with
`zproject-cli token create` or through the API:

```sh
zproject-cli token create --kind agent --name assistant --projects <project-id> --out-file assistant.token
```

An agent token is read-only, limited to the named projects, expires after 90
days and needs a dry run plus `X-Zproject-Confirm` for deletes, unless the
request asks for more (`scopes: ["write"]`, `expiresInDays` up to 90,
`requireConfirm`); it can never be `admin`.

- REST: [`api/openapi.yaml`](api/openapi.yaml) (also served at
  `/v1/openapi.json`). `?dryRun=true` previews a write; `If-Match: <rev>`
  refuses a stale update with 412.
- MCP: point an MCP client at `https://<server-host>.<tailnet>.ts.net/mcp`
  with `Authorization: Bearer <token>` (streamable HTTP, JSON responses).
- Backups: `zprojectd-backup.timer` runs `zprojectd --backup` at 03:17 into
  `/var/lib/zproject/backups`; `zprojectd --restore FILE` (service stopped)
  restores one. `zprojectd --verify-audit` checks the audit chain.
- Alerts: set `[alerts] notify_hub_url` to the ts-notify-hub URL and put its
  token in `/etc/zproject/notify-hub.token` (`root:zproject`, mode 0640);
  `zprojectd --notify-test` sends a test alert.

## Install

Each release has `zproject_<v>_amd64.deb` (desktop app and `zproject-cli`,
Ubuntu/Pop!_OS 24.04), `zprojectd_<v>_amd64.deb` (server),
`zproject-<v>-Darwin.dmg` (macOS on Apple Silicon, ad-hoc signed),
`SHA256SUMS`, and `SHA256SUMS.minisig`. Before installing, check them:

```sh
gh release download vX.Y.Z -R sbj-ee/zproject
sha256sum -c --ignore-missing SHA256SUMS          # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
minisign -Vm SHA256SUMS -P RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9
```

The key is the one in [`core/UpdateSigningKey.cpp`](core/UpdateSigningKey.cpp)
and [docs/RELEASING.md](docs/RELEASING.md).

- Linux: `sudo apt install ./zproject_<v>_amd64.deb` (and `./zprojectd_<v>_amd64.deb` on the server).
- macOS: open the `.dmg` and drag `zproject.app` to Applications. The app is
  ad-hoc signed, not notarized, so the first launch needs right-click → Open.
  To put the CLI on your `PATH`, run
  `ln -s /Applications/zproject.app/Contents/MacOS/zproject-cli /usr/local/bin/zproject-cli`.

### Connecting the CLI

`zproject-cli` needs the server URL and a token. The URL is
`https://<server-host>.<tailnet>.ts.net`, the address `tailscale serve`
publishes, or `http://127.0.0.1:8788` on the server host itself. Plain http
is accepted only for loopback. The token never goes on the command line.
`login` reads it from a file, from stdin, or from a prompt that doesn't echo,
checks it against the server, and stores it:

- macOS: in the login Keychain (`--store keychain`, the default; service
  `ee.sbj.zproject`).
- Linux: in `~/.config/zproject/token`, mode 0600 (`--store file`, the
  default). The CLI refuses to read the file if anyone else can read it. With
  `libsecret-tools` installed, `--store keychain` uses the Secret Service
  instead.

The URL and store choice go into `~/.config/zproject/cli.ini`.
`ZPROJECT_URL`, `ZPROJECT_TOKEN` (for scripts) and `ZPROJECT_TOKEN_FILE`
override them.

Use one token per machine. On the server host, log in with the owner token
without ever displaying it, then make a device token for each other machine:

```sh
sudo -u zproject zprojectd --config /etc/zproject/zprojectd.conf --create-owner-token \
  | zproject-cli login --url http://127.0.0.1:8788 --stdin
zproject-cli token create --kind device --name <machine> --expires 365d --out-file <machine>.token
```

`token create` writes a new file with mode 0600. It prints the token only with
`--stdout`, and refuses even then if stdout is a terminal. Copy the file to
the machine (with `tailscale file cp` or `scp`), then on that machine:

```sh
zproject-cli login --url https://<server-host>.<tailnet>.ts.net --from-file <machine>.token
rm <machine>.token
zproject-cli status
```

`zproject-cli doctor` shows the config and TLS support. Run
`zproject-cli --help` for the commands: projects, tasks, tokens, audit and
backup, with `--json` output throughout. Deletes are dry runs until you add
`--confirm ID`. To revoke a lost machine's token, run
`zproject-cli token revoke <token-id>` (as owner) and check the result with
`zproject-cli token list`.

## Building

Linux (Ubuntu 24.04 / Pop!_OS 24.04, distro Qt 6.4):

```sh
sudo apt install qt6-base-dev cmake ninja-build g++ dpkg-dev file
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
(cd build && cpack -G DEB)   # zproject_<ver>_amd64.deb and zprojectd_<ver>_amd64.deb
```

macOS (Apple Silicon, Homebrew Qt):

```sh
brew install qt cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build && ctest --test-dir build --output-on-failure
(cd build && cpack -G DragNDrop)   # zproject-<ver>-Darwin.dmg
```

CMake options: `ZPROJECT_WERROR` (warnings are errors; CI sets it),
`ZPROJECT_BUILD_GUI` (off for a server-only build), `ZPROJECT_BUILD_TESTS`,
`ZPROJECT_SANITIZE=address,undefined`.

The code targets Qt 6.4 as its minimum: use no Qt API newer than 6.4.

SQLite is not in the repository. The first configure downloads the
amalgamation zip from sqlite.org and checks it against the SHA3-256 and
SHA-256 pinned in [`cmake/FetchSQLite.cmake`](cmake/FetchSQLite.cmake); a
mismatch stops the build. The zip is cached in `~/.cache/zproject` (or
`$XDG_CACHE_HOME/zproject`, or `$ZPROJECT_SQLITE_CACHE`), so later builds work
offline. For a build with no network: put the zip in the cache, or pass
`-DSQLITE_SOURCE_DIR=<dir>` with an extracted `sqlite3.c`/`sqlite3.h` (each is
still hash-checked); `-DZPROJECT_SQLITE_OFFLINE=ON` forbids the download.

## Version bumps

The version is the single `project(zproject VERSION x.y.z)` in
`CMakeLists.txt`. Every pull request bumps it (patch for fixes and
documentation, minor for features) and adds a dated `## x.y.z — YYYY-MM-DD`
entry to [CHANGELOG.md](CHANGELOG.md); the README header and the version line
at the top of SPEC.md follow it. CI checks both (`scripts/check-version.sh`,
`scripts/check-version-increase.sh`).

## License

[MIT](LICENSE). Icon: concept B, `assets/icon/zproject.svg`.
