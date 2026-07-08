-- 0002_recs_tracking.sql — behavioral tracking + recommendations rollup.
-- Adds: track_rate (per-actor rate budget for the `track` rpc), user_interest
-- (per-user category affinity built by the rollup_interest job), rollup_state
-- (event-stream cursor). IF NOT EXISTS so it's also a no-op on a DB freshly built
-- from schema.sql. Do NOT edit once applied (cellar checksums applied migrations).

CREATE TABLE IF NOT EXISTS track_rate (
  actor_id     TEXT PRIMARY KEY,
  window_start INTEGER NOT NULL,
  count        INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS user_interest (
  user_id     TEXT NOT NULL,
  category_id TEXT NOT NULL,
  score       REAL NOT NULL DEFAULT 0,
  updated_at  TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (user_id, category_id)
);
CREATE INDEX IF NOT EXISTS idx_user_interest_user ON user_interest (user_id, score DESC);

CREATE TABLE IF NOT EXISTS rollup_state (
  name   TEXT PRIMARY KEY,
  cursor INTEGER NOT NULL DEFAULT 0
);
