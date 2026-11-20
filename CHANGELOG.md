# Changelog

All notable changes to zproject. The version is the single
`project(zproject VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps
it ([semantic versioning](https://semver.org/): patch for fixes and
documentation, minor for features) and adds an entry here. A PR dates its own
heading. Releases will be tagged `vX.Y.Z` and minisign-signed (SPEC.md §20);
there is no release yet.

## 0.1.0 — 2026-11-20

Phase 0: repository skeleton (SPEC.md §24).

### Added
- `SPEC.md`: the approved specification (locked 2026-11-20; zprojectd host
  changed to dragon at 3:59 PM MST; repository and build approved by Stephen
  at 4:02 PM MST, §25 decision 8).
- Icon concept B: `assets/icon/zproject.svg` (master) and PNGs at 16, 24, 32,
  48, 64, 128, 256, 512 and 1024 px (`scripts/render-icons.sh`).
- CMake skeleton (C++20, Qt 6.4 minimum): `zproject_core` (`core/`: version,
  20-hex entity ids), `zproject_server_lib` + `zprojectd` (`server/`: listen
  address parsing that accepts loopback only, default `127.0.0.1:8788`),
  `zproject` desktop placeholder window (`app/`), `zproject-cli` (`cli/`).
  Options `ZPROJECT_WERROR`, `ZPROJECT_BUILD_GUI`, `ZPROJECT_BUILD_TESTS`,
  `ZPROJECT_SANITIZE`.
- Qt Test harness (`tests/`) and `--version` checks for every binary.
- Packaging: `zproject_<ver>_amd64.deb` (desktop + CLI, `.desktop` file,
  hicolor icons) and `zprojectd_<ver>_amd64.deb` (server only, no Qt Widgets
  dependency) from one CPack build.
- `api/openapi.yaml`: OpenAPI 3.1 stub with `GET /v1/health`.
- CI (`ci.yml`, actions pinned by SHA): Version consistency; Version increase
  (PRs); Linux (ubuntu-24.04, distro Qt 6.4) build + test + both `.deb`s +
  install smoke test in an `ubuntu:24.04` container; macOS arm64 build + test;
  Sanitizers (ASan + UBSan); clang-tidy.

### Not yet (later PRs)
- macOS `.dmg` and bundle checks, fuzzing, OpenAPI lint, systemd units and
  `systemd-analyze verify`, the release workflow and its dry run.
