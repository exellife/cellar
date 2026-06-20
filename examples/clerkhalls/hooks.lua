-- ClerkHalls hooks.
--
-- before(): server-set timestamps so created_at/updated_at are trustworthy
-- regardless of client clock skew (the client may also set them; we overwrite).
-- resolve(): conflict policy for sync — most-recently-edited row wins (by updated_at).

local function now_iso()
  -- cellar's Lua has os.time(); format a stable UTC ISO-8601 second stamp.
  return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time())
end

-- Backfill timestamps only when the client omitted them (NOT NULL columns). We do
-- NOT overwrite a client-supplied updated_at: it's the user's EDIT time, which is
-- what resolve() compares to settle an offline conflict (server apply-time would be
-- wrong for that).
function before(op, tbl, input, who)
  if op == 'create' then
    if not input.created_at or input.created_at == '' then input.created_at = now_iso() end
    if not input.updated_at or input.updated_at == '' then input.updated_at = input.created_at end
  elseif op == 'update' then
    if not input.updated_at or input.updated_at == '' then input.updated_at = now_iso() end
  end
  return true
end

-- resolve(table, incoming, current, who) -> 'incoming' | 'current'
function resolve(tbl, incoming, current, who)
  if incoming then
    if tostring(incoming.updated_at or '') >= tostring(current.updated_at or '') then
      return 'incoming'
    end
    return 'current'
  end
  return 'incoming'   -- a delete: last-write-wins
end
