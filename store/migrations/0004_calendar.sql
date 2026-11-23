-- Schema 4 (SPEC.md §5.1, §6.1; scheduling plan S4): working calendars and
-- their exceptions. One pool for the installation; a project (and a task)
-- may name one, and Standard, seeded here, is the default. Deletes are
-- tombstones; the store refuses to delete a calendar something still uses.

CREATE TABLE calendar (
  id          TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt   TEXT NOT NULL,
  updatedAt   TEXT NOT NULL,
  deleted     INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
  rev         INTEGER NOT NULL DEFAULT 1 CHECK (rev >= 1),
  name        TEXT NOT NULL CHECK (length(trim(name)) > 0),
  base        TEXT REFERENCES calendar (id),
  -- Seven {"working": bool, "hours": number}, Monday first; NULL = the base's.
  workWeek    TEXT CHECK (workWeek IS NULL OR (json_valid(workWeek) AND json_type(workWeek) = 'array'
                                               AND json_array_length(workWeek) = 7)),
  hoursPerDay REAL NOT NULL DEFAULT 8 CHECK (hoursPerDay > 0 AND hoursPerDay <= 24),
  CHECK (base IS NULL OR base <> id)
) STRICT;

CREATE TABLE calendar_exception (
  id          TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt   TEXT NOT NULL,
  updatedAt   TEXT NOT NULL,
  deleted     INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
  rev         INTEGER NOT NULL DEFAULT 1 CHECK (rev >= 1),
  calendar    TEXT NOT NULL REFERENCES calendar (id),
  fromDate    TEXT NOT NULL CHECK (length(fromDate) = 10),
  toDate      TEXT NOT NULL CHECK (length(toDate) = 10),
  kind        TEXT NOT NULL CHECK (kind IN ('holiday', 'non_working', 'custom_hours')),
  hours       REAL NOT NULL DEFAULT 0 CHECK (hours >= 0 AND hours <= 24),
  label       TEXT NOT NULL DEFAULT '',
  CHECK (toDate >= fromDate)
) STRICT;

CREATE INDEX calendar_exception_calendar ON calendar_exception (calendar, deleted, fromDate);

-- Standard: Monday–Friday, 8 hours (its work week is NULL: the built-in one).
INSERT INTO calendar (id, createdAt, updatedAt, name, hoursPerDay)
VALUES ('00000000000000000001', strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
        'Standard', 8);

-- No API could set these before; anything there is stale. NULL = Standard.
UPDATE project SET calendar = NULL WHERE calendar IS NOT NULL;
UPDATE task SET calendar = NULL WHERE calendar IS NOT NULL;
