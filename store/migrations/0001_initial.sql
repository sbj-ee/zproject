-- Schema 1 (SPEC.md §5, §16.4): meta, projects, tasks, and the append-only
-- changes and audit tables. Migrations are numbered and forward-only; the
-- runner applies every pending one in a single transaction and then sets
-- PRAGMA user_version. Never edit a migration that has been merged; add a new
-- one.

CREATE TABLE meta (
  key   TEXT PRIMARY KEY NOT NULL,
  value TEXT NOT NULL
) STRICT;

INSERT INTO meta (key, value) VALUES
  ('installationId', lower(hex(randomblob(10)))),
  ('currency', 'USD');

CREATE TABLE project (
  id              TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt       TEXT NOT NULL,
  updatedAt       TEXT NOT NULL,
  deleted         INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
  rev             INTEGER NOT NULL DEFAULT 1 CHECK (rev >= 1),
  name            TEXT NOT NULL CHECK (length(trim(name)) > 0),
  description     TEXT NOT NULL DEFAULT '',
  objectives      TEXT NOT NULL DEFAULT '',
  scope           TEXT NOT NULL DEFAULT '',
  outOfScope      TEXT NOT NULL DEFAULT '',
  successCriteria TEXT NOT NULL DEFAULT '',
  owner           TEXT NOT NULL DEFAULT '',
  sponsor         TEXT NOT NULL DEFAULT '',
  status          TEXT NOT NULL DEFAULT 'active'
                  CHECK (status IN ('active', 'on_hold', 'done', 'cancelled')),
  rag             TEXT NOT NULL DEFAULT 'green' CHECK (rag IN ('green', 'amber', 'red')),
  priority        INTEGER NOT NULL DEFAULT 2 CHECK (priority BETWEEN 1 AND 4),
  start           TEXT,
  finish          TEXT,
  calendar        TEXT,             -- calendar id; the calendar table arrives in a later migration
  budget          REAL CHECK (budget IS NULL OR budget >= 0),
  color           TEXT,
  ord             INTEGER NOT NULL DEFAULT 0,
  templateId      TEXT
) STRICT;

CREATE TABLE task (
  id              TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt       TEXT NOT NULL,
  updatedAt       TEXT NOT NULL,
  deleted         INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
  rev             INTEGER NOT NULL DEFAULT 1 CHECK (rev >= 1),
  project         TEXT NOT NULL REFERENCES project (id),
  parent          TEXT REFERENCES task (id),
  ord             INTEGER NOT NULL DEFAULT 0,
  title           TEXT NOT NULL CHECK (length(trim(title)) > 0),
  notes           TEXT NOT NULL DEFAULT '',
  type            TEXT NOT NULL DEFAULT 'task' CHECK (type IN ('task', 'milestone')),
  mode            TEXT NOT NULL DEFAULT 'auto' CHECK (mode IN ('auto', 'manual')),
  duration        REAL NOT NULL DEFAULT 1 CHECK (duration >= 0),
  start           TEXT,
  finish          TEXT,
  constraintType  TEXT NOT NULL DEFAULT 'ASAP' CHECK (constraintType IN ('ASAP', 'SNET', 'FNLT')),
  constraintDate  TEXT,
  deadline        TEXT,
  calendar        TEXT,             -- null = the project's calendar
  percentComplete INTEGER NOT NULL DEFAULT 0 CHECK (percentComplete BETWEEN 0 AND 100),
  actualStart     TEXT,
  actualFinish    TEXT,
  status          TEXT NOT NULL DEFAULT 'todo' CHECK (status IN ('todo', 'in_progress', 'done')),
  priority        INTEGER NOT NULL DEFAULT 2 CHECK (priority BETWEEN 1 AND 4),
  fixedCost       REAL NOT NULL DEFAULT 0 CHECK (fixedCost >= 0),
  CHECK ((type = 'milestone') = (duration = 0)),
  CHECK (parent IS NULL OR parent <> id)
) STRICT;

CREATE INDEX task_project_parent ON task (project, parent, ord);

-- Every write appends one row per changed entity (SPEC.md §5.1, §16.1): the
-- source for history, undo, the change log and the client sync feed.
CREATE TABLE changes (
  seq        INTEGER PRIMARY KEY AUTOINCREMENT,
  ts         TEXT NOT NULL,        -- hybrid clock, UTC ISO 8601 with ms
  actor      TEXT NOT NULL,        -- token id (or 'system')
  entityKind TEXT NOT NULL,
  entityId   TEXT NOT NULL,
  before     TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(before)),
  after      TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(after)),
  requestId  TEXT
) STRICT;

CREATE INDEX changes_entity ON changes (entityKind, entityId, seq);

-- Agent and app requests (SPEC.md §14): metadata for reads, before/after for
-- writes, hash-chained. The server fills it in a later pull request.
CREATE TABLE audit (
  seq        INTEGER PRIMARY KEY AUTOINCREMENT,
  ts         TEXT NOT NULL,
  tokenId    TEXT,
  tokenName  TEXT,
  tokenKind  TEXT,
  tsUser     TEXT,
  tsNode     TEXT,
  method     TEXT,
  path       TEXT,
  mcpTool    TEXT,
  entityKind TEXT,
  entityId   TEXT,
  before     TEXT CHECK (before IS NULL OR json_valid(before)),
  after      TEXT CHECK (after IS NULL OR json_valid(after)),
  dryRun     INTEGER NOT NULL DEFAULT 0 CHECK (dryRun IN (0, 1)),
  confirmId  TEXT,
  status     INTEGER,
  requestId  TEXT,
  prevHash   TEXT NOT NULL,
  hash       TEXT NOT NULL UNIQUE
) STRICT;

-- changes and audit are append-only (SPEC.md §5.2).
CREATE TRIGGER changes_no_update BEFORE UPDATE ON changes
BEGIN SELECT RAISE(ABORT, 'changes is append-only'); END;
CREATE TRIGGER changes_no_delete BEFORE DELETE ON changes
BEGIN SELECT RAISE(ABORT, 'changes is append-only'); END;
CREATE TRIGGER audit_no_update BEFORE UPDATE ON audit
BEGIN SELECT RAISE(ABORT, 'audit is append-only'); END;
CREATE TRIGGER audit_no_delete BEFORE DELETE ON audit
BEGIN SELECT RAISE(ABORT, 'audit is append-only'); END;
