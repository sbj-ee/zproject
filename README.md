# zproject

**0.1.0**

Current release: none yet

Project management for the z-suite: projects with a WBS, milestones,
dependencies, a critical-path schedule, baselines, resources and cost, a RAID
log, reports and alerts, with a Gantt chart at its centre. Three programs from
one repository (C++20, Qt 6):

| Program | What it is |
|---|---|
| `zprojectd` | Headless server (Qt Core/Network, SQLite). Listens on `127.0.0.1:8788` only and is published on the tailnet with `tailscale serve`. Runs on dragon (Pop!_OS 24.04, amd64). |
| `zproject` | Desktop client (Qt Widgets) for Linux amd64 and macOS on Apple Silicon. |
| `zproject-cli` | Thin command-line client of the `/v1` API. |

The full design is in [SPEC.md](SPEC.md).

## Status

Phase 0: the build skeleton, CI and packaging. The binaries print their
version and nothing more yet; the server store and API arrive in phase 1a
(SPEC.md §24). Nothing is released.

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

## Version bumps

The version is the single `project(zproject VERSION x.y.z)` in
`CMakeLists.txt`. Every pull request bumps it (patch for fixes and
documentation, minor for features) and adds a dated `## x.y.z — YYYY-MM-DD`
entry to [CHANGELOG.md](CHANGELOG.md); the README header and the version line
at the top of SPEC.md follow it. CI checks both (`scripts/check-version.sh`,
`scripts/check-version-increase.sh`).

## License

[MIT](LICENSE). Icon: concept B, `assets/icon/zproject.svg`.
