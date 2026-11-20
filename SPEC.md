# zproject — Project specification

This specification describes zproject **0.2.0**, the version in `CMakeLists.txt`
(`scripts/check-version.sh` keeps the two in step).

**Status: Approved / locked 2026-11-20** (Stephen: "go with the recs",
3:27 PM MST). Any change after this needs Stephen's yes. Approving the spec is
not approval to build. **Repository and build (§25, decision 8; Notion Q11):
Approved 2026-11-20 4:02 PM MST by Stephen** ("Go ahead and start building zproject"): `sbj-ee/zproject` is created
and phase 1 starts in reviewable pull requests. Merges, tags, releases and
installs still each need Stephen's yes.
**Post-lock change, approved by Stephen:** Changed 2026-11-20 3:59 PM MST by Stephen: zprojectd host debian199 -> dragon (§4.4, decision 2).
This document consolidates the Notion spec
([zproject spec](https://app.notion.com/p/41e2b260c3f08169ad71f9c403f787f9))
and the additions of 2026-11-20 afternoon: Stephen's feature list (1:20 PM), the
six PM features he approved (1:21 PM), suite interop and agent access
(1:24 PM), icon concept B, and the backend recommendation. Where Notion and this
file differ, this file is newer; the Notion page carries a mirror of it.

It is modelled on zterminal's `SPEC.md`: once a repository exists, the build
files and workflows win over this document, and this document is corrected to
match them. Decisions are recorded in
[§25 Decisions](#25-decisions): every recommendation was accepted on
2026-11-20 except decision 8 (creating the repository and starting the build),
which was approved 2026-11-20 4:02 PM MST by Stephen. Markers such as **(decided, Q14)** point to the decision
behind a choice.

Contents:

1. [Overview](#1-overview)
2. [Scope and non-goals](#2-scope-and-non-goals)
3. [Glossary](#3-glossary)
4. [Architecture](#4-architecture)
5. [Data model](#5-data-model)
6. [Scheduling engine](#6-scheduling-engine)
7. [Feature specifications](#7-feature-specifications)
8. [User interface](#8-user-interface)
9. [HTTP API](#9-http-api)
10. [CLI](#10-cli)
11. [MCP endpoint for agents](#11-mcp-endpoint-for-agents)
12. [Google Calendar](#12-google-calendar)
13. [Suite interop](#13-suite-interop)
14. [Security and threat model](#14-security-and-threat-model)
15. [Privacy](#15-privacy)
16. [Storage, backup and migration](#16-storage-backup-and-migration)
17. [Performance targets](#17-performance-targets)
18. [Accessibility](#18-accessibility)
19. [Testing, fuzzing and CI](#19-testing-fuzzing-and-ci)
20. [Packaging, signing and release](#20-packaging-signing-and-release)
21. [In-app updater](#21-in-app-updater)
22. [Versioning rules](#22-versioning-rules)
23. [Branding](#23-branding)
24. [Phased milestones](#24-phased-milestones)
25. [Decisions](#25-decisions)

---

## 1. Overview

zproject is a personal and small-team project-management app in the z-suite:
projects with a WBS, milestones, dependencies, a schedule computed by the
critical path method, resources with calendars and cost, baselines, a RAID log,
templates, reports, and alerts — with a Gantt chart at its centre. It is for
work that has a start, a finish and dependencies, which ztodo (daily capture and
to-do lists) is deliberately not built for. The two stay separate apps that
interoperate through their APIs (§13).

Three deliverables, one repository (`sbj-ee/zproject`, MIT, public; creation
approved 2026-11-20 4:02 PM MST by Stephen):

| Component | What it is | Runs on |
|---|---|---|
| `zprojectd` | Headless server: C++20, Qt 6 Core/Network (no GUI), SQLite. The only writer to the plan database. Serves the REST API `/v1`, the MCP endpoint `/mcp`, reports, alerts, Google Calendar sync and backups. | dragon — Pop!_OS 24.04 LTS (Ubuntu 24.04 noble base), amd64, verified 2026-11-20 **(decided, Q2; changed 3:59 PM by Stephen)** |
| `zproject` | Desktop client: C++20, Qt 6 Widgets. Gantt, lists, editors, reports; local read-only cache for instant start and offline reading. | MacBook (macOS, Apple Silicon), vertex and dragon (Linux amd64) |
| `zproject-cli` | Thin command-line client of `/v1`. | Any client machine, and the server |

Conventions shared with the other Z apps: C++20 and Qt 6, `core/` (logic, no
GUI, unit-tested) and `app/` (widgets) split, CMake as the single version
source, every PR bumps the version with a CHANGELOG entry, minisign-signed
releases (key ID `32960BBA77B36F1F`), an in-app updater that installs nothing
unless the signature verifies, ASan/UBSan and fuzzing in CI.

## 2. Scope and non-goals

In scope (by phase, §24): projects and project details; WBS with summary tasks
and milestones; FS/SS/FF dependencies with lag and lead (SF later); auto and
manual scheduling, constraints, deadlines; critical path and float; baselines
and variance; resources, assignments, working calendars, holidays and cost;
over-allocation warnings; RAID log; templates; task notes; tags and links;
multi-project portfolio; reports and status reports; alerting; exports and
imports; read-only Google Calendar overlay; REST API with OpenAPI 3.1; scoped
tokens for devices, apps and agents; an MCP endpoint; an audit log; ztodo
interop.

Non-goals unless Stephen changes them:

- Multi-user collaboration with per-user permissions, comments threads and
  presence. (Agents and apps are separate *clients* with scoped tokens, not
  users.)
- Mobile or web clients, and a Windows build.
- Any exposure to the public internet (no Tailscale Funnel, no port forwarding,
  no CORS).
- Writing to Google Calendar in the first release; writing into ztodo's data
  files ever.
- PERT/Monte Carlo schedule risk, portfolio financials, approvals workflows.
- Running the live database on a synced folder (Box, rclone, iCloud) — see §16.

## 3. Glossary

| Term | Meaning in this spec |
|---|---|
| WBS | Work breakdown structure: the task tree. A task with children is a *summary task*; WBS codes (`1.2.3`) are derived from position. |
| Milestone | A task with `type = milestone` and zero duration. |
| FS / SS / FF / SF | Finish-to-start, start-to-start, finish-to-finish, start-to-finish dependency types. |
| Lag / lead | Delay (positive) or overlap (negative lag) on a dependency, in working days. |
| CPM | Critical path method: forward pass for early dates, backward pass for late dates. |
| Float (slack) | Total float = late start − early start; free float = slack before the earliest successor is delayed. |
| Critical | Total float ≤ threshold (default 0 working days). |
| Constraint | A date rule on a task: ASAP (default), SNET (start no earlier than), FNLT (finish no later than). |
| Deadline | A soft target date that raises an alert but does not move the schedule. |
| Baseline | An immutable snapshot of planned dates, durations, work and cost. |
| Variance | Current (or actual) minus baseline. |
| Units | A resource's share of a working day on an assignment (100% = full day). |
| Over-allocation | A resource's summed units on a day exceed its max units, or it is assigned on a non-working day of its calendar. |
| RAID | Risks, assumptions, issues, decisions log. |
| RAG | Red / amber / green status. |
| Token | A scoped API credential (§9.2). `owner`, `device`, `app`, `agent` kinds. |
| Change record | One row of the append-only `changes` table: who changed which fields of which entity, before and after. |
| Tailnet | Stephen's Tailscale network. |

## 4. Architecture

```
 MacBook / vertex / dragon (clients)               dragon (server)    
 ┌─────────────────────────┐   HTTPS (tailnet)   ┌────────────────────────────┐
 │ zproject (Qt Widgets)   │ ──────────────────▶ │ tailscale serve :443       │
 │  local cache (SQLite)   │                     │      │ 127.0.0.1:8788       │
 │ zproject-cli            │ ──────────────────▶ │ zprojectd                  │
 └─────────────────────────┘                     │  /v1 REST  /mcp  reports   │
 Agent box (remote, on the tailnet)              │  alerts  calendar sync     │
 ┌─────────────────────────┐   HTTPS (tailnet)   │  SQLite WAL  backups       │
 │ Grok Bot agents (MCP)   │ ──────────────────▶ │       │ alerts over tailnet ─┼─▶ ts-notify-hub
 └─────────────────────────┘                     └────────────────────────────┘
                                                   nightly snapshot ─▶ Box folder
 ts-notify-hub runs on debian199 (http://100.112.153.39:8787).
```

### 4.1 Directory layout

```
core/        model, validation, scheduling engine, calendars, CSV/JSON/MSPDI
             parsers, minisign verify, change records — no GUI, no network
store/       Store interface + SQLite implementation, migrations, backup
server/      zprojectd: HTTP router, auth, scopes, rate limits, audit, MCP,
             reports, alerts, Google Calendar sync
client/      REST client, local cache, (phase 2) offline outbox
app/         Qt Widgets desktop app
cli/         zproject-cli
api/         openapi.yaml (OpenAPI 3.1, source of truth for /v1)
tests/       unit, property, API contract, GUI smoke tests
fuzz/        libFuzzer harnesses + corpus
packaging/   deb (zproject, zprojectd), dmg, systemd units, icons
assets/icon/ zproject-B.svg master
scripts/     check-version.sh, check-version-increase.sh, check-bundle.sh
docs/        RELEASING.md, API.md, MCP.md, THREAT-MODEL.md
```

### 4.2 Build targets

`zproject_core` (static lib), `zproject_store`, `zproject_server_lib`,
`zprojectd`, `zproject_client`, `zproject` (GUI), `zproject-cli`, tests,
fuzzers (`-DZPROJECT_FUZZ=ON`, clang only). CMake options:
`ZPROJECT_WERROR`, `ZPROJECT_BUILD_GUI` (off for the server-only
`zprojectd` package build), `ZPROJECT_SANITIZE`.

### 4.3 Dependencies

Qt 6, **minimum 6.4** (Core, Network, Widgets, PrintSupport for PDF, Test), SQLite ≥ 3.45 (bundled amalgamation so WAL and `VACUUM INTO`
behaviour is fixed), libsodium (minisign verification, token hashing helpers),
libsecret (Linux keychain) and Security.framework (macOS Keychain). No
JavaScript runtime, no Python at runtime.

**Qt version.** The server host (dragon, Pop!_OS 24.04) and vertex use the
distribution's Qt **6.4.2** from Ubuntu noble (`qt6-base-dev`); the macOS build
uses Homebrew's newer Qt. The code therefore targets Qt 6.4 as its minimum and
uses no API newer than 6.4. Nothing in this spec needs a newer Qt:
- the server uses only Qt Core and Network;
- the HTTP/1.1 layer is zproject's own small, fuzzed parser on `QTcpServer`.
  It listens only on `127.0.0.1` behind `tailscale serve`. Qt HttpServer is
  deliberately not used, because it is a technology preview in 6.4 and its
  API changed in later releases;
- SQLite is the bundled amalgamation, not `QtSql`;
- PDF uses `QPdfWriter` / `QPrinter`, which are in 6.4.

A bundled or newer Qt is only worth it if a later feature needs one (for
example Qt Graphs). It would be decided then, with Stephen's yes, not now.

### 4.4 Deployment on dragon

Changed 2026-11-20 3:59 PM MST by Stephen: zprojectd host debian199 -> dragon. The decision is recorded in §25, decision 2. The read-only checks
run on dragon at about 4:00 PM MST on 2026-11-20, with no sudo, agreed with
CoS's independent check:

- **OS / arch:** `/etc/os-release` reports Pop!_OS 24.04 LTS (an Ubuntu 24.04
  noble base; `/etc/debian_version` shows `trixie/sid`, which is Ubuntu's
  Debian base, not Debian). `uname -m` reports `x86_64` and dpkg reports
  `amd64`.
- **Capacity:** 24 cores, 61 GiB RAM (about 49 GiB available), 261 GB free
  on `/`. Up 13 days, load average about 0.5. GNOME's suspend on AC is set to
  `nothing`.
- **Listening TCP ports:**
  - on `127.0.0.1`: 631 (CUPS), 7681 (ttyd), 9996–9997 (batfish container),
    11434 (Ollama);
  - on all interfaces: 1313 (Hugo dev server), 1716 (KDE Connect), 22, 25,
    111, 139, 445;
  - on the tailnet address: tailscaled's own ports.
  - **8787 and 8788 are free.**
- **Tailscale:** `tailscale serve status` reports "No serve config". The node is
  `dragon` at `100.97.211.64`.

Chosen settings:
- **Local port:** `zprojectd` binds `127.0.0.1:8788`. It is free on dragon,
  and the port that ts-notify-hub uses (8787) on debian199 is left alone.
- **Tailscale serve mapping** (Stephen runs it; Software changes nothing on
  dragon): `tailscale serve --bg --https=443 http://127.0.0.1:8788`, which
  publishes `https://dragon.<tailnet>.ts.net/` → `/v1` and `/mcp`. Nothing is
  served on dragon today, so it does not clash with anything. If Stephen
  later wants 443 for something else, `--https=8443` is the fallback, and
  clients store the full URL. Never Funnel.
- **Paths on dragon:** database `/var/lib/zproject/zproject.db`, backups
  `/var/lib/zproject/backups`, Google credentials in a root-owned 0600 file
  under `/etc/zproject/` or `systemd-creds`. The Google Calendar cache lives in
  the database on dragon. An off-site copy of the snapshots goes to a Box
  folder through dragon's existing rclone setup.
- **Alerts:** `zprojectd` on dragon posts to ts-notify-hub on debian199 at
  `http://100.112.153.39:8787` over the tailnet/LAN. That is the only
  outbound call besides Google, and the systemd unit allows it.

Coexistence with what already runs on dragon:
- **What runs there:** Ollama, a batfish Docker container, the Hugo dev
  server, ttyd, Samba, Postfix, and libvirt. Timers: netbox-scanner (system,
  every 30 minutes), rusty-rag-index (user, about every 45 minutes) and
  home-dashboard (user, every minute; the Blink cache every 5 minutes). The
  user service desk-presence runs too.
- **Footprint:** `zprojectd` is small (about 60 MB RSS, mostly idle). Its unit
  sets `MemoryMax=512M`, `CPUWeight=50` and `IOWeight=50`, so Ollama
  inference and rusty-rag indexing keep priority.
- **Backups:** the nightly timer runs at 03:17 with `RandomizedDelaySec=10m`,
  away from the netbox-scanner :00/:30 slots. It uses `Nice=10` and
  `IOSchedulingClass=idle`.
- **Startup:** it is a system unit, so it runs without a login session and
  does not touch the user-session services.
- **Risk:** dragon is also a desktop workstation. Pop!_OS updates and reboots
  take the server offline briefly; desktop clients fall back to the read-only
  cache, and agents get connection errors until it is back.

## 5. Data model

All ids are 20 lower-case hex characters (from a UUID), the shared z-suite id
format. Dates are `YYYY-MM-DD` (date-only; the plan has no time zone).
Timestamps are UTC ISO 8601 with milliseconds. Every entity has `id`,
`createdAt`, `updatedAt`, `deleted` (tombstone flag; deletes are tombstones and
restorable), and `rev` (integer, incremented on every change; used for
optimistic concurrency via `If-Match`).

### 5.1 Tables

| Table | Fields (beyond the common ones) |
|---|---|
| `project` | `name`, `description` (Markdown), `objectives`, `scope`, `outOfScope`, `successCriteria`, `owner`, `sponsor` (resource id or text), `status` (`active`, `on_hold`, `done`, `cancelled`), `rag` (`green`, `amber`, `red`), `priority` (1–4), `start`, `finish` (optional target), `calendar`, `budget`, `color`, `order`, `templateId` (if created from one) |
| `task` | `project`, `parent` (null = top level), `order`, `title` (required), `notes` (Markdown), `type` (`task`, `milestone`; `summary` is derived), `mode` (`auto`, `manual`), `duration` (working days, ≥ 0; 0 for milestones), `start`, `finish` (stored for manual tasks; computed for auto), `constraint` (`ASAP`, `SNET`, `FNLT`), `constraintDate`, `deadline`, `calendar` (null = project's), `percentComplete` (0–100), `actualStart`, `actualFinish`, `status` (`todo`, `in_progress`, `done`; derived from % unless overridden), `priority` (1–4), `fixedCost` |
| `dependency` | `project`, `predecessor`, `successor`, `type` (`FS`, `SS`, `FF`; `SF` phase 2), `lag` (working days, may be negative) |
| `resource` | `name`, `kind` (`person`), `email` (optional display, e.g. `someone@example.com`), `maxUnits` (default 100), `rate` (per hour), `calendar` |
| `assignment` | `task`, `resource`, `units` (1–1000 %) |
| `calendar` | `name`, `base` (parent calendar or null), `workWeek` (7 entries: working flag + hours), `hoursPerDay` (default 8) |
| `calendar_exception` | `calendar`, `from`, `to`, `kind` (`holiday`, `non_working`, `custom_hours`), `hours`, `label` |
| `baseline` | `project`, `name`, `active`, `createdBy` (token id) |
| `baseline_task` | `baseline`, `task`, `start`, `finish`, `duration`, `work`, `cost` (immutable) |
| `raid_item` | `project`, `type` (`risk`, `assumption`, `issue`, `decision`), `title`, `description`, `status` (per type, §7.5), `owner`, `probability` (1–5), `impact` (1–5), `response` (`avoid`, `reduce`, `transfer`, `accept`), `mitigation`, `dueDate`, `reviewDate`, `decidedOn`, `rationale`, `convertedFrom` |
| `raid_task` | `raid_item`, `task` (links) |
| `template` | `name`, `description`, `schemaVersion`, `body` (JSON, §7.6), `createdFrom` |
| `task_note` | `task`, `at`, `author` (token id), `body` (Markdown); append-only journal |
| `tag`, `entity_tag` | `name`, `color`; (`entityKind`, `entityId`, `tag`) |
| `link` | `entityKind`, `entityId`, `app` (`url`, `ztodo`, `zproject`, `zmail`, `zwriter`), `kind`, `target` (URL or id), `label` |
| `status_report` | `project`, `asOf`, `summary` (text), `payload` (JSON snapshot of the generated report) |
| `alert_rule` | `rule` (§7.8), `enabled`, `params` (JSON), `quietHours` |
| `alert` | `rule`, `entityKind`, `entityId`, `severity`, `firstSeen`, `lastSeen`, `state` (`open`, `acknowledged`, `resolved`), `dedupKey` |
| `api_token` | `name`, `kind`, `secretHash`, `scopes` (JSON list), `projects` (list or `*`), `expiresAt`, `lastUsedAt`, `revokedAt`, `requireConfirm`, `rateLimit` (JSON override) |
| `confirm` | `token`, `requestHash`, `expiresAt`, `used` |
| `changes` | `seq` (autoincrement), `ts` (hybrid clock), `actor` (token id), `entityKind`, `entityId`, `before` (JSON, changed fields), `after` (JSON), `requestId` — append-only |
| `audit` | `seq`, `ts`, `tokenId`, `tokenName`, `tokenKind`, `tsUser`, `tsNode` (Tailscale identity), `method`, `path` or `mcpTool`, `entityKind`, `entityId`, `before`, `after`, `dryRun`, `confirmId`, `status`, `requestId`, `prevHash`, `hash` — append-only |
| `cal_account`, `cal_calendar`, `cal_event`, `cal_sync` | Google Calendar cache (§12): account address, calendar id/name/selected, event id/start/end/title/all-day/transparency, sync token and last refresh |
| `meta` | `schemaVersion`, `installationId`, `currency` (default `USD`) |

Derived (never stored): WBS code, summary-task roll-ups (start, finish, work,
cost, % complete weighted by duration), early/late dates, float, critical flag,
variance, over-allocation, project health suggestion.

### 5.2 Integrity rules

- `title` required; `duration = 0` iff `type = milestone`; milestones have no
  children; a task cannot be its own ancestor; dependencies are within one
  project (cross-project in phase 3), never duplicate, never self.
- The dependency graph (including implicit parent/child relations) must be
  acyclic. A write that would create a cycle is refused with 409 and the cycle
  path in the error.
- `actualFinish` implies `percentComplete = 100`; `percentComplete > 0` implies
  `actualStart` (defaulted to the scheduled start).
- `changes` and `audit` reject `UPDATE` and `DELETE` through SQLite triggers.

## 6. Scheduling engine

Pure `core/` code, deterministic, shared by server and desktop client (for
instant previews and the offline cache).

### 6.1 Working time

- Durations and lags are in **working days** of the task's calendar (task →
  project → Standard). A working day is a weekday marked working in `workWeek`
  minus `calendar_exception` dates; `custom_hours` exceptions change hours for
  work and cost, not day counting.
- Default Standard calendar: Monday–Friday, 8 hours. Holidays are added by the
  user; a one-click import of US federal holidays for a chosen year is offered
  (not preloaded) **(decided, Q14)**.
- Resource calendars inherit from the project calendar and add exceptions
  (vacation, part-time).

### 6.2 Algorithm

1. Build the graph of tasks and dependencies; topological sort (Kahn). Summary
   tasks are not scheduled themselves; their dates are roll-ups, and a
   dependency on a summary applies to its earliest-starting / latest-finishing
   leaf.
2. **Forward pass.** For each auto task in order: early start = max(project
   start, constraint (SNET), each predecessor's driving date: FS → pred EF +
   lag; SS → pred ES + lag; FF → pred EF + lag − duration; SF → pred ES + lag −
   duration), snapped to the next working day. Early finish = early start +
   duration (working days). Tasks with `actualStart` start there; tasks with
   `actualFinish` are fixed. Manual tasks keep their stored dates.
3. **Backward pass** from the project finish (the latest early finish, or the
   project's target finish if later): late finish = min(successor-driven dates,
   FNLT constraint, deadline when "schedule to deadline" is on); late start =
   late finish − duration.
4. Total float = late start − early start (working days); free float = min over
   successors of (successor driving date − this task's driving date).
   Critical = total float ≤ threshold.
5. **Violations**, never silently fixed: a manual task that breaks a link, a
   constraint that cannot be met (negative float), a deadline after forecast.
   They appear as warnings in the UI, in `schedule` responses, and as alerts.
6. Over-allocation detection (§7.4) runs after scheduling.

Auto tasks are rescheduled whenever an input changes. Every change shows (UI) or
returns (API `dryRun`) the ripple first: tasks moved and by how much, project
finish change, critical path change.

### 6.3 Phase 2+

SF links, SNLT/FNET/MSO/MFO constraints, effort-driven scheduling, resource
leveling (preview-first, never automatic), cross-project dependencies,
calendar busy time as non-working time.

## 7. Feature specifications

Stephen's 2026-11-20 list → sections: Gantt §8.2; Resources §7.4; Holidays
§6.1/§7.4; Resource cost §7.4; Google Calendar §12; Reporting §7.7; Alerting
§7.8; Task notes §7.10; Projects and Project details §7.10; Multi-project
§7.11; API endpoints §9; Exports §7.9. The six features Stephen approved as
committed at 1:21 PM are marked **[approved]**.

### 7.1 Milestones and WBS [approved] — phase 1

Task tree is the WBS; summary tasks roll up; milestones are zero-duration tasks
that can be linked and be critical. UI: outline with Tab/Shift-Tab, collapse,
WBS column, summary brackets and milestone diamonds on the Gantt, "Convert to
milestone", WBS-only tree view. API: `GET /v1/projects/{id}/wbs`,
`POST /v1/tasks/{id}/move`.

### 7.2 Critical path [approved] — phase 1

As §6. UI: critical tasks and links shaded with colour and a hatch pattern,
float column, "critical only" filter, link editor (type, lag). API:
`GET /v1/projects/{id}/schedule`, `GET /v1/projects/{id}/critical-path`,
`POST /v1/projects/{id}/reschedule?dryRun=true`.

### 7.3 Baselines (planned vs actual) [approved] — phase 1: one per project

"Set baseline" snapshots every task's start, finish, duration, work and cost.
Re-baselining asks for confirmation, creates a new active baseline and keeps
the previous one for undo. Variance (start, finish, duration, cost; "not in
baseline" for new tasks) as List columns, Gantt ghost bars, milestone slip
arrows and a Variance report. API: `GET|POST /v1/projects/{id}/baselines`,
`GET /v1/baselines/{id}`, `GET /v1/projects/{id}/variance`. Phase 2: up to 11
named baselines and compare-any-two.

### 7.4 Resources, cost, calendars, holidays, over-allocation [approved: over-allocation warnings] — phase 1

One resource pool across projects. Assignments carry units. Work = duration ×
hours per day × units. Task cost = Σ work × rate + fixed cost; roll-ups to
summaries and project; project budget vs planned vs baseline cost. One
currency per installation (USD default). Over-allocation: per resource per
working day, Σ units across all projects > max units, or assignment on a
non-working day of the resource's calendar. Warnings only (red markers on bars,
Resource Usage heat map, alert); nothing moves automatically. API:
`/v1/resources`, `/v1/tasks/{id}/assignments`, `/v1/assignments/{id}`,
`/v1/calendars`, `/v1/calendars/{id}/exceptions`,
`GET /v1/resources/{id}/usage`, `GET /v1/overallocations`. Phase 3: leveling,
earned value (PV/EV/AC, SPI, CPI) as a report.

### 7.5 RAID log [approved] — phase 1

Four types. Statuses: risk `open|mitigating|closed|occurred`; issue
`open|in_progress|resolved`; decision `proposed|decided|superseded`;
assumption `unverified|valid|invalid`. Risk score = probability × impact (1–25),
5×5 heat map. A risk that occurs converts to an issue with a back-link. Linked
tasks show a RAID badge on the Gantt. Review and due dates raise alerts. API:
`GET|POST /v1/projects/{id}/raid?type=&status=`, `GET|PATCH|DELETE /v1/raid/{id}`,
`POST /v1/raid/{id}/convert`.

### 7.6 Project templates [approved] — phase 1

Template body (JSON, `schemaVersion`): WBS tasks with relative offsets and
durations in working days, dependencies with type and lag, milestones, resource
roles (placeholders), RAID seed items, tags, optional calendar. Files:
`*.zproject-template.json`, importable and exportable so they can live in a
repo. "Save as template" (choose what to keep); "New from template" (start or
finish date, role → resource map). API: `/v1/templates`,
`POST /v1/projects/{id}/save-as-template`,
`POST /v1/templates/{id}/instantiate` (supports `dryRun`). Phase 3: template
variables, applying into an existing project.

### 7.7 Reporting — phase 1 (canned reports)

Server-side, from the same data and engine: Status report (RAG, summary text,
milestones due/slipped, critical-path change, top risks/issues, decisions this
period, % complete vs baseline), Variance, Critical path, Resource usage /
over-allocation, Cost, RAID summary, Portfolio one-pager. Formats: JSON,
Markdown, PDF. Status reports can be saved as dated snapshots. API:
`GET /v1/reports`, `GET /v1/projects/{id}/reports/{kind}?format=&asOf=`,
`GET /v1/portfolio/report`, `GET|POST /v1/projects/{id}/status-reports`.
Later: custom reports, scheduled delivery, earned value.

### 7.8 Alerting — phase 1

Rules evaluated after every write and every 15 minutes: `task_overdue`,
`deadline_at_risk` (forecast after deadline), `milestone_slip` (vs baseline by
> N days), `finish_moved` (critical path finish changed), `overallocation`,
`raid_review_due`, `calendar_stale` (> 24 h). De-duplicated by `dedupKey`;
states open → acknowledged → resolved (auto-resolve when the condition clears).
Delivery: desktop Alerts panel and notifications; ts-notify-hub on debian199
(`http://100.112.153.39:8787`) over HTTP across the tailnet/LAN from dragon
**(decided, Q19)**. Quiet hours and per-rule
toggles. API: `GET /v1/alerts`, `POST /v1/alerts/{id}/ack`,
`GET|PATCH /v1/alert-rules`.

### 7.9 Exports and imports

Phase 1 exports: JSON (full fidelity; also a backup format), CSV (tasks,
resources, assignments, RAID), PDF and PNG Gantt (date range, columns, page
tiling). Phase 1 imports: zproject JSON, CSV (columns: WBS level or outline
number, title, duration, start, predecessors in `3FS+2d` notation, resource
names). Phase 2: MS Project XML (MSPDI) import and export, `.ics` export of
milestones and deadlines. Every import is a dry run first. API:
`GET /v1/projects/{id}/export?format=json|csv|pdf|png`,
`POST /v1/import?format=json|csv|mspdi&dryRun=true`.

### 7.10 Projects, project details, task notes

Project details page: description, objectives, scope / out of scope, success
criteria, owner, sponsor, dates, budget, RAG (manual, with a computed
suggestion), priority, tags, links. Task notes: a Markdown `notes` field plus
an append-only note journal (`task_note`) so people and agents add notes
without overwriting each other. API: `PATCH /v1/projects/{id}`,
`GET|POST /v1/tasks/{id}/notes`.

### 7.11 Multi-project — phase 1

Portfolio view (status, RAG, % complete, next milestone, finish vs baseline,
over-allocations), an all-projects timeline (summary bar per project,
expandable), shared resource pool. API: `GET /v1/portfolio`,
`GET /v1/portfolio/timeline?from=&to=`. Phase 3: cross-project dependencies,
programmes.

### 7.12 Also in phase 1 (recommended best practice)

% complete; tags on projects, tasks and RAID items; saved filters and views;
global search; undo/redo (client session) and history / restore any entity from
`changes`; change log view; scheduling modes; SNET/FNLT and deadlines;
keyboard-first UX (§8.4). Phase 2: time tracking (hours), attachments (stored
on the server; links in phase 1), recurring project tasks. Skipped: PERT/Monte
Carlo, multi-user permissions.

## 8. User interface

### 8.1 Window

- Left sidebar: Portfolio, then projects (active first, then on hold, done
  collapsed), with a project's sub-views: Details, Gantt, List, Resources,
  RAID, Baselines, Reports. Alerts badge at the top.
- Main area: the selected view. Inspector panel on the right for the selected
  task, milestone, resource or RAID item (Details, Schedule, Assignments,
  Notes journal, Links, History tabs).
- Status bar: server connection (connected / offline-stale with last refresh
  time), current filter, schedule warnings count.
- Menus: File (New project, New from template, Import, Export, Print/PDF,
  Refresh F5, Preferences), Edit (Undo/Redo, Cut/Copy/Paste rows, Find),
  View (Gantt, List, Resources, RAID, Portfolio; zoom; critical only; baseline
  bars; calendar overlay), Project (Set baseline, Save as template, Reschedule,
  Project calendar), Help (Check for updates, About, API docs).
- First run: server URL (e.g. `https://dragon.<tailnet>.ts.net`), then a
  one-time sign-in that creates a `device` token (pasting an owner-approved
  enrolment code from `zproject-cli token create --kind device`), stored in the
  keychain. Single instance per device (`QLockFile`), as ztodo.

### 8.2 Gantt

Rows follow the WBS outline (task table on the left: WBS, title, duration,
start, finish, % complete, resources, float). Timeline: day / week / month /
quarter zoom, non-working days shaded per project calendar, today line, bars
(auto: solid; manual: outlined), summary brackets, milestone diamonds,
progress fill, dependency arrows by type, critical path shading (colour +
hatch), baseline ghost bars, deadline flags, over-allocation markers, RAID
badges, and an optional read-only Google Calendar lane (and, phase 3, a ztodo
lane). Drag a bar to move (sets SNET on auto tasks, or dates on manual tasks,
after a ripple preview), drag the end to change duration, drag between bars to
link. Print and export to PDF/PNG.

### 8.3 Other views

List (outline table with configurable columns, inline edit), Resources sheet
and Resource Usage heat map, RAID table with type filters and risk heat map,
Baselines and Variance table, Reports (rendered Markdown with export),
Portfolio table and all-projects timeline, Calendar editor (work week and
exceptions), Preferences (server, appearance light/dark/system, font size,
updates, Google Calendar connect/disconnect and calendar picker, Tokens,
Audit log viewer, alert rules).

### 8.4 Keyboard

Insert task (Enter below / Ins), indent Tab, outdent Shift-Tab, link selected
tasks Ctrl-L (unlink Ctrl-Shift-L), edit F2, delete Del (tombstone, undoable),
undo Ctrl-Z / redo Ctrl-Shift-Z, find Ctrl-F, command palette Ctrl-K, quick add
RAID R / A / I / D in the RAID view, Gantt zoom Ctrl-+/−, scroll to today T,
switch views Ctrl-1…6, refresh F5. Cmd instead of Ctrl on macOS.

## 9. HTTP API

### 9.1 Transport and binding

- `zprojectd` listens on `127.0.0.1:8788` (configurable) on dragon. 8788
  was verified free there on 2026-11-20; 8787 stays reserved by the suite's
  convention for ts-notify-hub on debian199.
  `tailscale serve` publishes it as `https://<host>.<tailnet>.ts.net` with a
  Tailscale certificate, tailnet only. Never Funnel, never a raw LAN bind by
  default, no CORS.
- Tailscale ACL grants allow only Stephen's devices and the agent box's tag
  (e.g. `tag:grokbot`) to reach the host on 443 **(decided, Q30)**.
- The server reads the identity headers `tailscale serve` adds
  (`Tailscale-User-Login`, `Tailscale-User-Name`) and the peer node, and
  records them in the audit log. They are an extra signal, not a replacement
  for tokens (tagged nodes do not carry a user login).
- JSON over HTTP/1.1, UTF-8, `Content-Type: application/json`. Request bodies
  ≤ 1 MiB (imports ≤ 20 MiB).

### 9.2 Authentication and scopes

`Authorization: Bearer zpt_<id>.<secret>` on every request except
`GET /v1/health`. The server stores only `SHA-256(secret)` and compares in
constant time. Token kinds: `owner` (created once on the server with
`zprojectd --create-owner-token`, printed once), `device` (one per desktop
client), `app` (another z-suite app), `agent` (one per Grok Bot agent).
Scopes: `projects:read|write`, `tasks:read|write`, `schedule:read`,
`resources:read|write`, `baselines:write`, `raid:read|write`,
`templates:write`, `reports:read`, `calendar:read`, `alerts:read|write`,
`audit:read`, `admin`; bundles `read` and `write`. Each token may be limited to
a project allowlist. Defaults for `agent` tokens: `read`, named projects only,
expires in 90 days, `requireConfirm = true`; agents never get `admin`
**(decided, Q24)**. Revocation takes effect on the next request.

### 9.3 Conventions

- Versioning: everything under `/v1`; breaking changes go to `/v2`, with `/v1`
  kept for at least one minor release. `GET /v1/health` returns
  `{ok, version, apiVersion, schemaVersion}` and nothing else.
- Concurrency: every entity has `rev`; `PATCH` honours `If-Match: <rev>` and
  returns 412 on mismatch. Desktop clients always send it.
- Dry run: every write accepts `?dryRun=true` and returns
  `{changes:[...], ripple:{moved:[...], finishDelta, criticalChanged}, confirmId?}`.
- Confirm: destructive operations (delete, bulk update, re-baseline, template
  instantiate into an existing project, import, restore, token revoke by
  non-owner) for tokens with `requireConfirm` need
  `X-Zproject-Confirm: <confirmId>` from a dry run of the identical request
  (bound to token and request hash, 5-minute expiry, single use). Otherwise
  409 `confirm_required`.
- Pagination: `?limit=` (≤ 500) and `?cursor=`; responses include
  `nextCursor`.
- Errors: `{"error":"human text","code":"machine_code","details":{...}}`.
  400 `validation`, 401 `unauthenticated`, 403 `forbidden_scope` /
  `forbidden_project`, 404 `not_found`, 409 `cycle` / `conflict` /
  `confirm_required`, 412 `rev_mismatch`, 413 `too_large`, 422
  `schedule_violation`, 429 `rate_limited` (with `Retry-After`), 500
  `internal` (no stack traces or secrets).
- Rate limits per token (token bucket): `agent` 120 reads, 30 writes, 5
  destructive per minute; `app` 600 / 120 / 20; `device` and `owner` 1,200
  total. Failed authentication 10 per minute per source. Overridable per token
  **(decided, Q28)**.
- OpenAPI 3.1: `api/openapi.yaml` is the source of truth, served at
  `GET /v1/openapi.json` (any valid token). Each operation declares
  `x-zproject-scope`, `x-zproject-destructive`, `x-zproject-dry-run`.

### 9.4 Endpoints

| Area | Endpoints |
|---|---|
| Health, schema | `GET /v1/health`, `GET /v1/openapi.json` |
| Projects | `GET/POST /v1/projects`, `GET/PATCH/DELETE /v1/projects/{id}`, `GET /v1/projects/{id}/wbs` |
| Tasks | `GET/POST /v1/projects/{id}/tasks`, `GET/PATCH/DELETE /v1/tasks/{id}`, `POST /v1/tasks/{id}/move`, `POST /v1/tasks/bulk` (destructive), `GET/POST /v1/tasks/{id}/notes`, `GET /v1/tasks/{id}/history`, `POST /v1/tasks/{id}/restore` |
| Dependencies | `GET /v1/projects/{id}/dependencies`, `POST /v1/dependencies`, `GET/PATCH/DELETE /v1/dependencies/{id}` |
| Schedule | `GET /v1/projects/{id}/schedule`, `GET /v1/projects/{id}/critical-path`, `POST /v1/projects/{id}/reschedule` |
| Baselines | `GET/POST /v1/projects/{id}/baselines`, `GET /v1/baselines/{id}`, `GET /v1/projects/{id}/variance` |
| Resources | `GET/POST /v1/resources`, `GET/PATCH/DELETE /v1/resources/{id}`, `GET /v1/resources/{id}/usage`, `GET/POST /v1/tasks/{id}/assignments`, `PATCH/DELETE /v1/assignments/{id}`, `GET /v1/overallocations` |
| Calendars | `GET/POST /v1/calendars`, `GET/PATCH/DELETE /v1/calendars/{id}`, `GET/POST /v1/calendars/{id}/exceptions`, `DELETE /v1/calendar-exceptions/{id}`, `POST /v1/calendars/{id}/import-holidays?country=US&year=` |
| RAID | `GET/POST /v1/projects/{id}/raid`, `GET/PATCH/DELETE /v1/raid/{id}`, `POST /v1/raid/{id}/convert` |
| Templates | `GET/POST /v1/templates`, `GET/PATCH/DELETE /v1/templates/{id}`, `POST /v1/projects/{id}/save-as-template`, `POST /v1/templates/{id}/instantiate` |
| Reports | `GET /v1/reports`, `GET /v1/projects/{id}/reports/{kind}`, `GET/POST /v1/projects/{id}/status-reports`, `GET /v1/portfolio`, `GET /v1/portfolio/timeline`, `GET /v1/portfolio/report` |
| Alerts | `GET /v1/alerts`, `POST /v1/alerts/{id}/ack`, `GET/PATCH /v1/alert-rules` |
| Export / import | `GET /v1/projects/{id}/export`, `POST /v1/import` |
| Tags, links | `GET/POST /v1/tags`, `PUT /v1/{kind}/{id}/tags`, `GET/POST /v1/{kind}/{id}/links`, `DELETE /v1/links/{id}` |
| Google Calendar | `GET /v1/calendar/status`, `GET /v1/calendar/calendars`, `GET /v1/calendar/events?from=&to=`, `GET /v1/calendar/busy?from=&to=`, `POST /v1/calendar/connect` (admin; receives tokens from the desktop sign-in), `POST /v1/calendar/disconnect` (admin) |
| Sync feed | `GET /v1/changes?since=<seq>&limit=` (for client caches) |
| Admin | `GET/POST /v1/tokens`, `DELETE /v1/tokens/{id}` (revoke), `GET /v1/audit`, `POST /v1/admin/backup` |

## 10. CLI

`zproject-cli` is a thin client of `/v1`. Server URL from
`ZPROJECT_URL` or `~/.config/zproject/cli.ini`; token from the keychain or
`ZPROJECT_TOKEN` — never a command-line flag (process listings). Output is
human tables by default, `--json` for scripts. Exit codes: 0 ok, 1 error,
2 usage, 3 validation or conflict, 4 auth, 5 rate limited.

```
zproject-cli projects list|show|create|update
zproject-cli tasks list|show|add|update|move|done|delete [--dry-run] [--confirm ID]
zproject-cli deps add|remove  "3FS+2d"
zproject-cli schedule show|critical [--project ID]
zproject-cli baseline set|show|variance
zproject-cli raid list|add|update
zproject-cli report status|variance|resources|portfolio [--format md|pdf|json]
zproject-cli export|import ...
zproject-cli token create|list|revoke  --kind agent --scopes read --projects ID,ID --expires 90d
zproject-cli audit [--token ID] [--since ...]
```

On the server, `zprojectd` has its own admin flags: `--create-owner-token`,
`--backup`, `--restore FILE` (service stopped), `--verify-audit`,
`--migrate-only`, `--version`.

## 11. MCP endpoint for agents

- Built into `zprojectd` at `/mcp` (MCP Streamable HTTP transport), behind the
  same TLS, Tailscale ACL, token auth, scopes, rate limits, confirm flow and
  audit log as `/v1` **(decided, Q25)**. A stdio wrapper `zproject-mcp`
  (calls `/v1` with a token from its environment) is phase 3, only if a client
  needs stdio.
- Tools are generated from OpenAPI operations and **filtered by the caller's
  scopes**, so an agent discovers only what it may use. Each tool's
  description states whether it is destructive and whether it needs a
  confirmation.
- Phase 1 tools: `list_projects`, `get_project`, `search_tasks`,
  `get_task`, `get_schedule`, `get_critical_path`, `get_variance`,
  `get_overallocations`, `list_raid`, `status_report`, `list_alerts`,
  `create_task`, `update_task`, `add_dependency`, `add_task_note`,
  `add_raid_item`, `update_raid_item`, `delete_task` (destructive: returns a
  dry-run diff and `confirmId`; a second call with `confirm_id` commits).
- Resources: `zproject://projects/{id}/summary`, `zproject://openapi`.
- Agents keep their token in their own secret store; it is never put in a
  prompt, a repository, chat, or a log.

## 12. Google Calendar

Read-only awareness in phase 1; the intended account is Stephen's own Google
account (configured at sign-in; examples in this file use
`owner@example.com`).

- **Sign-in:** desktop OAuth client, loopback redirect on `127.0.0.1` and PKCE
  (S256), run by the desktop app because it needs a browser. Scope
  `calendar.readonly` (calendar list, titles and busy times); the tighter pair
  `calendar.events.readonly` + `calendar.calendarlist.readonly` is an option.
  No write scopes unless write-back is opted in later.
- **Where tokens live (decided, Q11):** the desktop app sends the refresh
  token to `zprojectd` over the authenticated tailnet API
  (`POST /v1/calendar/connect`, admin scope). The server stores it in a
  root-owned mode-0600 credentials file or `systemd-creds`, never in the
  database, logs, backups' JSON export, or Box. The alternative is the
  original per-desktop design (keychain + local cache).
- **Calendars:** picker from the calendar list; primary selected by default.
- **Sync:** incremental sync tokens per calendar; on 410, full resync. Every
  15 minutes and on demand. Offline or failing: keep serving the cache marked
  stale with last-refresh time; `calendar_stale` alert after 24 h.
- **Overlay:** read-only lane on the Gantt (titles, all-day spans) and busy
  shading. Busy time does not change working days in phase 1.
- **Disconnect:** delete tokens and cache; call Google's revoke endpoint
  (best-effort when offline). `invalid_grant` → prompt to reconnect.
- **Testing mode:** an unpublished consent screen issues refresh tokens that
  expire after 7 days; accept weekly re-auth at first, publish later
  **(decided, Q12)**. Google Cloud project setup is Stephen's; nothing is
  created or published without his yes.
- **Later:** busy time as non-working time; opt-in write-back of milestones
  and deadlines only to a dedicated `zproject` calendar, with idempotent ids,
  refusing to touch events it did not create.

## 13. Suite interop

- Each app owns its data; interop is via APIs or explicit imports, never by
  writing another app's files. Shared id format (20 hex). Cross-app links are
  typed URIs in `link`: `ztodo:task/<id>`, `zproject:task/<id>`,
  `zmail:message/<id>`, `zwriter:doc/<path>`.
- **One owner per field** for linked pairs: zproject owns dates,
  dependencies, assignments and % complete of zproject tasks; ztodo owns
  title, Today and done state of ztodo tasks. No cross-app last-writer-wins.
  When a linked ztodo task is done, zproject shows "done in ztodo" and offers
  to set 100%; it never changes itself silently. A missing counterpart leaves
  a link marked missing, never a cascade delete.
- **Phase 2:** import selected ztodo tasks into a project as copies with a
  back-link.
- **Phase 3:** ztodo tasks with a due date and a chosen tag appear as a
  read-only lane on the Gantt. Source: a small read-only ztodo API
  (decided, **Q27**) or, until then, the desktop client reading the local
  ztodo folder read-only. "Send to ztodo" through ztodo's API when one exists.
- Two-way sync is not planned. Other apps (zmail "make a task from this
  email", zwriter links, zterminal via `zproject-cli`) use `/v1` with an `app`
  token.

## 14. Security and threat model

Assets: the plan (personal and possibly client data), API tokens, the Google
refresh token, the release signing key (CI only), and the integrity of the
audit trail.

| Threat | Mitigation |
|---|---|
| Someone on a shared LAN or the internet reaches the API | Server binds `127.0.0.1` only; reachable solely through `tailscale serve` on the tailnet; no Funnel, no port forward, no LAN bind by default; Tailscale ACL limits sources to Stephen's devices and the agent tag. |
| A tailnet device or agent goes rogue or is compromised | Per-token scopes and project allowlists; agent tokens read-only by default, expiring, revocable instantly; rate limits; dry-run/confirm for destructive writes; tombstones and restore; hash-chained audit log naming the token and Tailscale identity. |
| Token theft from disk, logs or process lists | Server stores only SHA-256 hashes; secrets shown once; clients keep tokens in the OS keychain or `ZPROJECT_TOKEN`; never on the command line, never in logs, crash reports, Box, the cache, or the repository. Request logs record a token *id*, never a secret. |
| Token guessing | 256-bit secrets, constant-time compare, failed-auth rate limit per source. |
| Prompt-injected agent tries mass deletion | Destructive ops need a confirm bound to the exact request; agents lack `admin`; 5 destructive ops/min; deletes are tombstones; audit makes it visible. |
| Malicious import file (CSV, JSON, MSPDI, template) | Parsers in `core/` with size and depth limits, no external entity resolution in XML, dry-run first, fuzzed in CI (§19). |
| Browser-based CSRF / DNS rebinding against `127.0.0.1` | No CORS; bearer tokens (no cookies); `Host` header must match the configured names; JSON content type required for writes. |
| Google token leakage | Root-owned 0600 file or `systemd-creds`; never in the DB, exports, logs or Box; revoke on disconnect; read-only scope. |
| Tampered update | minisign-verified `SHA256SUMS` with the compiled-in key, trusted comment `zproject vX.Y.Z SHA256SUMS`; the server is never self-updated; Stephen installs `.deb` files himself. |
| Audit tampering | Triggers block `UPDATE`/`DELETE`; SHA-256 hash chain; `zprojectd --verify-audit`; audit included in backups. |
| Database corruption | One writer process, WAL, never on a synced folder, nightly `VACUUM INTO` + `integrity_check`, tested restore. |
| Server process compromise | Hardened systemd unit: dedicated user, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp`, `NoNewPrivileges`, `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`, `ReadWritePaths=/var/lib/zproject`, `MemoryDenyWriteExecute`, `SystemCallFilter=@system-service`. |

## 15. Privacy

- Stored on the server: the plan, tokens' hashes and metadata, audit entries,
  and the Google Calendar cache (calendar id/name, event id, start, end,
  title, all-day, transparency, sync token). Not stored: attendees,
  descriptions, conference links, attachments, other people's addresses.
- Stored on clients: settings, device id, the keychain token, and a read-only
  cache of the plan on the local disk (not encrypted in phase 1; it is deleted
  when the device token is revoked or the user signs out).
- Nothing is sent anywhere except Google (only when Calendar is connected),
  GitHub (update checks by desktop clients), and ts-notify-hub on debian199 (alerts, over the tailnet).
  No telemetry.
- Example data, fixtures and docs use `example.com` addresses only.

## 16. Storage, backup and migration

### 16.1 Server storage

`/var/lib/zproject/zproject.db` (SQLite, `journal_mode=WAL`,
`synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`), owned by the
`zproject` user, mode 0600. One process writes (`zprojectd`); readers inside
the process use a connection pool. Every write transaction updates the
current-state tables and appends to `changes` (and `audit`), so history,
undo, the change log and the client sync feed share one source.

### 16.2 Why not the earlier designs

The first design used ztodo's per-device append-only logs in Box. That design
survives as the format of `changes` rows and of the phase 2 offline outbox
(hybrid clock `max(wall, highest seen + 1)`, per-field last-writer-wins,
idempotent application). It is superseded as the live store because alerting,
server-side reports, a shared calendar cache and agent access all need one
always-on process. A SQLite file synced through Box is rejected outright:
SQLite warns against live databases on network or sync filesystems; Box Drive
and rclone bisync copy the main file, `-wal` and `-shm` independently and can
fork or tear the database. Postgres is not needed for one user and a few
agents; the `Store` interface keeps it possible later.

### 16.3 Backup and restore

- Nightly systemd timer: `VACUUM INTO /var/lib/zproject/backups/zproject-<date>.db`,
  `PRAGMA integrity_check` on the copy, plus a full JSON export. Keep 30 daily
  and 12 monthly. Optional off-site copy of the snapshot and export (never the
  live file) to a Box folder through rclone **(decided, Q5)**.
- `zprojectd --restore FILE` with the service stopped: integrity check,
  schema migration if older, then replace. Tested in CI.
- Before every schema migration the server takes an automatic snapshot.

### 16.4 Schema migration

`PRAGMA user_version`; numbered, forward-only migrations in `store/migrations/`
run at startup in one transaction; a failed migration rolls back and the
server refuses to start with a clear log line. CI applies every migration from
schema 1 to head on fixture databases.

### 16.5 Clients

- Linux: `~/.config/zproject/zproject.ini` (settings),
  `~/.config/zproject/device-id`, `~/.cache/zproject/cache.db` (read-only plan
  cache), `update-state.ini`. macOS: `~/Library/Preferences/zproject/` and
  `~/Library/Caches/zproject/`. None of these are in Box.
- Cache refresh: `GET /v1/changes?since=<seq>` at start, on F5, and every 15 s
  while open (long-poll later). Offline: read-only, marked stale.
- Phase 2: offline edits written to a local outbox of change records, sent on
  reconnect; server applies with per-field last-writer-wins and returns a
  conflict list that the client shows.

### 16.6 Migration from the earlier design

No zproject data exists yet, so nothing must be converted. If Stephen chooses
the MacBook-local fallback first (Q3), moving to the server later is
`zprojectd --backup` on the MacBook and `--restore` on the server.

## 17. Performance targets

| Measure | Target |
|---|---|
| CPM reschedule, 2,000 tasks / 4,000 links | ≤ 50 ms on the server, ≤ 100 ms in the client |
| Over-allocation scan, 50 resources × 2 years | ≤ 100 ms |
| API p95 latency on the tailnet (read) | ≤ 100 ms; writes with reschedule ≤ 200 ms |
| Desktop cold start to Gantt (warm cache) | ≤ 1.5 s |
| Gantt scroll/zoom, 2,000 rows | 60 fps on the MacBook |
| Server memory, idle | ≤ 60 MB RSS |
| Report PDF (status, 200 tasks) | ≤ 2 s |
| Nightly backup, 50 MB database | ≤ 10 s |

## 18. Accessibility

Qt accessible names and descriptions on every control and Gantt element; full
keyboard operation (§8.4) including linking and moving bars by keyboard; no
meaning by colour alone (critical path uses a hatch pattern, RAG uses a letter
as well as colour, over-allocation uses an icon); high-contrast theme and
respect for the system's contrast and reduced-motion settings; font size
setting honoured across views; screen-reader smoke test (VoiceOver on macOS,
Orca on Linux) before each minor release; minimum 4.5:1 text contrast in both
light and dark themes.

## 19. Testing, fuzzing and CI

- **Unit (`core/`, `store/`):** model validation, WBS roll-ups, calendars and
  exceptions, CPM golden tests for every dependency type with lag/lead,
  constraints, manual tasks, actuals, critical path and float, baseline
  variance, cost, over-allocation, RAID transitions, templates
  (save/instantiate round-trip), migrations from every schema version,
  backup/restore with integrity check.
- **Property tests:** random acyclic graphs — forward/backward passes satisfy
  every link; float ≥ 0 when no constraints; critical tasks form a connected
  chain from start to finish; reschedule is idempotent; applying the same
  change records twice is a no-op; outbox merge is order-independent.
- **API and contract tests:** `zprojectd` on a temp database bound to
  `127.0.0.1`; every response validated against `api/openapi.yaml`; token
  scopes, project allowlists, expiry, revocation; rate limits and 429;
  dry-run and confirm (mismatch, expiry, reuse); `If-Match`; audit rows and
  hash chain; error shapes. MCP tool listing filtered by scope and a
  create/confirm/delete round-trip. No real tokens in fixtures or logs.
- **Calendar:** fake Google backend; incremental sync, 410 resync, auth
  failure, offline/stale.
- **GUI:** offscreen smoke tests of each view; Gantt drag and keyboard linking.
- **Fuzzing (libFuzzer, like zterminal and zwriter):** harnesses for the CSV
  importer, JSON import/export and template parser, MSPDI XML parser (phase 2),
  the `3FS+2d` predecessor notation, HTTP request parser and router, JSON body
  decoding, the minisign verifier, and change-record decoding. 60 s per harness
  in PR CI; nightly 20-minute runs with a cached, minimised corpus.
- **CI (`ci.yml`, on every PR and push to main; actions pinned by SHA;
  `-Werror`):** Version consistency; Linux (ubuntu-24.04) build + test +
  `cpack` both `.deb`s (built on ubuntu-24.04, matching dragon's Pop!_OS 24.04
  noble base and its distro Qt 6.4.2) + `systemd-analyze verify` of the
  `zprojectd` unit + an install smoke test in an `ubuntu:24.04` container; macOS arm64 build + test + dmg checks;
  Sanitizers (ASan + UBSan); clang-tidy; Fuzz (60 s per harness); OpenAPI
  lint; Version increase (PRs, not required). Required checks are set in
  branch protection once the repo exists.

## 20. Packaging, signing and release

- Assets per release (one tag, one signature):
  `zproject_<ver>_amd64.deb` (desktop + CLI), `zprojectd_<ver>_amd64.deb`
  (server, for Pop!_OS 24.04 / Ubuntu 24.04 noble amd64, depends on the distro
  `libqt6core6`/`libqt6network6` 6.4; systemd unit, backup timer, `zproject`
  user created in `postinst`), `zproject-<ver>-Darwin.dmg` (desktop + CLI, Apple Silicon,
  ad-hoc signed like ztodo), `SHA256SUMS`, `SHA256SUMS.minisig`.
- macOS bundle id `ee.sbj.zproject`.
- Signing: minisign key ID `32960BBA77B36F1F` (shared with zterminal and
  ztodo); trusted comment `zproject vX.Y.Z SHA256SUMS`; public key compiled
  into `core/UpdateSigningKey.cpp`. CI secrets (names only)
  `MINISIGN_SECRET_KEY`, `MINISIGN_PASSWORD` in the protected `release`
  environment, approved only by sbj-ee. SLSA build provenance attestations.
- Release procedure as zterminal: merged PRs already bumped the version and
  dated the CHANGELOG; tag `vX.Y.Z` after merge; build jobs on Linux
  (ubuntu-24.04) and macOS; publish job waits for sbj-ee's approval, combines
  checksums, signs, re-verifies against the key at the tagged commit, creates
  the GitHub release.
- Server install: Stephen verifies (`minisign -Vm SHA256SUMS -P <key>`,
  `sha256sum -c --ignore-missing SHA256SUMS`) and runs
  `sudo apt install ./zprojectd_<ver>_amd64.deb` himself. The server never
  self-updates. Clients refuse a server whose `apiVersion` major they do not
  support, and say so.

## 21. In-app updater

Desktop app only, as zterminal and ztodo: optional daily check of
`https://api.github.com/repos/sbj-ee/zproject/releases/latest`, asset download
only from that repo's release URLs, minisign verification against the
compiled-in key and the trusted comment, SHA-256 match, then Linux
`pkexec apt install` of the verified `.deb` (fallback: show the path for a
manual `sudo apt install`) and macOS open-the-dmg. Install / Later / Skip this
version. Nothing installs unless verification passes. The desktop app also
shows when the server is older or newer than itself.

## 22. Versioning rules

- Single source: `project(zproject VERSION X.Y.Z)` in `CMakeLists.txt`, used by
  all three binaries, `/v1/health`, packages and tests.
- **Every pull request bumps the version** — patch for fixes and documentation,
  minor for features (SemVer) — and adds a dated `## X.Y.Z — YYYY-MM-DD`
  CHANGELOG entry. Dependabot PRs are exempt. `scripts/check-version.sh`
  checks CMake, CHANGELOG, README and this SPEC agree;
  `check-version-increase.sh` checks the PR is greater than its base.
- API versioning is separate: `/v1` changes only additively within a major
  app version line; breaking API changes need `/v2`. Database
  `schemaVersion` increments with each migration.
- Tags `vX.Y.Z` only after merge and only with Stephen's yes.

## 23. Branding

Icon **concept B**, approved by Stephen on 2026-11-20. Source (on Software's
box until the repo exists): `/workspace/zproject-icon/concepts/zproject-B.svg`
with `zproject-B-16.png`, `-32.png`, `-128.png`, `-1024.png`. It uses zmail's
exact Z path and the z-suite family tile, with the blue accent `#2fa8f0`.
When the repo is created: commit the SVG master to `assets/icon/zproject.svg`,
generate Linux hicolor PNGs (16–512) and the macOS `.icns` in the build, and
use `#2fa8f0` as the app accent. Concepts A and C are not used.

## 24. Phased milestones

| Phase | Contents | Exit criteria |
|---|---|---|
| 0 — Repo and skeleton | After Stephen's yes: `sbj-ee/zproject`, this SPEC, CMake skeleton for three targets, CI with all jobs, icon B, `openapi.yaml` stub, release workflow dry run | CI green; dry-run signed release verified |
| 1a — Server core | `store/` with migrations, `changes`, `audit`; tokens, scopes, rate limits, dry-run/confirm; projects, tasks, WBS, dependencies (FS/SS/FF, lag), calendars, CPM, critical path; `/v1` for those; backups; `zprojectd` `.deb`; `zproject-cli` | Contract tests and fuzzers green; installed on dragon by Stephen; `tailscale serve` + ACL in place (his change) |
| 1b — Desktop MVP | Qt client: Gantt, List, inspector, keyboard, cache, offline read-only, Preferences, updater | Daily use on MacBook and vertex |
| 1c — PM features | Baselines and variance; resources, assignments, cost, over-allocation; RAID; templates; project details; notes journal; tags and links; portfolio and all-projects timeline | Approved six features complete |
| 1d — Reports, alerts, calendar, agents | Canned reports (MD/PDF), status-report snapshots, alert rules (+ ts-notify-hub), CSV/JSON/PDF/PNG export, CSV/JSON import, Google Calendar read-only (server cache), `/mcp` with phase 1 tools | First agent token in use; v1.0.0 |
| 2 | Offline edits (outbox), ztodo import with back-link, MSPDI import/export, `.ics`, SF and more constraints, multiple baselines, time tracking, attachments | — |
| 3 | Leveling, effort-driven, earned value, ztodo overlay and push (with a ztodo API), busy time in scheduling, opt-in calendar write-back, Kanban, recurring tasks, custom reports, webhooks, approval queue, stdio MCP wrapper, cross-project links | — |

## 25. Decisions

Stephen accepted every recommendation on 2026-11-20 at 3:27 PM MST ("go with
the recs"), except decision 8, which was approved 2026-11-20 4:02 PM MST by Stephen. Each entry below records the
decision. The numbers in brackets are the question numbers on the Notion parent
page. Notion questions 13–17 and 19 (the desktop-only `127.0.0.1` API, the CLI
talking to it, the keychain token, the desktop `--daemon`, the FS push flag)
are superseded by §9. Question 6 (cycles after sync merges) is moot because the
server refuses cycles on write. Question 18 (a fixed port) is kept: the server
uses `127.0.0.1:8788` on dragon (verified free; 8787 is ts-notify-hub's port
on debian199).

1. **Backend:** `zprojectd` + SQLite WAL on a home server (dragon, decision 2), desktop clients over
   Tailscale. **Accepted 2026-11-20 (Stephen: go with the recs).** [27]
2. **Host: dragon.** **Changed 2026-11-20 3:59 PM MST by Stephen: zprojectd host debian199 -> dragon.** This is a post-lock change approved by
   Stephen. It replaces the earlier decision (debian199, accepted at 3:27 PM
   with "go with the recs"). Verified read-only on 2026-11-20 at about 4:00 PM
   MST: **Pop!_OS 24.04 LTS (Ubuntu 24.04 noble base), x86_64 / amd64**, 24
   cores, 61 GiB RAM, ports 8787 and 8788 free, and no existing
   `tailscale serve` config. Evidence and coexistence notes are in §4.4.
   ts-notify-hub stays on debian199. (The earlier attempt to check debian199
   failed on every route and is moot now.) [28]
3. **Server from day one;** the MacBook-local mode is only for development.
   **Accepted 2026-11-20 (Stephen: go with the recs).** [29]
4. **Offline:** read-only in phase 1, offline edits in phase 2. **Accepted 2026-11-20 (Stephen: go with the recs).** [30]
5. **Backups:** nightly snapshots (30 daily, 12 monthly) on the server, plus
   copies (never the live file) to a Box folder such as
   `BoxSync/zproject-backups`. **Accepted 2026-11-20 (Stephen: go with the recs).** [31]
6. **Milestones are zero-duration tasks.** **Accepted 2026-11-20 (Stephen: go with the recs).** [33]
7. **Dependency types:** FS, SS and FF with lag/lead in phase 1; SF in phase 2
   (supersedes FS-only). **Accepted 2026-11-20 (Stephen: go with the recs).** [34, 5]
8. **Repository and build: Approved 2026-11-20 4:02 PM MST by Stephen** ("Go ahead and
   start building zproject", relayed by Chief of Staff). A public
   `sbj-ee/zproject` (MIT, same minisign key) is created and phase 1 starts in
   reviewable pull requests with CI, each bumping the version. Merges, tags,
   releases and installs on dragon or any machine still each need Stephen's
   yes. [11]
9. **Phase 1 views:** Gantt, List, Resources, RAID, Baselines, Reports,
   Portfolio; Kanban later. **Accepted 2026-11-20 (Stephen: go with the recs).** [1]
10. **ztodo interop timing:** none in phase 1, import with back-link in phase
    2, read-only overlay in phase 3. **Accepted 2026-11-20 (Stephen: go with the recs).** [3, 45]
11. **Google tokens and cache live on the server;** sign-in stays on the
    desktop. **Accepted 2026-11-20 (Stephen: go with the recs).** [32]
12. **OAuth Testing mode** with weekly re-auth at first; publish later if
    wanted. **Accepted 2026-11-20 (Stephen: go with the recs).** [26]
13. **Calendar scope and display:** `calendar.readonly`, event titles shown,
    only Stephen's own Google account (warn on any other). **Accepted 2026-11-20 (Stephen: go with the recs).**
    [21, 22, 23]
14. **Working calendar default:** Mon–Fri 8 h, holidays added by the user, with
    a one-click US federal import. **Accepted 2026-11-20 (Stephen: go with the recs).** [35, 9]
15. **Resources:** people only in phase 1, one shared pool. **Accepted 2026-11-20 (Stephen: go with the recs).** [36]
16. **Cost:** hourly rate + fixed task cost, USD, earned value later.
    **Accepted 2026-11-20 (Stephen: go with the recs).** [37]
17. **Baselines:** one active per project in phase 1. **Accepted 2026-11-20 (Stephen: go with the recs).** [38]
18. **RAID:** includes assumptions; 1–5 × 1–5 scoring. **Accepted 2026-11-20 (Stephen: go with the recs).** [39]
19. **Alerts via ts-notify-hub.** **Accepted 2026-11-20 (Stephen: go with the recs).** ts-notify-hub is owned by
    Software and runs on debian199 at `http://100.112.153.39:8787`. It accepts
    messages over HTTP, so `zprojectd` posts alerts to it like its other
    clients do. The exact payload follows ts-notify-hub's existing client
    contract. [40]
20. **Phase 1 exports:** JSON, CSV, PDF/PNG; MSPDI and `.ics` in phase 2.
    **Accepted 2026-11-20 (Stephen: go with the recs).** [41]
21. **Multi-project in phase 1:** portfolio and all-projects timeline;
    cross-project links later. **Accepted 2026-11-20 (Stephen: go with the recs).** [42, 8]
22. **Time tracking** in phase 2. **Accepted 2026-11-20 (Stephen: go with the recs).** [43]
23. **Templates** stored on the server and exportable as JSON files.
    **Accepted 2026-11-20 (Stephen: go with the recs).** [44]
24. **Agent token defaults:** read-only, project-scoped, 90 days, confirm
    required; write scopes per agent only on Stephen's request. **Accepted 2026-11-20 (Stephen: go with the recs).** [46]
25. **MCP is built into `zprojectd` at `/mcp`;** a stdio wrapper only if
    needed. **Accepted 2026-11-20 (Stephen: go with the recs).** [47]
26. **Destructive agent writes:** dry-run + `confirmId` in phase 1; an optional
    human-approval queue later. **Accepted 2026-11-20 (Stephen: go with the recs).** [48]
27. **ztodo API:** ztodo gets a small read-only API before the phase 3
    overlay; no two-way sync. **Accepted 2026-11-20 (Stephen: go with the recs).** [45]
28. **Agent rate limits:** 120 reads / 30 writes / 5 destructive per minute,
    adjustable per token. **Accepted 2026-11-20 (Stephen: go with the recs).** [49]
29. **Audit:** every agent and app request is logged (metadata for reads,
    before/after for writes), forever, hash-chained. **Accepted 2026-11-20 (Stephen: go with the recs).** [50]
30. **Tailscale:** `tailscale serve` on the server, tailnet only, an ACL for
    Stephen's devices plus the agent box tag (e.g. `tag:grokbot`), never
    Funnel. Stephen makes the ACL and serve changes himself. **Accepted 2026-11-20 (Stephen: go with the recs).**
    [51, 14, 15]
31. **Server package:** a signed `zprojectd` `.deb` for Pop!_OS 24.04 / Ubuntu
    noble amd64 (distro Qt 6.4), installed by Stephen, no self-update.
    Updated for the dragon host on 2026-11-20 at 3:59 PM. **Accepted 2026-11-20 (Stephen: go with the recs).** [52]
32. **Shared z-suite library:** copy patterns from ztodo/zterminal now,
    extract later. **Accepted 2026-11-20 (Stephen: go with the recs).** [10]
33. **Later calendar phases:** busy time stays display-only until opted in;
    write-back only to a dedicated `zproject` calendar after explicit opt-in.
    **Accepted 2026-11-20 (Stephen: go with the recs).** [24, 25]
34. **Icon:** concept B's SVG is the master in `assets/icon/`, accent
    `#2fa8f0`. **Accepted 2026-11-20 (Stephen: go with the recs).** [53]
