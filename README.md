# zproject

**0.3.0**

Current release: none yet

Project management for the z-suite: projects with a WBS, milestones,
dependencies, a critical-path schedule, baselines, resources and cost, a RAID
log, reports and alerts, with a Gantt chart at its centre. Three programs from
one repository (C++20, Qt 6):

| Program | What it is |
|---|---|
| `zprojectd` | Headless server (Qt Core/Network, SQLite). Listens on `127.0.0.1:8788` only and is published on the tailnet with `tailscale serve`. Runs on a home server (Pop!_OS 24.04, amd64). |
| `zproject` | Desktop client (Qt Widgets) for Linux amd64 and macOS on Apple Silicon. |
| `zproject-cli` | Thin command-line client of the `/v1` API. |

The full design is in [SPEC.md](SPEC.md).

## Status

Phase 1b: `zprojectd` serves the first slice of the `/v1` API (projects,
tasks, WBS, history, the change feed, tokens, the audit log, backups) and an
MCP endpoint at `/mcp`, with scoped tokens, rate limits and a hash-chained
audit log. Dependencies, the scheduling engine, resources, baselines, the RAID
log, reports and the desktop client follow (SPEC.md §24). Nothing is
released.

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

The owner token is printed once; keep it in a password manager. Other
tokens are made through the API:

```sh
curl -H "Authorization: Bearer $ZPROJECT_TOKEN" -H 'Content-Type: application/json' \
  -d '{"name":"assistant","kind":"agent","projects":["<project-id>"]}' \
  https://<server-host>.<tailnet>.ts.net/v1/tokens
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
