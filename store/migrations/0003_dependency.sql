-- Schema 3 (SPEC.md §5.1, §5.2; scheduling plan S3): links between tasks.
-- A link belongs to one project; both tasks must be in it (checked by the
-- store, as are cycles through the task tree). Deletes are tombstones, and a
-- task's links are tombstoned and restored with it.

CREATE TABLE dependency (
  id          TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt   TEXT NOT NULL,
  updatedAt   TEXT NOT NULL,
  deleted     INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
  rev         INTEGER NOT NULL DEFAULT 1 CHECK (rev >= 1),
  project     TEXT NOT NULL REFERENCES project (id),
  predecessor TEXT NOT NULL REFERENCES task (id),
  successor   TEXT NOT NULL REFERENCES task (id),
  type        TEXT NOT NULL DEFAULT 'FS' CHECK (type IN ('FS', 'SS', 'FF')),
  lag         INTEGER NOT NULL DEFAULT 0 CHECK (lag BETWEEN -3650 AND 3650),
  CHECK (predecessor <> successor)
) STRICT;

CREATE INDEX dependency_project ON dependency (project, deleted);
CREATE INDEX dependency_predecessor ON dependency (predecessor);
CREATE INDEX dependency_successor ON dependency (successor);
-- At most one live link per ordered pair of tasks.
CREATE UNIQUE INDEX dependency_live_pair ON dependency (predecessor, successor) WHERE deleted = 0;
