# Changelog

All notable changes to zproject. The version is the single
`project(zproject VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps
it ([semantic versioning](https://semver.org/): patch for fixes and
documentation, minor for features) and adds an entry here. A PR dates its own
heading. Releases are tagged `vX.Y.Z` and minisign-signed (SPEC.md §20,
[docs/RELEASING.md](docs/RELEASING.md)). The first release is v0.4.0.

## 0.7.0 — 2026-11-22

Desktop client, PR 2 of the approved plan: the app shell. `zproject` now opens
a real window wired to the 0.6.0 client core; the data views (outline, Gantt,
resources, ...) and the updater follow. The CLI and server behave as before.

- **Main window and menus:** a project sidebar fed from the cache, a central
  area for the views to come, an inspector dock, and File (Refresh F5,
  Connect to Server…, Preferences, Quit), View (Theme, Zoom In / Zoom Out /
  Actual Size, Sidebar, Inspector, Go to Projects) and Help menus. One window
  per user (`QLockFile` in the cache directory).
- **Themes:** Light, Dark, High contrast and System, built zwriter's way (one
  set of colour tokens, one application style sheet, forced Fusion) with the
  z-suite accent `#2fa8f0`. System follows `QStyleHints::colorScheme()` on Qt
  6.5+ and the platform palette on Qt 6.4. Every theme keeps text at ≥ 4.5:1
  (High contrast ≥ 7:1); a test checks the ratios.
- **UI text zoom:** Ctrl/Cmd + = / − / 0, 50–300 % in zwriter's steps, saved in
  `zproject.ini`. The Gantt zoom moves to Ctrl+Shift + = / − and Ctrl+wheel
  (SPEC.md §8.4).
- **First run:** the server URL comes from `cli.ini`; if `zproject-cli` already
  has a token, the app offers it by its label (`zpt_<id>`) and both share one
  device identity. Otherwise a sign-in dialog takes a pasted device token in
  a password field and stores it where the CLI does (0600 file or keychain, on
  stdin). The secret is never displayed, logged or cached; dialogs show only
  the label. After a 401 the same dialog asks for a new token.
- **Status and offline:** the status bar shows the connection, the server's
  version and the last refresh; a banner says "Offline — showing data from
  <time>" (or the refused token, certificate or API version) with Retry now.
  Editing is off whenever the server isn't answering (`editingAllowed()`),
  and a Read-only marker shows it.
- **Preferences** (Ctrl+, ; Settings on macOS): theme, text size, the offline
  cache on/off (off deletes the copy) and **Clear cache** (only while online:
  deletes the copy and reloads from seq 0), the server and the token label,
  and Change server or token….
- **Help** (zwriter's pattern): F1 opens a modeless guide from an embedded
  Markdown resource with in-page links; external links open only if https.
  Keyboard Shortcuts, API Reference, Report an Issue and About (app, Qt and
  server versions, token label).
- **Keyboard and accessibility:** every action has a shortcut or mnemonic, no
  two share a key, Ctrl+1…6 and the other §8.4 keys stay free for the views,
  and every focusable control has an accessible name.
- **Tests:** `tst_appshell` (offscreen) drives the shell against the
  fault-injecting mock server: themes and contrast, the System fallback,
  zoom and shortcuts, menus, accessible names, online/offline/401 states,
  first-run token secrecy and the 0600 write, Preferences → Clear cache,
  cache off, Help links, About and the lock. With `ZPROJECT_SCREENSHOT_DIR`
  set it renders `docs/screenshots/` from example data.
- **Build:** the shell is a static `zproject_app_lib` linked by `zproject`
  and the test; the desktop app now links the client core.

## 0.6.0 — 2026-11-22

Desktop client, PR 1 of the approved plan: the client core the desktop app
will be built on. No new windows yet; `zproject` still shows its placeholder.
The CLI and server behave as before.

- **`AsyncApiClient`:** a non-blocking `/v1` client on
  `QNetworkAccessManager`. It shares request building with the CLI's client:
  bearer token, no redirects, TLS peer verification always on (there is no
  option to ignore a certificate error), a timeout per request. Up to 4 reads
  at a time; writes to one entity run in order, one at a time; every `PATCH`
  can carry `If-Match`, `?dryRun=true` and `X-Zproject-Confirm`. After a 429
  nothing new starts until `Retry-After`. Nothing is retried blindly.
- **Error mapping (`ApiError`):** every SPEC.md §9.3 answer maps to what the
  app will do with it (inline field error, token dialog, conflict dialog,
  offline with backoff, TLS hard stop, ...). The 0.5.0 refusal to hand-edit a
  summary's progress gets its own message. A 2xx whose body isn't JSON is
  refused.
- **Version gate:** `/v1/health` is checked at connect; another API major is
  refused, and an older or newer server gets an info message.
- **Shared token store:** the token lookup (`ZPROJECT_TOKEN`, the `cli.ini`
  store, the 0600 file, the Keychain) moved from the CLI into
  `resolveDeviceToken()`, used by both. The desktop reuses this machine's CLI
  token and server URL, so a machine set up for `zproject-cli` needs no second
  setup. New desktop settings file `zproject.ini` (0600), never with a token.
- **Read-only cache (`Cache`):** SQLite at `~/.cache/zproject/cache.db`
  (macOS `~/Library/Caches/zproject/`), 0600 in a 0700 directory: projects,
  tasks, WBS codes, the last 5,000 change records and the feed position.
  Replaying records is a no-op; Clear cache deletes the files.
- **Change-feed syncer:** `GET /v1/changes` every 15 s while active, 60 s in
  the background, and on F5; offline backoff 5 s → 60 s; a full reload when
  the server was restored (the cached position no longer matches) or its
  schema changed; list polling every 30 s for project-limited tokens.
- **Tests (headless):** `tst_clientcore`, `tst_cache`, `tst_asyncapi` and
  `tst_syncer`, with a new fault-injecting mock server (`tests/MockServer`:
  delays, hangs, dropped connections, cut-off and truncated bodies, 412, 429,
  5xx, API-version mismatch) and a real `zprojectd` on loopback for end-to-end
  sync, paging and restore detection. TLS tests: an `https://` URL to a plain
  server, and a self-signed certificate (OpenSSL backend), both refused.
- SPEC.md §4.1, §9.3 and §16.5 describe the client core.

## 0.5.0 — 2026-11-22

Summary tasks now follow their children (SPEC.md §5.3). Before this, marking
every child done left the summary where it was (in one plan, children
6.1–6.4 done and summary 6 still `in_progress`, 50 %).

- **% complete** of a summary is weighted by duration over its leaf tasks,
  Σ(% × duration) / Σ duration, the MS Project roll-up. Milestones carry no
  weight; a summary of only milestones uses the plain average. It reads 100
  only when every leaf is done and at least 1 once any work is done.
- **Status:** all children `done` → `done`; all `todo` → `todo`; otherwise
  `in_progress`. Actual start is the earliest child's, actual finish the
  latest once every child has one.
- Recursive through nested summaries, and recomputed in the same transaction
  whenever a task is created, updated, moved or deleted. Each summary that
  changes gets a new revision and its own change record, with the writer's
  token and request id, so history and `/v1/changes` show it. Dry runs list
  the summary updates too.
- **Behaviour change:** setting `percentComplete`, `status`, `actualStart`
  or `actualFinish` on a summary task is refused (400 validation, CLI exit
  3); `tasks done` on a summary now fails unless it is already done. A task
  whose last child is deleted or moved keeps its values and is editable again.
- **Existing data:** `zprojectd` recomputes every summary when it opens the
  database and corrects stale ones as actor `system`, so plans from 0.4.x are
  fixed on the first start of 0.5.0. No schema change.
- `core/Rollup.cpp` holds the rules; new tests in `tst_rollup`, `tst_store`,
  `tst_api` and `tst_cli`. The OpenAPI description of `updateTask` says which
  fields are derived.

## 0.4.2 — 2026-11-22

Fix: on a Mac without Homebrew Qt, the `zproject-cli` in the v0.4.0 `.dmg`
had no TLS backend (`doctor` printed `tls: no`), so it couldn't reach an
`https://` server.

- **Cause:** Qt reads the bundle's `qt.conf` (`Plugins = PlugIns`) only for
  the bundle's main executable. The CLI, in `zproject.app/Contents/MacOS`
  beside the GUI, looked for plugins in `Contents/MacOS/tls`. CI didn't
  notice because the runner's Homebrew Qt supplied its own plugins.
- **Fix:** on macOS, `zproject-cli` adds `Contents/PlugIns` to Qt's library
  paths at startup, before any network use, so it loads the bundled OpenSSL
  backend.
- **New check:** `tools/macos/check-cli-tls.sh` runs the bundled CLI with
  Homebrew's Qt kegs hidden and `QT_PLUGIN_PATH` unset, and requires
  `tls: yes`. CI runs it on the `.dmg`, and so does the release workflow.
- **Workaround for 0.4.0 and 0.4.1:** a `QT_PLUGIN_PATH` wrapper in place of
  the symlink, described in DEPLOY.md. Once 0.4.2 is installed, replace the
  wrapper with the plain symlink.

## 0.4.1 — 2026-11-22

Documentation only.

- `docs/DEPLOY.md`: a step-by-step install for a server host and Linux and
  macOS clients, based on what the packages actually ship. It covers:
  - verifying a release (`sha256sum -c` and minisign);
  - the packaged config and why it is `root:zproject 0640`;
  - creating the owner token straight into a 0600 file;
  - enabling the service and the backup timer, and `tailscale serve`;
  - one 365-day device token per machine, stored with `--store file`
    (0600) and copied with `scp -p`;
  - smoke tests, including a read-only scope check and MCP;
  - upgrade, restore and uninstall.
- README: "Current release" is now **0.4.0**, and the server section links
  to DEPLOY.md.

## 0.4.0 — 2026-11-21

The first usable client, `zproject-cli`, and a release pipeline that ships it
to Linux and macOS (SPEC.md §10, §20).

- `zproject-cli` (new `client/` library) covers the whole `/v1` slice:
  `projects list|show|create|update|delete`, `tasks
  list|show|add|update|done|delete|history` (the list shows WBS codes, with
  milestones marked ◆), `token list|create|revoke`, `audit`, `backup`, plus
  `status` and `doctor`. Every command takes `--json`. Exit codes follow
  SPEC.md §10: 0 ok, 1 error, 2 usage, 3 validation/conflict, 4 auth, 5 rate
  limited (with the `Retry-After` hint). Deletes and confirm-required tokens
  print a dry run until you add `--confirm ID`.
- The URL comes from `--url`, `ZPROJECT_URL` or `~/.config/zproject/cli.ini`.
  It must be https, except for loopback, and must not include a path,
  credentials or query. Redirects are never followed. The CLI checks the
  server's `apiVersion` and refuses a mismatch.
- Tokens never go on the command line. `login` reads one from a file, stdin
  or a prompt that doesn't echo. It validates the token against the server,
  then stores it in the macOS Keychain (via `security -i`, so the token never
  appears in an argv) or in a 0600 file under a 0700 directory on Linux. The
  file store refuses a file anyone else can read, and `--store keychain` on
  Linux uses `secret-tool`. `ZPROJECT_TOKEN` and `ZPROJECT_TOKEN_FILE` serve
  scripts. `token create` writes the new token to a fresh 0600 file
  (`--out-file`), or to stdout only when stdout is not a terminal.
- macOS: `zproject.app` bundles `zproject-cli` (`Contents/MacOS`), with Qt and
  its TLS backend deployed. It is ad-hoc signed and packaged as
  `zproject-<v>-Darwin.dmg` (`cmake/MacBundle.cmake`,
  `tools/macos/check-bundle.sh`). CI builds the .dmg, checks the bundle and
  runs the bundled CLI against a local zprojectd.
- Linux: the `zproject` .deb depends on `libssl3` (Qt's TLS backend) and
  suggests `libsecret-tools`. The install smoke test now drives the installed
  `zproject-cli` against the installed `zprojectd`.
- `.github/workflows/release.yml`: on a `v*` tag it builds and tests on
  Ubuntu 24.04 and macOS 15 and packages both .debs and the .dmg with build
  provenance. It signs `SHA256SUMS` with minisign in the protected `release`
  environment, verifies the signature against `core/UpdateSigningKey.cpp`
  (the key shared with zterminal and ztodo), and creates the GitHub Release.
  See docs/RELEASING.md.
- The desktop placeholder points to `zproject-cli`. The README gains Install
  and CLI login sections.
- clang-tidy now covers `client/`. The SQLite hash test no longer depends on
  how CMake wraps long build paths.

## 0.3.0 — 2026-11-21

zprojectd serves its first slice of the `/v1` API and `/mcp` (SPEC.md §9,
§11, §14, §16.3), with tokens, the audit log, backups and systemd units.

### Added
- Own HTTP/1.1 layer on `QTcpServer` (Qt Core + Network only): keep-alive and
  pipelining; 8 KiB request line, 32 KiB / 64 headers, 1 MiB body; refuses
  `Transfer-Encoding`, bare LF, missing or duplicate `Host`, conflicting
  `Content-Length`, obs-fold and `..` paths; 64 connections, 30 s idle
  timeout. Loopback only (`127.0.0.1:8788`).
- `/v1` REST API, described by `api/openapi.yaml` (OpenAPI 3.1, embedded and
  served at `/v1/openapi.json`): health; projects and tasks (list, search,
  get, create, PATCH, delete, WBS, task history); the change feed; tokens
  (list, create, revoke); the audit log; a backup trigger. Errors are
  `{"error","code","details"}`. `If-Match` gives 412 on a stale revision;
  `?dryRun=true` previews writes; cursor pagination.
- Tokens (`zpt_<id>.<secret>`): only a SHA-256 of the secret is stored and
  compared in constant time; scopes (`read` and `write` bundles, `admin`),
  project allowlists, expiry, revocation, `lastUsedAt`. Agent tokens default
  to read-only, named projects only, 90-day expiry (also the maximum), and
  confirmation for destructive calls; they can never hold `admin` (also
  enforced by a database trigger). `zprojectd --create-owner-token` makes
  the first token (shown once).
- Destructive calls: a dry run returns a single-use `confirmId` (5 minutes,
  bound to the token and the exact request), sent back as
  `X-Zproject-Confirm`.
- Rate limits per token kind (read/write/destructive per minute, with
  per-token overrides) and 10 failed authentications per minute per source;
  429 with `Retry-After`.
- Append-only, hash-chained audit log (migration 0002): every write, every
  agent and app request, MCP tool calls, 401/403/429 and admin calls, with
  before/after and Tailscale identity headers; written in the same
  transaction as the change. `zprojectd --verify-audit` checks the chain.
- `/mcp`: MCP over HTTP (JSON-RPC 2.0, protocol 2025-06-18, also 2025-03-26
  and 2024-11-05). Tools `list_projects`, `get_project`, `search_tasks`,
  `get_task`, `create_task`, `update_task`, `delete_task` (dry run, then
  confirm), filtered by the token's scopes and run through the same checks as
  `/v1`; resources `zproject://openapi` and
  `zproject://projects/{id}/summary`.
- Backups: `zprojectd --backup` (`VACUUM INTO`, `integrity_check`, JSON
  export; keeps 30 daily and the newest of each of 12 months),
  `--restore FILE`, and a snapshot before any schema migration.
- Alerts: backup and audit-verification failures are posted to ts-notify-hub
  (`[alerts] notify_hub_url`; the token is read from a file).
  `zprojectd --notify-test` sends a test alert.
- Configuration file `/etc/zproject/zprojectd.conf` (placeholders only in the
  repository) and `--config`, `--listen`, `--db`, `--allowed-host`,
  `--backup-dir`.
- Packaging: `zprojectd.service` (hardened; `MemoryMax=512M`,
  `CPUWeight=50`, `IOWeight=50`), `zprojectd-backup.service` and
  `zprojectd-backup.timer` (03:17 nightly, `RandomizedDelaySec=10m`), the
  config as a conffile, and a postinst that creates the `zproject` user and
  `/var/lib/zproject` (mode 0700). Nothing is enabled or started on install.
- Tests: `tst_httpparser`, `tst_httpserver` (real sockets), `tst_auth`,
  `tst_audit`, `tst_backup`, and `tst_api` (the OpenAPI contract, scopes,
  allowlists, expiry, revocation, confirm, rate limits, audit, MCP).
- CI: an "OpenAPI lint" job (`@redocly/cli` 2.77.0, and `api/openapi.json` is
  checked against the YAML); the install smoke test now runs
  `systemd-analyze verify` on the units, checks the postinst, and runs the
  server as the `zproject` user against `/v1` and `/mcp`.

### Changed
- `store::Transaction` is public and nests with savepoints.

## 0.2.2 — 2026-11-20

SQLite is fetched at build time instead of vendored.

### Changed
- The SQLite 3.54.1 amalgamation is no longer in the repository
  (`third_party/sqlite/` removed). `cmake/FetchSQLite.cmake` downloads
  `sqlite-amalgamation-3540100.zip` from sqlite.org at configure time and
  checks its SHA3-256 and SHA-256, and the SHA3-256 of the extracted
  `sqlite3.c` and `sqlite3.h`, against pinned values; any mismatch is a fatal
  configure error. Compile options are unchanged (`cmake/SQLite.cmake`).
- Offline and cached builds: the verified zip is kept in `~/.cache/zproject`
  (`$XDG_CACHE_HOME/zproject`, or `ZPROJECT_SQLITE_CACHE`);
  `-DSQLITE_SOURCE_DIR=<dir>` uses already extracted sources (still
  hash-checked); `-DZPROJECT_SQLITE_OFFLINE=ON` never downloads.
- CI restores the download cache with `actions/cache` in every building job.
- CI comments name the server host generically.

### Added
- ctest `sqlite_hash_verification`: a cached zip with the wrong hash, a
  tampered `SQLITE_SOURCE_DIR` and an empty cache with downloads off are all
  refused; verified sources are accepted.
- CI step "Wrong SQLite hash fails the configure" (Linux job): a full configure
  against a cached zip with one byte appended must fail with a SHA3-256
  mismatch.

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
