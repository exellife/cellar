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
local MAX_PHOTOS  = 12

local function now_iso()        return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time()) end
local function iso_in(days)     return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time() + days * 86400) end

-- A1.1: validate the photos array (ordered media ids from POST /media). `photos`
-- is the inbound array proxy or nil. Each id is a 32-hex media id; cap the count.
-- NB: iterate by index until nil — LuaJIT's `#` does not honor the proxy's __len
-- (table __len is 5.2+), so #photos would read as 0 and skip every check.
local function validate_photos(photos)
  if photos == nil then return true end
  local i = 1
  while true do
    local p = photos[i]
    if p == nil then break end
    if i > MAX_PHOTOS then return false, 'too many photos (max ' .. MAX_PHOTOS .. ')' end
    if type(p) ~= 'string' or #p ~= 32 or not p:match('^[0-9a-f]+$') then
      return false, 'invalid photo id'
    end
    i = i + 1
  end
  return true
end

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
    local pok, perr = validate_photos(input.photos)
    if not pok then return false, perr end
    return validate_attrs(input.category_id, input.attributes)

  elseif op == 'update' then
    input.updated_at = now_iso()
    input.seller_id  = nil                    -- ownership can't be reassigned via update
    local pok, perr = validate_photos(input.photos)
    if not pok then return false, perr end
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
  -- Numeric types (and bool, as 0/1) land in num_value for range/equality
  -- filtering; enum/text land in text_value. json_extract yields a number for
  -- JSON numbers/true/false and a string for JSON strings.
  cellar.exec([[
    INSERT INTO listing_facet (listing_id, key, num_value, text_value)
    SELECT l.id, ca.key,
           CASE WHEN ca.type IN ('int','number','bool')
                THEN json_extract(l.attributes, '$."' || ca.key || '"') END,
           CASE WHEN ca.type NOT IN ('int','number','bool')
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

-- ── A1.2: post-form contract ────────────────────────────────────────────────
-- POST /rpc/category_form {"category": "<id-or-slug>"} → the data a client needs
-- to render the post form / filters for a category, in one call: the category,
-- its breadcrumb (root→leaf), and its ordered attributes with enum options as
-- real arrays. Pure data assembled from the metadata tables — no per-category
-- code. Public (anon may call) so the form renders before login.
local function category_form(arg)
  local rows = cellar.query(
    'SELECT id, parent_id, slug, name, labels FROM category WHERE id = ? OR slug = ? LIMIT 1',
    { arg, arg })
  local cat = rows[1]
  if not cat then return nil end

  -- breadcrumb: walk parents up, then reverse to root→leaf
  local chain, cur, guard = {}, cat, 0
  while cur and guard < 16 do
    table.insert(chain, 1, { id = cur.id, slug = cur.slug, name = cur.name })
    if not cur.parent_id then break end
    local p = cellar.query('SELECT id, parent_id, slug, name FROM category WHERE id = ?', { cur.parent_id })
    cur = p[1]; guard = guard + 1
  end

  -- ordered attributes; enum options pulled as a clean array via json_each
  local defs = cellar.query(
    'SELECT id, key, label, labels, type, required, filterable, unit, depends_on ' ..
    'FROM category_attribute WHERE category_id = ? ORDER BY sort_order', { cat.id })
  local attrs = {}
  for _, d in ipairs(defs) do
    local a = {
      key = d.key, label = d.label, labels = d.labels, type = d.type,
      required = tonumber(d.required) == 1, filterable = tonumber(d.filterable) == 1,
      unit = d.unit, depends_on = d.depends_on,
    }
    if d.type == 'enum' then
      local opts = cellar.query(
        'SELECT value AS v FROM category_attribute, json_each(category_attribute.options) ' ..
        'WHERE category_attribute.id = ?', { d.id })
      local list = {}
      for _, o in ipairs(opts) do list[#list + 1] = o.v end
      a.options = list
    end
    attrs[#attrs + 1] = a
  end

  return { category = { id = cat.id, slug = cat.slug, name = cat.name, labels = cat.labels },
           breadcrumb = chain, attributes = attrs }
end

-- ── A1.3: full-text search ──────────────────────────────────────────────────
local SEARCH_LIMIT_MAX = 50
local SEARCH_LIMIT_DEF = 20

-- A token is "wordish" if it has any ASCII alphanumeric or any UTF-8 multibyte
-- byte (Cyrillic letters are multibyte). Lua's %w is ASCII-only, so we scan bytes.
local function wordish(w)
  for i = 1, #w do
    local b = w:byte(i)
    if b >= 0x80 then return true end
    if (b >= 48 and b <= 57) or (b >= 65 and b <= 90) or (b >= 97 and b <= 122) then return true end
  end
  return false
end

-- Build a safe FTS5 MATCH string from untrusted input: split on whitespace, drop
-- embedded quotes (so a term can't break out of its phrase), keep only wordish
-- tokens, quote each and prefix-match it. Space between terms = implicit AND.
local function fts_query(q)
  local terms = {}
  for w in tostring(q or ''):gmatch('%S+') do
    w = w:gsub('"', '')
    if #w > 0 and wordish(w) then terms[#terms + 1] = '"' .. w .. '"*' end
  end
  return table.concat(terms, ' ')
end

-- POST /rpc/search { q, category?, city?, limit?, offset? } → ranked active
-- listings matching the text query, with optional category/city narrowing.
local function search(args)
  args = args or {}
  local match = fts_query(args.q)
  if match == '' then return { results = {}, total = 0, limit = SEARCH_LIMIT_DEF, offset = 0 } end

  local limit = tonumber(args.limit) or SEARCH_LIMIT_DEF
  if limit < 1 then limit = SEARCH_LIMIT_DEF end
  if limit > SEARCH_LIMIT_MAX then limit = SEARCH_LIMIT_MAX end
  local offset = tonumber(args.offset) or 0
  if offset < 0 then offset = 0 end

  -- shared WHERE: text match + active, plus optional category/city. Binds are
  -- assembled in lockstep so the count and page queries stay identical.
  local conds  = { 'listings_fts MATCH ?', "l.status = 'active'" }
  local binds  = { match }
  if args.category and args.category ~= '' then conds[#conds+1] = 'l.category_id = ?'; binds[#binds+1] = args.category end
  if args.city and args.city ~= '' then conds[#conds+1] = 'l.city_id = ?'; binds[#binds+1] = args.city end
  local where = table.concat(conds, ' AND ')

  local total = cellar.query(
    'SELECT count(*) AS n FROM listings_fts JOIN listings l ON l.rowid = listings_fts.rowid WHERE ' .. where,
    binds)[1].n

  -- page query: same binds + limit/offset; bm25 ascending = most relevant first
  local pbinds = {}
  for i = 1, #binds do pbinds[i] = binds[i] end
  pbinds[#pbinds+1] = limit; pbinds[#pbinds+1] = offset
  local rows = cellar.query(
    'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.photos, l.created_at, ' ..
    'bm25(listings_fts) AS rank ' ..
    'FROM listings_fts JOIN listings l ON l.rowid = listings_fts.rowid ' ..
    'WHERE ' .. where .. ' ORDER BY rank LIMIT ? OFFSET ?', pbinds)

  return { results = rows, total = total, limit = limit, offset = offset }
end

-- ── repair / ops primitive ──────────────────────────────────────────────────
-- POST /rpc/rebuild_facets {"id": "<listing-id>"}  (admin) — reconcile one
-- listing's facets if an after() fault ever left them stale.
function rpc(name, args, who)
  if name == 'search' then
    return search(args)
  end
  if name == 'category_form' then
    return category_form(args and args.category)
  end
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
