-- Test bundle for the cellar.create_user CONTEXT guard.
--   rpc make_user      → the SAFE path (rpc holds no write lock) — should succeed.
--   before() on products with sku 'TRIGGER-CREATE' calls create_user while the CRUD
--     write path holds the app write lock — the engine must REFUSE it (not deadlock).

function before(op, tbl, input, who)
  if op == 'create' and tbl == 'products' and input.sku == 'TRIGGER-CREATE' then
    -- runs UNDER app_db_write_lock; create_user must return nil + a "not in this
    -- context" error rather than re-acquiring the non-recursive lock (deadlock).
    local id, err = cellar.create_user('from-before@ctx.dev', 'a-password-1', 'editor')
    if not id then return false, 'guarded: ' .. err end
    return false, 'BUG: create_user succeeded under the write lock -> ' .. id
  end
  return true
end

function rpc(name, args, who)
  if name == 'make_user' then
    local id, err = cellar.create_user(args.email, args.password, args.role)
    if not id then return { error = err } end
    return { id = id }
  end
  return nil, 'unknown rpc: ' .. name
end
