# Changelog

All notable changes to zproject. The version is the single
`project(zproject VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps
it ([semantic versioning](https://semver.org/): patch for fixes and
documentation, minor for features) and adds an entry here. A PR dates its own
heading. Releases will be tagged `vX.Y.Z` and minisign-signed (SPEC.md §20);
there is no release yet.

## 0.2.1 — 2026-11-20

Dependabot for the pinned GitHub Actions, as in zterminal.

### Added
- `.github/dependabot.yml`: weekly `github-actions` updates with a 7-day
  cooldown; minor and patch updates grouped into one PR, commit prefix `ci`.
- `.github/workflows/dependabot-auto-merge.yml`: turns on GitHub auto-merge for
  a Dependabot PR whose updates are all minor or patch; branch protection on
  `main` holds the merge until the required checks pass. A major update is left
  for review.

## 0.2.0 — 2026-11-20

Phase 1a, first part: the store (SPEC.md §5, §16).

### Added
- Vendored SQLite 3.54.1 amalgamation (`third_party/sqlite/`, unmodified;
  SHA3-256 of the zip checked against sqlite.org and recorded in
  `README.zproject.md`). zproject uses it directly, not QtSql.
- `zproject_store` (`store/`): `Database`/`Statement` RAII wrappers; the
  connection settings of §16.1 (WAL for files, `synchronous=NORMAL`,
  `foreign_keys=ON`, `busy_timeout=5000`); numbered, forward-only migrations
  in `store/migrations/` embedded at build time and applied in one
  transaction (`PRAGMA user_version`), rolled back completely on failure; a
  database newer than the build is refused.
- Schema 1: `meta` (installation id, currency USD), `project`, `task` (WBS via
  `parent` + order), the append-only `changes` and `audit` tables (triggers
  reject `UPDATE` and `DELETE`). Table `CHECK`s and foreign keys back the
  model rules.
- `Store` interface and `SqliteStore`: create/read/list/update/delete for
  projects and tasks with `rev` (If-Match, `rev_mismatch`), tombstone deletes
  (a project or task takes its subtree with it), no-op updates that keep the
  revision, a change row per entity per write (changed fields before/after,
  actor, request id, hybrid-clock timestamp), and the `changesSince` sync
  feed. Task tree rules: parent in the same project and live, no milestone
  parents, no cycles (`cycle` error with the path), a task with children
  cannot become a milestone, tasks cannot move between projects.
- `core/`: `Error`/`Result` (the API error codes of §9.3), `Project` and
  `Task` with validation (§5.2: milestone iff duration 0, ASAP/SNET/FNLT
  constraint dates, % complete and actuals, dates, priority, RAG, budget,
  colour) and JSON with the API's field names.
- `zprojectd --db FILE --migrate-only`; `--version` also prints the schema
  version.
- Tests: `tst_model`, `tst_migrations` (each migration applied step by step,
  connection settings, re-open, newer schema refused, failed batch rolled
  back), `tst_store`, and `--migrate-only` run twice on a fresh file.

## 0.1.0 — 2026-11-20

Phase 0: repository skeleton (SPEC.md §24).

### Added
- `SPEC.md`: the approved specification (locked 2026-11-20; zprojectd host
  host changed at 3:59 PM MST; repository and build approved by Stephen
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

### Changed (during review)
- The public `SPEC.md` uses placeholders (`<server-host>`, `<tailnet>.ts.net`,
  `<notify-hub-url>`, `<backup-dir>`, `<offsite-backup-target>`) instead of
  concrete host names, tailnet addresses and backup locations.

### Not yet (later PRs)
- macOS `.dmg` and bundle checks, fuzzing, OpenAPI lint, systemd units and
  `systemd-analyze verify`, the release workflow and its dry run.
