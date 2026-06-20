-- Tasklets — hooks.lua
--
-- The whole cellar hook contract (design §8) on one small app. Every hook is
-- optional and runs server-side, per request, against THIS app's database. The
-- `cellar` table gives you parameterized SQL (cellar.query / cellar.exec) and
-- logging (cellar.log.*). Hooks are pcall-isolated and fail CLOSED: a Lua error
-- in authorize/before/on_realtime denies; in after it's logged but the write stands.

local function now() return os.time() end

-- ---------------------------------------------------------------------------
-- before(op, table, input, who) -> (ok, reason)
--   Validate and set server-owned fields BEFORE the write. `input` is a writable
--   proxy of the inbound row — mutate it in place. Returning (false, reason) makes
--   the request a 400 and rolls back anything the hook already wrote (it runs in
--   the same transaction as the main write). `who` = { authenticated, user_id, role }.
-- ---------------------------------------------------------------------------
function before(op, tbl, input, who)
  if tbl ~= 'tasks' then return true end

  if op == 'create' then
    local title = input.title and tostring(input.title):gsub('%s', '') or ''
    if #title == 0 then
      return false, 'title is required'            -- -> 400 before any write
    end
    input.owner_id = who.user_id                   -- server-owned: clients can't spoof it
    input.status   = input.status or 'todo'
    if input.status == 'done' then input.done_at = now() end

  elseif op == 'update' then
    -- A PATCH only carries changed columns. Stamp done_at when a task is completed.
    if input.status == 'done' then input.done_at = now() end
  end
  return true
end

-- ---------------------------------------------------------------------------
-- authorize(op, table, row, who) -> boolean
--   An extra allow/deny gate ON TOP of policies.json — for rules RBAC/ownership
--   can't express. Returning false -> 403.
--
--   NOTE on what `row` is here: on the WRITE path (create/update/delete) the engine
--   hands authorize the *inbound values* (the request body) — the same object before()
--   sees — NOT the stored row (and for delete that's nil; the target id lives in the
--   URL). Row-level *read* scoping is policies.json's owner_column job. So a useful
--   authorize gate keys off the input + the caller. Here: only an admin may flag a
--   task P5 (urgent) — a rule about a value, layered on top of the role policy.
-- ---------------------------------------------------------------------------
function authorize(op, tbl, row, who)
  if tbl == 'tasks' and (op == 'create' or op == 'update') then
    if row and tonumber(row.priority) == 5 and who.role ~= 'admin' then
      return false                                 -- urgent is admin-only -> 403
    end
  end
  return true
end

-- ---------------------------------------------------------------------------
-- after(op, table, row, who)
--   Post-commit side effect (the write already happened, on a fresh connection).
--   Here: append to an audit trail. A fault here is logged, not fatal.
-- ---------------------------------------------------------------------------
function after(op, tbl, row, who)
  if tbl ~= 'tasks' then return end
  cellar.exec('INSERT INTO activity(task_id, actor_id, action) VALUES (?, ?, ?)',
              { row.id, who.user_id, op })
  cellar.log.info('tasklets: ' .. who.user_id .. ' did ' .. op .. ' on task ' .. tostring(row.id))
end

-- ---------------------------------------------------------------------------
-- rpc(name, args, who) -> json
--   Anything beyond CRUD. Reached at POST /rpc/<name> with the JSON body as `args`.
--   Returns any JSON-serializable value; returning nil (or an unknown name) -> 400.
-- ---------------------------------------------------------------------------
function rpc(name, args, who)
  if name == 'board_stats' then
    -- Counts per column, for the signed-in user — one query, grouped.
    local rows = cellar.query(
      'SELECT status, count(*) AS n FROM tasks WHERE owner_id = ? GROUP BY status',
      { who.user_id })
    local out = { todo = 0, doing = 0, done = 0 }
    for _, r in ipairs(rows) do out[r.status] = r.n end
    return out

  elseif name == 'recent_activity' then
    return cellar.query(
      'SELECT action, task_id, at FROM activity WHERE actor_id = ? ORDER BY at DESC LIMIT 10',
      { who.user_id })

  elseif name == 'clear_done' then
    -- A custom bulk mutation: archive (delete) all of my completed tasks.
    local n = cellar.exec('DELETE FROM tasks WHERE owner_id = ? AND status = ?',
                          { who.user_id, 'done' })
    return { cleared = n }
  end

  return nil  -- unknown rpc -> 400 "unknown rpc"
end

-- ---------------------------------------------------------------------------
-- on_realtime(change, subscriber) -> boolean
--   A per-subscriber delivery filter for realtime CHANGE events (pure in-memory,
--   no DB). Here: only stream a task change to its owner (admins see everything).
--   `change` = { table, op, row }; `subscriber` = { user_id, role, table }.
-- ---------------------------------------------------------------------------
function on_realtime(change, subscriber)
  if subscriber.role == 'admin' then return true end
  return change.row.owner_id == subscriber.user_id
end
