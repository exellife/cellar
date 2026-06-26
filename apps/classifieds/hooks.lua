-- ============================================================================
-- classifieds hooks — write-time validation (A0.4) + facet-sync (A0.5).
--
-- The category_attribute table is the single source of truth: it drives the
-- post form, the filters, AND these two server-side hooks — no per-category code.
--
-- before(listings): server-owns id/seller/timestamps, then VALIDATES the
--   submitted attributes against the category's schema (required/type/enum).
--   Runs IN the write transaction, so a rejection (→ 400) rolls back the whole
--   write — invalid attributes can never land.
--
-- after(listings): rebuilds listing_facet from the *stored* JSON for the
--   category's filterable attrs, entirely in SQL (json_extract over the
--   committed row — no dependence on Lua marshaling). Idempotent (DELETE +
--   INSERT…SELECT), so it's also a safe repair primitive. Post-commit on a
--   separate connection (cellar's model): the listing is already durable; the
--   derived index is rebuilt right after. A fault here is logged, not fatal —
--   re-saving the listing (or the rebuild_facets rpc) reconciles.
-- ============================================================================

local EXPIRY_DAYS = 30

local function now_iso()        return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time()) end
local function iso_in(days)     return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time() + days * 86400) end

-- ── A0.4: validate submitted attributes against the category schema ─────────
-- `attrs` is the inbound attributes object (a read-only proxy) or nil. We drive
-- the loop from category_attribute (known keys), reading each value by key.
local function validate_attrs(category_id, attrs)
  if not category_id then return true end   -- FK guarantees a real category
  local defs = cellar.query(
    'SELECT id, key, type, required FROM category_attribute WHERE category_id = ?',
    { category_id })

  for _, d in ipairs(defs) do
    local v = attrs and attrs[d.key]
    local missing = (v == nil or v == '')

    if tonumber(d.required) == 1 and missing then
      return false, 'missing required attribute: ' .. d.key
    end

    if not missing then
      local t = d.type
      if t == 'int' or t == 'number' then
        if type(v) ~= 'number' then return false, d.key .. ' must be a number' end
        if t == 'int' and v ~= math.floor(v) then return false, d.key .. ' must be a whole number' end
      elseif t == 'bool' then
        if type(v) ~= 'boolean' then return false, d.key .. ' must be true/false' end
      elseif t == 'enum' then
        -- membership check stays in SQL (json_each over the stored options array)
        local ok = cellar.query(
          'SELECT 1 AS ok FROM category_attribute ' ..
          'WHERE id = ? AND EXISTS (SELECT 1 FROM json_each(options) WHERE value = ?)',
          { d.id, tostring(v) })
        if #ok == 0 then return false, 'invalid value for ' .. d.key end
      end
      -- 'text' accepts any scalar
    end
  end
  return true
end

function before(op, tbl, input, who)
  if tbl ~= 'listings' then return true end

  if op == 'create' then
    input.seller_id  = who.user_id            -- server-owned (anti-spoof); id via column DEFAULT
    if not input.created_at or input.created_at == '' then input.created_at = now_iso() end
    input.updated_at = input.created_at
    if not input.expires_at or input.expires_at == '' then input.expires_at = iso_in(EXPIRY_DAYS) end
    return validate_attrs(input.category_id, input.attributes)

  elseif op == 'update' then
    input.updated_at = now_iso()
    input.seller_id  = nil                    -- ownership can't be reassigned via update
    -- Validate only when attributes are part of this PATCH. category_id may be
    -- absent on a partial update; if so we can't re-validate here (the stored
    -- category still governs — a category change would carry category_id).
    if input.attributes ~= nil then
      return validate_attrs(input.category_id, input.attributes)
    end
    return true
  end

  return true   -- delete: FK ON DELETE CASCADE removes facets
end

-- ── A0.5: rebuild the derived facet index from the stored JSON ──────────────
-- One DELETE + one INSERT…SELECT, keyed by the committed listing id. json_extract
-- pulls each filterable attr's value out of the stored attributes; numeric types
-- land in num_value, everything else in text_value.
local function sync_facets(listing_id)
  cellar.exec('DELETE FROM listing_facet WHERE listing_id = ?', { listing_id })
  cellar.exec([[
    INSERT INTO listing_facet (listing_id, key, num_value, text_value)
    SELECT l.id, ca.key,
           CASE WHEN ca.type IN ('int','number')
                THEN json_extract(l.attributes, '$."' || ca.key || '"') END,
           CASE WHEN ca.type NOT IN ('int','number')
                THEN json_extract(l.attributes, '$."' || ca.key || '"') END
    FROM listings l
    JOIN category_attribute ca
      ON ca.category_id = l.category_id AND ca.filterable = 1
    WHERE l.id = ?
      AND json_extract(l.attributes, '$."' || ca.key || '"') IS NOT NULL
  ]], { listing_id })
end

function after(op, tbl, row, who)
  if tbl ~= 'listings' then return end
  if op == 'create' or op == 'update' then
    sync_facets(row.id)
  end
  -- delete: ON DELETE CASCADE already removed the facet rows
end

-- ── repair / ops primitive ──────────────────────────────────────────────────
-- POST /rpc/rebuild_facets {"id": "<listing-id>"}  (admin) — reconcile one
-- listing's facets if an after() fault ever left them stale.
function rpc(name, args, who)
  if name == 'rebuild_facets' then
    if who.role ~= 'admin' then return nil end
    local id = args and args.id
    if not id then return nil end
    sync_facets(id)
    local n = cellar.query('SELECT count(*) AS n FROM listing_facet WHERE listing_id = ?', { id })
    return { listing_id = id, facets = n[1].n }
  end
  return nil
end
