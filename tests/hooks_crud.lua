-- CRUD-hook bundle for hooks_crud_test.py: exercises before/authorize/after on
-- the demo `products` table over the real REST path. (Other tests use the default
-- rpc-only hooks.lua, so this is loaded only for this test via CEL_HOOKS_FILE.)

-- before: validate + transform the input in place.
function before(op, table, input, who)
  if table ~= 'products' then return true end
  if op == 'create' then
    if not input.name or #input.name == 0 then
      return false, 'name required by hook'        -- -> 400 (before any side write)
    end
    -- In-txn side write: a trace note. Because before() now runs inside the
    -- request transaction, this rolls back with the main write if it fails (or
    -- with an authorize() deny that follows).
    cellar.exec('INSERT INTO notes(owner_id, title) VALUES (?, ?)',
                { who.user_id, 'btrace:' .. tostring(input.sku) })
    input.name = 'HOOKED:' .. input.name           -- transform, in place
  end
  return true
end

-- authorize: an additional deny gate beyond the built-in policy.
function authorize(op, table, row, who)
  if table == 'products' and op == 'create'
     and row.sku and tostring(row.sku):match('^BLOCKED') then
    return false                                   -- create gate -> 403
  end
  -- row-level READ gate: hide a fetched product whose name contains 'SECRET'.
  if table == 'products' and op == 'get'
     and row.name and tostring(row.name):find('SECRET') then
    return false                                   -- get gate -> 403
  end
  return true
end

-- after: post-commit side effect — drop an audit note owned by the caller.
function after(op, table, row, who)
  if op == 'create' and table == 'products' then
    cellar.exec('INSERT INTO notes(owner_id, title) VALUES (?, ?)',
                { who.user_id, 'audit:created:' .. tostring(row.sku) })
  end
end
