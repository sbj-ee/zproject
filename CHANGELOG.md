# Changelog

All notable changes to zproject. The version is the single
`project(zproject VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps
it ([semantic versioning](https://semver.org/): patch for fixes and
documentation, minor for features) and adds an entry here. A PR dates its own
heading. Releases are tagged `vX.Y.Z` and minisign-signed (SPEC.md §20,
[docs/RELEASING.md](docs/RELEASING.md)). The first release is v0.4.0.

## 0.16.0 — 2026-11-23

Desktop client plan, PR 11: packaging and docs polish (plan §6; SPEC §18,
§19, §20). This completes the desktop client plan's eleven PRs.

- **`zproject --smoke-test`**: builds the real main window in a throwaway
  home, config and cache (it never reads the user's settings, connects, or
  checks for updates), shows it, checks the platform plugin, the Fusion
  style, the bundled help and icon, the menus and views, and exits 0 with
  "smoke test: ok". It runs as a ctest, from the mounted `.dmg` in the
  macOS CI job (the bundle's own cocoa plugin), and from the installed
  `.deb` in the Ubuntu 24.04 container.
- **`.deb`**: the desktop package now depends on `qt6-qpa-plugins`. Qt's
  platform plugins (xcb, offscreen) are loaded at run time, so
  `dpkg-shlibdeps` never listed them; without one the app cannot open a
  window. CI checks the dependency.
- **Accessibility pass** (`tst_accessibility`): on every view (details, all
  projects, outline, Gantt, milestones, activity) and in the task editor,
  every control Tab reaches has a name a screen reader can read; every menu
  item has text and every toolbar button a name; every listed shortcut is a
  real action and no key is used twice. It found one gap, now fixed: the
  task editor's Details and Schedule tabs were Tab stops with no name.
- **docs/DEPLOY.md**: the `.deb` row no longer calls the app a placeholder;
  new section 5.1 on the desktop app (first run with the CLI's token,
  updates, the offline cache, where settings and cache live, Clear cache);
  `--smoke-test` in the client steps; uninstall also removes the app's
  settings and cache. README points to it.

## 0.15.0 — 2026-11-23

Desktop client plan, PR 10: project activity (plan §3.6; SPEC §8.3, §18).
Without the audit-log viewer: device tokens keep no `audit:read` (plan Q7).

- **View → Activity** (Ctrl + Shift + A, and an Activity button on the
  toolbar after Milestones): the project's changes, newest first, from the
  change records the cache already keeps (no extra request; works
  offline). Columns: when (local time), who, the item ("1.2 Drawings" or
  "Project details") and what changed.
- **Who**: "This computer" for this device's token, "system" for the
  server's roll-ups, otherwise the token label (whole label in the
  tooltip). **Filters**: by who, and by tasks or project details; both, and
  the selection, survive refreshes.
- **Enter** (or a double-click) on a task's row opens it in the outline. A
  deleted task keeps the last title its records carry, marked "(deleted)".
- Each row is described in words for screen readers. The note under the
  list counts the changes and says when only the newest 1,000 are shown.
- With a project-limited token (no change feed) the view says so and
  points to the task's History tab.
- The History tab and the activity view now share one wording of a change
  (`app/ChangeText`), which also names a created project by its name
  rather than listing every field.
- A new Activity icon (a clock face). Help has an Activity section and the
  new shortcut.

## 0.14.0 — 2026-11-23

Desktop client plan, PR 9: the milestones view (plan §3.5; SPEC §7.1, §8.3,
§18).

- **View → Milestones** (Ctrl + Shift + M, and a Milestones button on the
  toolbar after Gantt): the selected project's milestones by date, with WBS
  code, date, deadline, status, the summary each sits under, and how far
  away it is ("In 4 days", "Today", "6 days late", "Done"). Milestones with
  no date come last; the note under the list counts the late and undated
  ones.
- **All projects**: one list of every project's milestones, with a Project
  column. Done and cancelled projects are included only when the sidebar
  shows them.
- **Jump to it**: Enter (or a double-click) opens the milestone in the
  outline, switching project if needed; Show in Gantt opens it on the
  timeline. An outline status filter that would hide it is cleared.
- Late milestones and dates after the deadline are marked ⚠ and in bold,
  and say so in words; each row has an accessible description ("Milestone
  3 Handover, Kitchen remodel, 20 Nov 2026, in 50 days, after its deadline
  of 18 Nov 2026"). A new Milestones icon (two diamonds on a line).
- The list is read-only and follows refreshes; it is built from the cache,
  so it also works offline.
- The project details page no longer says the Gantt "arrives in a later
  release".
- Help has a Milestones section and the new shortcut.

## 0.13.2 — 2026-11-23

- SPEC.md §22: `scripts/check-version.sh` also checks `api/openapi.yaml`
  `info.version`, which follows the product version while the API stays
  `/v1` (`apiVersion` 1).

## 0.13.1 — 2026-11-23

Documentation and version drift fixes.

- README: **Current release** is 0.13.0 (it still said 0.12.0 after v0.13.0
  was published).
- README: the Status section said "Phase 1c", which SPEC.md §24 defines as
  baselines, resources, RAID and templates, none of which exist yet. What is
  built is phase 1a (without dependencies, calendars or CPM) and phase 1b, so
  it now says that.
- README: the program table lists the task editor and the read-only Gantt.
- `api/openapi.yaml` `info.version` was still 0.3.0, though the move and
  restore endpoints (0.9.0) and later changes are in the document. It now
  follows the product version, and `scripts/check-version.sh` fails CI when
  the two differ. `api/openapi.json` is regenerated.

## 0.13.0 — 2026-11-23

Desktop client plan, PR 8: the read-only Gantt (plan §3.4; SPEC §8.2 v1
subset, §17, §18).

- **View → Gantt** (Ctrl + G, and a Gantt button on the toolbar after
  Outline): a task table (WBS, title, duration, start, finish, % complete)
  and a custom-painted timeline that share one read-only row model and scroll
  together; collapsing a summary hides its rows in both.
- **Timescales** Day, Week, Month and Quarter (the Timescale box,
  Ctrl + Shift + = / − or Ctrl + wheel while the Gantt has focus;
  Shift + wheel scrolls sideways), weekends shaded at day and week scale, a dashed **today line**
  and Today / **T** to scroll to it.
- **Bars from the stored dates**: auto tasks solid, manual tasks outlined,
  summary brackets, milestone diamonds, % complete as a band inside the bar,
  and deadline flags (filled red when the finish is past the deadline).
  Tasks with no dates say "unscheduled" in the table and have no bar.
- **Keyboard**: arrows move the selection and the inspector follows; Enter
  (or a double-click on a bar) opens the task editor. The Gantt edits
  nothing itself.
- **Accessible bars**: the timeline is an accessible list whose items are
  the bars, named and described in words ("Task 1.2 Drawings" — "3 days,
  9–11 Sep, 40% complete, manual dates"), the same text as the tooltips.
- Drawn with the app's own palette, so it follows Light, Dark and High
  contrast; only visible rows are painted. A 2,000-row benchmark test
  (≤ 16 ms a frame) runs in CI.
- **Title bar**: "zproject <version> — <project name>", or "zproject
  <version>" with no project, on Linux and macOS, from the build's version
  (the one --version and About show); unsaved task edits mark it with *.
- Help has a Gantt section and the new shortcuts.
- Works around Qt 6.4 keeping a hidden header section's width after a model
  reset (the Gantt's table re-hides its columns on reload).

Not yet (later PRs, per the plan): dragging, linking, dependency arrows,
critical path, baselines, over-allocation and RAID badges, the calendar
lane, project calendars, and print/export.

## 0.12.0 — 2026-11-22

Desktop client plan, PR 7: the task editor (plan §3.3, §3.6, §4.4, §4.5).
Selecting a task in the outline opens it for editing in the inspector.

- **Details tab**: title, type (task or milestone), mode, status,
  % complete, priority and Markdown notes with a plain preview (links are
  not followed). **Schedule tab**: duration, start and finish (manual tasks
  only), constraint and constraint date, deadline, actual start and finish.
  **History tab**: `GET /v1/tasks/{id}/history`, newest first, with local
  time, who ("This computer", "system" or the token label) and field
  before → after.
- **Checks before sending**: the client runs core's `validateTask` (one rule
  set with the server), marks each failing field ⚠ with its reason, switches
  to its tab and sends nothing. Milestones get duration 0; ASAP clears the
  constraint date; % above 0 fills in the actual start; a summary's rolled-up
  fields and type are locked.
- **Save** (Ctrl + S, Edit → Save Task) does a dry run first; when the change
  ripples into summaries (or deletes something) a preview shows it, otherwise
  it saves directly. The PATCH carries If-Match with the rev read when the
  task was shown; a 412 refetches and asks per field (Keep mine / Take
  theirs / Cancel). Server 400s mark the field they name.
- **Unsaved edits** are never dropped silently: moving to another task or
  project asks Save / Discard, quitting asks Discard / Cancel. While
  untouched the editor follows the server's copy.
- **Edit → Task Details** (Alt + Enter) jumps to the editor.
- **Icons**: each project in the sidebar has a folder icon with a dot in its
  RAG colour (greyed when not active), and the menu actions use the same
  set. They are drawn by the app in the theme's colours, so they look the
  same on Linux (no dependence on the system icon theme) and macOS.
- Read-only tokens and offline mode show the editor read-only, with the
  reason.
- **Menu bar always in the window on Linux.** A global-menu service on the
  session bus (`com.canonical.AppMenu.Registrar`, e.g. Fildem on GNOME) made
  Qt hand the menus to it and hide the window's menu bar; when that service
  shows nothing, File, Edit, View, Project and Help were unreachable. The
  app now sets `AA_DontUseNativeMenuBar` and a non-native menu bar on Linux;
  macOS keeps its native menu bar.
- **Toolbar** with the common actions (New Project, Edit Project Details,
  Refresh, Outline, Insert Task, Task Details, Save Task, Find, Check for
  Updates) in the app's icons, with tooltips naming the shortcut. **View →
  Show Toolbar** toggles it; the choice is saved (`[view] toolbar` in
  `zproject.ini`).

## 0.11.0 — 2026-11-22

Desktop client plan, PR 6: the WBS tree (plan §3.2). A project's tasks open
as an outline over the cache, with read-only roll-ups and structural edits
from the keyboard.

- **View → Outline** (and Open Outline on the Details page): a tree of WBS
  code, title, type, status, % complete, duration, start, finish, deadline and
  priority. WBS codes are numbered like the server's and checked against the
  cached `GET /v1/projects/{id}/wbs`; until the next refresh after a move the
  header says "WBS codes update after the next refresh".
- **Summary rows** are bold, with a lock on their status and % cells, the
  tooltip "Rolled up from N child tasks", and an accessible name ending
  "(rolled up)"; those cells can't be edited. If another change makes a task
  a summary while you edit it, the 400 reads "This task now has subtasks; its
  progress is rolled up" and the outline refreshes. A summary's start and
  finish show the span of its subtasks (display only; the server doesn't roll
  dates up yet) and its duration is left blank. Milestones show a diamond.
- **Inline edits** (double-click or F2): title, priority, and status and %
  complete on leaf tasks. Setting % above 0 on a task without an actual start
  sends its scheduled start (else today) as the actual start.
- **Edit menu:** Insert Task (Ins or Enter: below the current task, then its
  title editor opens), Indent (Tab), Outdent (Shift-Tab), Move Up / Down
  (Alt-↑/↓, via `POST /v1/tasks/{id}/move`), Rename Task (F2), Convert to
  Milestone / Task, Delete Task… (Del or Backspace), Restore Deleted Task…,
  Find… (Ctrl-F), Find Next (F3), Find Previous (Shift-F3). Structural keys
  act only while the tree has focus.
- **Previews (plan Q6):** every write is a dry run first; a preview appears
  when the write ripples (a roll-up changes, say) or destroys. Siblings that
  are only renumbered don't count, so a plain move up or down saves directly.
  Delete always previews the subtree and its roll-ups and sends the
  `confirmId`; Restore (tasks deleted from this window) previews too.
- **412 on a field edit** opens the conflict dialog; on a structural change
  it says the task changed, does nothing, and refreshes (never a blind
  retry). 403, 404 and offline map as in 0.10.0.
- **Filter** by status (All, Not done, To do, In progress, Done) keeps the
  ancestors of matches; **Find** matches titles or an exact WBS code among the
  visible rows (Enter / Shift-Enter step, Esc closes).
- **Tests:** `tst_outline` (model unit tests with `QAbstractItemModelTester`,
  and offscreen GUI tests against the real API on loopback: insert and name,
  indent/outdent with preview, move without preview, delete and restore,
  confirming tokens, convert to milestone, roll-up ripple, edit and
  structural conflicts, the summary roll-up refusal, tree-scoped keys,
  read-only token, offline); `tst_appshell` knows the Edit menu.
- **Screenshots** (offscreen, example data): `outline-light/dark/high-contrast`,
  `outline-filter-find`, `outline-indent-preview`, `outline-delete-preview`;
  the main-window, offline, project-details and Help screenshots are
  re-rendered (Edit menu, Open Outline, the Help section "The outline").
- **Help:** a section on the outline and its keys; the shortcut table adds
  them (and Ctrl-N / Ctrl-E). Ctrl-Tab leaves the tree, since Tab indents
  there.
- README "Current release" is now 0.10.0.

## 0.10.0 — 2026-11-22

Desktop client plan, PR 5: the project list and project details, with
create, edit and delete through dry-run previews, and the shared 412 conflict
dialog (plan §4.4–4.5, §7.10, §8.1, §9.3). Also makes task `order`
read-only in the API.

- **Sidebar:** projects sorted active, then on hold (italic, "(on hold)");
  done and cancelled are hidden behind "Show done and cancelled (N)".
- **View → All Projects:** a table of Name, Status, RAG (letter and coloured
  dot, with an accessible name), Priority, Start, Target finish and Owner;
  activating a row opens that project's Details. View → Project Details goes
  back.
- **Project details** page: every field read-only, with Edit Details… and
  Delete Project…; the empty state points at File → New Project….
- **Project editor** (File → New Project… Ctrl-N, Project → Edit Project
  Details… Ctrl-E): local checks first (⚠ next to each field, nothing sent),
  only changed fields in the `PATCH`, and `If-Match` with the `rev` read from
  the server when the editor opens. Every write is a dry run first; a preview
  appears only when the dry run changes more than the project itself.
- **Delete Project…** always previews the project and each of its tasks,
  then sends the dry run's `confirmId` (needed by `requireConfirm` tokens); an
  expired or mismatched confirmation, or a 412, re-runs the preview.
- **Conflict dialog (412):** refetches the project, says who changed it and
  when (change feed, else cached activity, else `updatedAt`), and lists field ·
  your value · server value · result. Fields only you changed are kept; fields
  both changed need Keep Mine / Take Theirs / Cancel (default). Either choice
  re-runs the dry run on the server's revision; nothing is retried blindly.
- Errors map per plan §4.3: inline field errors for 400, a "this computer's
  token can't change projects" note for 403, "deleted elsewhere" and a refresh
  for 404, and no edits while offline (actions and buttons are disabled).
- **API — task `order` is read-only:** a create (`POST
  /v1/projects/{id}/tasks`) or `PATCH /v1/tasks/{id}` body with `order` is
  now 400 `validation` (`details.field: "order"`), like the other read-only
  fields; `order` stays in task JSON (OpenAPI `readOnly`). Reorder with `POST
  /v1/tasks/{id}/move`. Clients that sent `order` must drop it. Project
  `order` is unchanged.
- Inspector: "Revision" is shown only when the cache knows it (rows built from
  the change feed carry no `rev`).
- **Tests:** `tst_editflow` (changed-field bodies, dry-run preview rules,
  conflict merge) and `tst_projects` (offscreen, against the real API on
  loopback: list grouping, create, edit with If-Match, conflict take theirs /
  keep mine / cancel, delete previews, confirm expiry, read-only token, deleted
  elsewhere, offline); `tst_api` covers `order` being refused.
- **Screenshots** (offscreen, example data): `projects-details-light/dark`,
  `projects-table-light/high-contrast`, `project-editor`,
  `project-conflict-light/dark`, `project-delete-preview`; the main-window and
  offline screenshots are re-rendered with the new sidebar and Project menu.
- README "Current release" is now 0.9.0.

## 0.9.0 — 2026-11-22

Desktop client plan, PR 4: server move and restore, for the outline's
indent/outdent, drag-to-reorder and undo of a delete. Server, OpenAPI and CLI
only; no schema migration (`ord` and `deleted` are already in schema 1).

- **`POST /v1/tasks/{id}/move`** (`tasks:write`, not destructive): body
  `{parent?, position?}` reparents and/or reorders a task within its project.
  `position` is the 0-based index among the new parent's live children
  (default last); destination siblings are renumbered `order` 1..n, and only
  rows that change get a revision and a change row. Old and new parents roll
  up in the same transaction. 409 `cycle` for a move under itself or a
  descendant; 400 for a milestone, deleted or other-project parent or an
  out-of-range position; 412 on an `If-Match` mismatch; a move to the place it
  already has writes nothing. `?dryRun=true` lists the move (`op: "move"`),
  renumbered siblings and summary roll-ups.
- **`POST /v1/tasks/{id}/restore`** (`tasks:write`, destructive, so
  `requireConfirm` tokens need the dry run's `confirmId`): undeletes a task
  and the descendants the same delete tombstoned; ones deleted earlier stay
  deleted. 409 `conflict` with `details.reason` `not_deleted`,
  `parent_deleted`, `parent_milestone` or `project_deleted`. Dry-run ops are
  `restore`.
- Both write audit rows (the task's parent/order or `deleted` before and
  after) and change-feed rows for every task they touch, under one request.
- **OpenAPI:** both operations, a `TaskMove` schema, and `move` / `restore` in
  the dry-run `op` enum.
- **CLI:** `zproject-cli tasks move ID [--parent ID|none] [--position N]
  [--rev N] [--dry-run]`; N counts from 1, as WBS numbers do.
- README "Current release" is now 0.8.0.

## 0.8.0 — 2026-11-22

Desktop client, PR 3 of the approved plan: the signed in-app updater (SPEC.md
§21), zterminal's design. The CLI, the server and the `zprojectd` package
behave as before.

- **Help → Check for Updates…** asks
  `https://api.github.com/repos/sbj-ee/zproject/releases/latest` and says
  whether a newer release exists (Download and Verify / Later / Skip This
  Version, with the release notes).
- **Verify before offering to install:** Download and Verify fetches
  `SHA256SUMS` and `SHA256SUMS.minisig`, checks the minisign signature against
  the compiled-in key `32960BBA77B36F1F` and the trusted comment
  (`zproject <tag> SHA256SUMS`, so an old release's signature can't be
  replayed) *before* downloading the package, then checks the package's
  SHA-256. **Install…** appears only when both pass; any mismatch deletes the
  download and installs nothing.
- **HTTPS only**, assets only from `github.com/sbj-ee/zproject/releases/download/<tag>/`,
  redirects only to GitHub's release-asset hosts.
- **Install:** Linux runs `/usr/bin/pkexec /usr/bin/apt install -y` on a
  private, re-hashed copy of the verified `.deb` (only when running as
  `/usr/bin/zproject`; otherwise, or if pkexec fails, it shows the path and
  the `sudo apt install` command), then offers **Restart Now**. macOS opens
  the verified `.dmg`. Nothing is installed without the user's click.
- **Never touches `zprojectd`:** the updater selects only
  `zproject_<v>_amd64.deb` / `zproject-<v>-Darwin.dmg`, refuses any
  `zprojectd*` asset, and checks the `.deb`'s `Package:` field is `zproject`
  before pkexec.
- **Daily check:** asked once on first run (default on), changeable in
  Preferences ("Check for zproject updates once a day", Check Now…), at most
  once a day, silent unless there is something to offer.
  `update-state.ini` (0600, beside `zproject.ini`) keeps the last check and a
  skipped version.
- **Build:** the desktop app links libsodium (≥ 1.0.18) for minisign; the
  desktop `.deb` gains `libsodium23`, the `.dmg` bundles it, and the
  server-only build and `zprojectd` package don't need it (CI checks both).
- **Tests:** `tst_update` drives the updater offscreen against a mock release
  feed signed with a throwaway key: a valid update (verify, then install
  through a stand-in installer, restart), bad signature, signature by another
  key, tampered `SHA256SUMS`, a replayed trusted comment, bad package
  checksum, no newer version, network errors (check and download), 404 and
  rate limits, https-only, untrusted asset URLs, a foreign redirect, a
  server-only release, a `.deb` whose control file says `zprojectd`, a
  dismissed password prompt, macOS `.dmg` selection, Skip This Version, the
  quiet daily check and Cancel. Screenshots of the dialog states are in
  `docs/screenshots/update-*.png`.

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
