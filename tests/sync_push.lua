-- Conflict-resolution bundle for sync_push_test. resolve() decides the winner of
-- a sync_push conflict (base_rev != current rev). Here: a client can mark a write
-- "keep the server's copy" by prefixing the name with 'KEEP'; everything else is
-- last-write-wins (the default the engine would apply with no resolve at all).

-- Regression guard (fixed 2026-08-02): the engine must expose the row id to before()
-- on UPDATES too. Clients strip `id` from `values` (it rides at the mutation top
-- level), so before the fix input.id was nil on updates and self-merging guards
-- (e.g. ClerkHalls' confirmed-slot guard) silently bypassed. If id stops being
-- passed, the clean-update case in sync_push_test.py fails loudly here.
function before(op, tbl, input, who)
  if op == 'update' and (input.id == nil or input.id == '') then
    return false, 'before() missing input.id on update'
  end
  return true
end

function resolve(table, incoming, current, who)
  if incoming and incoming.name and tostring(incoming.name):match('^KEEP') then
    return 'current'      -- server's row wins; the incoming write is not applied
  end
  return 'incoming'       -- LWW
end
