-- Test bundle for the cellar.create_user / cellar.set_password CONTEXT guard.
--   rpc make_user / reset_pw → the SAFE path (rpc holds no write lock) — should succeed.
--   before() on products with sku 'TRIGGER-CREATE' / 'TRIGGER-SETPW' calls the primitive
--     while the CRUD write path holds the app write lock — the engine must REFUSE it
--     (return nil + error) rather than re-acquire the non-recursive lock (deadlock).

function before(op, tbl, input, who)
  if op == 'create' and tbl == 'products' and input.sku == 'TRIGGER-CREATE' then
    -- runs UNDER app_db_write_lock; create_user must return nil + a "not in this
    -- context" error rather than re-acquiring the non-recursive lock (deadlock).
    local id, err = cellar.create_user('from-before@ctx.dev', 'a-password-1', 'editor')
    if not id then return false, 'guarded: ' .. err end
    return false, 'BUG: create_user succeeded under the write lock -> ' .. id
  end
  if op == 'create' and tbl == 'products' and input.sku == 'TRIGGER-SETPW' then
    -- same guard for set_password (it also takes the non-recursive write lock).
    local ok, err = cellar.set_password('admin@cellar.dev', 'a-password-2')
    if not ok then return false, 'guarded: ' .. err end
    return false, 'BUG: set_password succeeded under the write lock'
  end
  return true
end

function rpc(name, args, who)
  if name == 'make_user' then
    local id, err = cellar.create_user(args.email, args.password, args.role)
    if not id then return { error = err } end
    return { id = id }
  end
  if name == 'reset_pw' then
    local ok, err = cellar.set_password(args.email, args.new_password)
    if not ok then return { error = err } end
    return { ok = true }
  end
  return nil, 'unknown rpc: ' .. name
end
