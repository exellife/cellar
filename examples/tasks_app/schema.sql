-- Tasklets — schema for the cellar example app.
--
-- Layer 1 of cellar's extension model is "lean on SQLite-native logic": the engine
-- introspects this schema and respects every constraint you declare here. A table
-- you create becomes a REST resource automatically (`tasks` -> /api/tasks). No
-- migrations, no codegen — the catalog is built from `sqlite_master` at boot.
--
-- Apply with:  sqlite3 <bundle>/data.db < schema.sql   (run.sh does this for you)

-- A v4-UUID generated entirely in SQLite, so POST /api/tasks needs no `id`.
-- (Same expression the bench harness uses; portable, no extensions.)
CREATE TABLE tasks (
  id          TEXT PRIMARY KEY DEFAULT (
                lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
                substr(lower(hex(randomblob(2))),2)||'-'||
                substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||'-'||
                lower(hex(randomblob(6)))),
  title       TEXT    NOT NULL,
  -- CHECK + DEFAULT are enforced by SQLite itself: a bad status is a 400 from the
  -- engine's error mapping, no hook code required.
  status      TEXT    NOT NULL DEFAULT 'todo' CHECK (status IN ('todo','doing','done')),
  priority    INTEGER NOT NULL DEFAULT 3      CHECK (priority BETWEEN 1 AND 5),
  owner_id    TEXT    NOT NULL,        -- server-owned; the before() hook forces this
  assignee    TEXT,                    -- a free-text label (who it's for)
  created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
  done_at     INTEGER,                 -- set by the before() hook when status -> done
  -- Opt this table into offline-first sync (cellar-sync-design.md): declaring both
  -- `rev` and `deleted` makes it "syncable". The engine then stamps a monotonic
  -- `rev` on every write and turns DELETE into a soft-delete (deleted=1) so a
  -- device that was offline can learn the row went away. Both are engine-owned;
  -- clients never set them, and tombstones are invisible to normal reads.
  rev         INTEGER NOT NULL DEFAULT 0,
  deleted     INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX tasks_owner_idx ON tasks(owner_id);

-- An audit trail written by the after() hook (a post-commit side effect).
CREATE TABLE activity (
  id        INTEGER PRIMARY KEY,
  task_id   TEXT,
  actor_id  TEXT    NOT NULL,
  action    TEXT    NOT NULL,        -- 'create' | 'update' | 'delete'
  at        INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX activity_actor_idx ON activity(actor_id);
