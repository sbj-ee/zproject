-- Schema 2 (SPEC.md §5.1, §9.2, §9.3): API tokens and dry-run confirmations.
-- Tokens are stored as SHA-256 of the secret only; the secret is shown once.

CREATE TABLE api_token (
  id             TEXT PRIMARY KEY NOT NULL CHECK (length(id) = 20 AND id NOT GLOB '*[^0-9a-f]*'),
  createdAt      TEXT NOT NULL,
  createdBy      TEXT,                -- token id, or NULL for zprojectd --create-owner-token
  name           TEXT NOT NULL CHECK (length(trim(name)) > 0),
  kind           TEXT NOT NULL CHECK (kind IN ('owner', 'device', 'app', 'agent')),
  secretHash     TEXT NOT NULL CHECK (length(secretHash) = 64),
  scopes         TEXT NOT NULL CHECK (json_valid(scopes) AND json_type(scopes) = 'array'),
  projects       TEXT NOT NULL CHECK (json_valid(projects) AND json_type(projects) = 'array'),
  expiresAt      TEXT,                -- NULL = never
  lastUsedAt     TEXT,
  revokedAt      TEXT,
  requireConfirm INTEGER NOT NULL DEFAULT 0 CHECK (requireConfirm IN (0, 1)),
  rateLimit      TEXT CHECK (rateLimit IS NULL OR json_valid(rateLimit))
) STRICT;

-- Agents never get admin (SPEC.md §9.2, decision Q24), whatever the caller.
CREATE TRIGGER api_token_agent_no_admin BEFORE INSERT ON api_token
WHEN NEW.kind = 'agent' AND EXISTS (SELECT 1 FROM json_each(NEW.scopes) WHERE value = 'admin')
BEGIN SELECT RAISE(ABORT, 'agent tokens cannot have the admin scope'); END;
CREATE TRIGGER api_token_agent_no_admin_update BEFORE UPDATE OF scopes, kind ON api_token
WHEN NEW.kind = 'agent' AND EXISTS (SELECT 1 FROM json_each(NEW.scopes) WHERE value = 'admin')
BEGIN SELECT RAISE(ABORT, 'agent tokens cannot have the admin scope'); END;

-- A dry run of a destructive request issues a confirmation bound to the token
-- and the request hash: single use, 5-minute expiry (SPEC.md §9.3).
CREATE TABLE confirm (
  id          TEXT PRIMARY KEY NOT NULL,
  token       TEXT NOT NULL REFERENCES api_token (id),
  requestHash TEXT NOT NULL,
  createdAt   TEXT NOT NULL,
  expiresAt   TEXT NOT NULL,
  used        INTEGER NOT NULL DEFAULT 0 CHECK (used IN (0, 1))
) STRICT;

CREATE INDEX audit_token ON audit (tokenId, seq);
