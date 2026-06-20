-- Offline Notes — schema for the cellar offline-first sync demo.
--
-- `notes` opts into sync by declaring `rev` + `deleted` (see cellar-sync-design.md):
-- the engine stamps a monotonic rev on every write and turns DELETE into a soft-
-- delete, so an offline device can later learn a note changed or went away. The id
-- is a CLIENT-generated UUID — offline creates mint their own id and push it, so two
-- devices never collide. owner_id is server-owned (the before() hook forces it).

CREATE TABLE notes (
  id          TEXT    PRIMARY KEY,            -- client-generated UUID (offline-safe)
  title       TEXT    NOT NULL DEFAULT '',
  body        TEXT    NOT NULL DEFAULT '',
  owner_id    TEXT    NOT NULL,               -- forced by before(); never client-set
  updated_at  INTEGER NOT NULL DEFAULT (unixepoch()),  -- client edit time (drives resolve)
  rev         INTEGER NOT NULL DEFAULT 0,     -- engine-owned: sync revision
  deleted     INTEGER NOT NULL DEFAULT 0      -- engine-owned: tombstone
);
CREATE INDEX notes_owner_idx ON notes(owner_id);
