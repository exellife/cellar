-- Offline Notes — hooks.lua
--
-- Two server-side rules that matter for sync:
--   before() forces the server-owned owner_id (clients never set it) — the same on a
--   normal write and on a sync_push, since push reuses the write path.
--   resolve() decides conflicts (two devices edited the same note offline): keep the
--   note with the more recent client edit time. Absent resolve would be plain LWW.

function before(op, tbl, input, who)
  if tbl ~= 'notes' then return true end
  -- Only on CREATE: owner_id is the owner_column for update/delete, so the engine
  -- scopes (and forbids modifying) it there — setting it on update would be rejected.
  if op == 'create' then
    input.owner_id = who.user_id                 -- server-owned; can't be spoofed
  end
  return true
end

-- resolve(table, incoming, current, who) -> 'incoming' | 'current'
--   incoming = the pushing device's note (nil for a delete); current = the stored one.
function resolve(tbl, incoming, current, who)
  if tbl == 'notes' and incoming then
    -- most-recently-edited wins (by the device's own updated_at)
    if (tonumber(incoming.updated_at) or 0) >= (tonumber(current.updated_at) or 0) then
      return 'incoming'
    end
    return 'current'
  end
  return 'incoming'   -- default LWW (e.g. for a delete)
end
