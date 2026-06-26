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

-- Mint a uuid4 server-side via SQLite (for rows whose id we need before insert).
local function server_uuid()
  return cellar.query(
    "SELECT lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||" ..
    "substr(lower(hex(randomblob(2))),2)||'-'||substr('89ab',abs(random())%4+1,1)||" ..
    "substr(lower(hex(randomblob(2))),2)||'-'||lower(hex(randomblob(6))) AS id")[1].id
end

-- A2.2: drop a notification into a user's in-app feed (the bell). Persisted, and
-- realtime-pushed to the user if they're online + subscribed (owner-scoped). The
-- realtime row carries only controlled ids (no free text → no JSON escaping
-- needed); the client refetches title/body from the notifications rpc.
local function notify(user_id, ntype, title, body, subject)
  if not user_id or user_id == '' then return end
  local id, ts = server_uuid(), now_iso()
  cellar.exec(
    'INSERT INTO notification(id, user_id, type, title, body, subject_id, created_at) VALUES (?,?,?,?,?,?,?)',
    { id, user_id, ntype, title or '', body or '', subject, ts })
  cellar.rt_emit('notification', 'INSERT',
    '{"id":"' .. id .. '","user_id":"' .. user_id .. '","type":"' .. tostring(ntype) ..
    '","subject_id":"' .. (subject or '') .. '"}')
end

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

-- A1.6 chat: a message create — sender must be a participant of the conversation
-- (owner_via scopes reads/subscribe, but VIA can't gate inserts, so we check here).
local function before_message(op, input, who)
  if op ~= 'create' then return false, 'messages are immutable' end
  if not (who and who.authenticated) then return false, 'authentication required' end
  local conv = input.conversation_id
  if not conv or conv == '' then return false, 'conversation_id required' end
  local body = input.body
  if not body or tostring(body):gsub('%s', '') == '' then return false, 'message body required' end
  local m = cellar.query(
    'SELECT 1 AS ok FROM conversation_member WHERE conversation_id = ? AND user_id = ?',
    { conv, who.user_id })
  if #m == 0 then return false, 'not a participant' end
  input.sender_id  = who.user_id           -- server-owned (anti-spoof)
  input.created_at = now_iso()             -- server-owned: never trust a client timestamp
  return true
end

-- price is optional, but if present must be a whole, non-negative number (soms).
local function valid_price(p)
  if p == nil then return true end
  return type(p) == 'number' and p >= 0 and p == math.floor(p)
end

function before(op, tbl, input, who)
  if tbl == 'message' then return before_message(op, input, who) end
  if tbl ~= 'listings' then return true end

  if op == 'create' then
    input.seller_id  = who.user_id            -- server-owned (anti-spoof); id via column DEFAULT
    input.created_at = now_iso()              -- server-owned (drives the "newest" sort)
    input.updated_at = input.created_at
    input.expires_at = iso_in(EXPIRY_DAYS)    -- server-owned
    if not valid_price(input.price) then return false, 'price must be a whole, non-negative number' end
    local pok, perr = validate_photos(input.photos)
    if not pok then return false, perr end
    return validate_attrs(input.category_id, input.attributes)

  elseif op == 'update' then
    input.updated_at = now_iso()
    input.seller_id  = nil                    -- ownership can't be reassigned via update
    input.created_at = nil                    -- can't be backdated via a PATCH
    if not valid_price(input.price) then return false, 'price must be a whole, non-negative number' end
    local pok, perr = validate_photos(input.photos)
    if not pok then return false, perr end
    -- before() sees only the PATCH body (no row id / no stored attributes), so a
    -- change to EITHER category_id OR attributes requires BOTH in the same PATCH —
    -- then validate. This blocks both bypass directions: changing attributes
    -- without a category to validate against, AND changing the category while
    -- leaving stale attributes un-revalidated (which would also pollute the
    -- shared facet index). Changing the category means resubmitting attributes
    -- anyway (the schema differs). A PATCH touching neither column is unaffected.
    if input.category_id ~= nil or input.attributes ~= nil then
      if not input.category_id or input.category_id == '' then
        return false, 'category_id is required when updating attributes'
      end
      if input.attributes == nil then
        return false, 'attributes are required when changing category'
      end
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
  if tbl == 'message' then
    if op == 'create' then       -- surface the conversation in both inboxes
      cellar.exec('UPDATE conversation SET last_message_at = ? WHERE id = ?',
                  { row.created_at, row.conversation_id })
      -- notify the OTHER participant(s) of the new message (the bell)
      local others = cellar.query(
        'SELECT user_id FROM conversation_member WHERE conversation_id = ? AND user_id <> ?',
        { row.conversation_id, row.sender_id })
      for _, m in ipairs(others) do
        notify(m.user_id, 'message', 'Новое сообщение', tostring(row.body):sub(1, 80), row.conversation_id)
      end
    end
    return
  end
  if tbl ~= 'listings' then return end
  if op == 'create' or op == 'update' then
    sync_facets(row.id)
  end
  -- delete: ON DELETE CASCADE already removed the facet rows
end

-- ── Phase 2: background jobs (claimed by the worker → dispatched here) ───────
-- POST /jobs/run (admin / worker) claims due jobs and calls this with each job's
-- type + payload. Returning normally = success (job completes); a Lua error =
-- retry/dead-letter. Unknown types are a no-op (drained).
function job(name, payload)
  if name == 'expire_listings' then
    -- sweep active listings whose ISO expires_at has passed (string compare is
    -- correct for ISO-8601); fetch first so each seller can be notified.
    local ts = now_iso()
    local due = cellar.query(
      "SELECT id, seller_id FROM listings " ..
      "WHERE status='active' AND expires_at <> '' AND expires_at < ?", { ts })
    for _, l in ipairs(due) do
      cellar.exec("UPDATE listings SET status='expired', updated_at=? WHERE id=?", { ts, l.id })
      notify(l.seller_id, 'listing_expired', 'Объявление истекло',
             'Срок размещения вашего объявления истёк', l.id)
    end
    cellar.log.info('job expire_listings: expired ' .. tostring(#due) .. ' listing(s)')
  elseif name == 'match_saved_searches' then
    match_saved_searches()               -- global (defined below); alerts on new matches
  end
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

-- A1.4: append the user's facet selections as EXISTS conditions (AND across
-- keys; OR within a key's value list). `filters` is an array proxy of
-- {key, values:[...]} (text/enum) or {key, min?, max?} (numeric range). Iterate
-- by index (proxy objects have no enumerable keys). Uses the `ff` alias so it
-- never collides with the facet-count query's `f`.
local function add_facet_conds(filters, conds, binds)
  if filters == nil then return end
  local i = 1
  while true do
    local f = filters[i]; if f == nil then break end
    i = i + 1
    local key = f.key
    if type(key) ~= 'string' or key == '' then goto continue end

    local vals = f.values
    if vals ~= nil then                         -- text/enum: text_value IN (...)
      local ph, vb, j = {}, {}, 1
      while true do local v = vals[j]; if v == nil then break end; ph[#ph+1] = '?'; vb[#vb+1] = tostring(v); j = j + 1 end
      if #ph > 0 then
        conds[#conds+1] = 'EXISTS(SELECT 1 FROM listing_facet ff WHERE ff.listing_id=l.id AND ff.key=? AND ff.text_value IN (' .. table.concat(ph, ',') .. '))'
        binds[#binds+1] = key
        for _, x in ipairs(vb) do binds[#binds+1] = x end
      end
    else                                        -- numeric range: num_value BETWEEN
      local mn, mx = tonumber(f.min), tonumber(f.max)
      if mn or mx then
        local rc, rb = { 'ff.listing_id=l.id', 'ff.key=?' }, { key }
        if mn then rc[#rc+1] = 'ff.num_value >= ?'; rb[#rb+1] = mn end
        if mx then rc[#rc+1] = 'ff.num_value <= ?'; rb[#rb+1] = mx end
        conds[#conds+1] = 'EXISTS(SELECT 1 FROM listing_facet ff WHERE ' .. table.concat(rc, ' AND ') .. ')'
        for _, x in ipairs(rb) do binds[#binds+1] = x end
      end
    end
    ::continue::
  end
end

-- POST /rpc/search — the real classifieds query: FTS candidate ids ∩ facet
-- lookups ∩ base filters (status/category/city), ranked + paged, plus sidebar
-- facet counts. `q` optional (empty q ⇒ browse). See the contract above.
local function search(args)
  args = args or {}
  local match = fts_query(args.q)

  local limit = tonumber(args.limit) or SEARCH_LIMIT_DEF
  if limit < 1 then limit = SEARCH_LIMIT_DEF end
  if limit > SEARCH_LIMIT_MAX then limit = SEARCH_LIMIT_MAX end
  local offset = tonumber(args.offset) or 0
  if offset < 0 then offset = 0 end

  -- base: status + (q) + category + city. binds assembled in '?'-order.
  local conds, binds = { "l.status = 'active'" }, {}
  local from
  if match ~= '' then
    from = 'listings_fts JOIN listings l ON l.rowid = listings_fts.rowid'
    conds[#conds+1] = 'listings_fts MATCH ?'; binds[#binds+1] = match
  else
    from = 'listings l'
  end
  if args.category and args.category ~= '' then conds[#conds+1] = 'l.category_id = ?'; binds[#binds+1] = args.category end
  if args.city and args.city ~= '' then conds[#conds+1] = 'l.city_id = ?'; binds[#binds+1] = args.city end

  -- snapshot the base (facet counts use it WITHOUT the user's facet selections,
  -- so the sidebar still shows the other available refinements)
  local base_from, base_where, base_binds = from, table.concat(conds, ' AND '), {}
  for i = 1, #binds do base_binds[i] = binds[i] end

  add_facet_conds(args.filters, conds, binds)
  local where = table.concat(conds, ' AND ')

  -- ordering: relevance only with a query; else newest. price sorts push NULLs last.
  local sort, order = args.sort, nil
  if     sort == 'price_asc'  then order = 'l.price IS NULL, l.price ASC'
  elseif sort == 'price_desc' then order = 'l.price IS NULL, l.price DESC'
  elseif sort == 'newest'     then order = 'l.created_at DESC'
  elseif match ~= ''          then order = 'bm25(listings_fts)'
  else                             order = 'l.created_at DESC' end

  local total = cellar.query('SELECT count(*) AS n FROM ' .. from .. ' WHERE ' .. where, binds)[1].n

  local pbinds = {}
  for i = 1, #binds do pbinds[i] = binds[i] end
  pbinds[#pbinds+1] = limit; pbinds[#pbinds+1] = offset
  local rows = cellar.query(
    'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.photos, l.created_at ' ..
    'FROM ' .. from .. ' WHERE ' .. where .. ' ORDER BY ' .. order .. ' LIMIT ? OFFSET ?', pbinds)

  -- sidebar facet counts (per category's filterable attrs, over the base set)
  local facets = nil
  if args.category and args.category ~= '' then
    local keys = cellar.query('SELECT key FROM category_attribute WHERE category_id = ? AND filterable = 1', { args.category })
    if #keys > 0 then
      local kph, cbinds = {}, {}
      for i = 1, #base_binds do cbinds[i] = base_binds[i] end
      for _, r in ipairs(keys) do kph[#kph+1] = '?'; cbinds[#cbinds+1] = r.key end
      local crows = cellar.query(
        'SELECT f.key AS k, f.text_value AS tv, f.num_value AS nv, count(*) AS n ' ..
        'FROM ' .. base_from .. ' JOIN listing_facet f ON f.listing_id = l.id ' ..
        'WHERE ' .. base_where .. ' AND f.key IN (' .. table.concat(kph, ',') .. ') ' ..
        'GROUP BY f.key, f.text_value, f.num_value ORDER BY f.key, n DESC', cbinds)
      facets = {}
      for _, r in ipairs(crows) do
        local v = r.tv; if v == nil then v = r.nv end
        local lst = facets[r.k]; if not lst then lst = {}; facets[r.k] = lst end
        lst[#lst+1] = { value = v, count = r.n }
      end
    end
  end

  if match ~= '' then
    cellar.emit('search', { props = '{"q":"' .. (tostring(args.q):gsub('"', '')) .. '"}' })
  end
  return { results = rows, total = total, limit = limit, offset = offset, facets = facets }
end

-- ── A1.5: listing detail ────────────────────────────────────────────────────
-- POST /rpc/listing {"id": "..."} → one listing for the detail page. Active
-- listings are also readable directly via GET /api/listings/<id> (policy grants
-- anon); this rpc adds the visibility rule the generic CRUD can't express — the
-- owner/admin may fetch their own NON-active (draft/expired/sold) listings too.
local function get_listing(args, who)
  local id = args and args.id
  if not id then return nil end
  local rows = cellar.query('SELECT * FROM listings WHERE id = ?', { id })
  local l = rows[1]
  if not l then return nil end
  if l.status ~= 'active' then
    local owner = who and who.authenticated and (who.user_id == l.seller_id or who.role == 'admin')
    if not owner then return nil end
  end
  -- social proof + the viewer's own save state (A1.7)
  local fc = cellar.query('SELECT count(*) AS n FROM favorite WHERE listing_id = ?', { id })
  local out = { listing = l, favorite_count = fc[1].n, favorited = false }
  if who and who.authenticated then
    local f = cellar.query('SELECT 1 AS ok FROM favorite WHERE user_id = ? AND listing_id = ?',
                           { who.user_id, id })
    out.favorited = #f > 0
  end
  -- behavioral telemetry (anon included) for the feed/recs — can't be backfilled
  cellar.emit('listing_viewed', { subject = id, actor = (who and who.user_id) or '' })
  return out
end

-- ── A1.7: favorites (save/unsave + my list) ─────────────────────────────────
-- All login-gated, self-guarded (rpc has no engine authz). Toggle by listing id.
local function favorite(args, who)
  if not (who and who.authenticated) then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  if not cellar.query('SELECT 1 AS ok FROM listings WHERE id = ?', { lid })[1] then return nil end
  cellar.exec('INSERT OR IGNORE INTO favorite(user_id, listing_id, created_at) VALUES (?,?,?)',
              { who.user_id, lid, now_iso() })
  cellar.emit('favorite', { actor = who.user_id, subject = lid })   -- telemetry (recs)
  return { favorited = true, listing_id = lid }
end

local function unfavorite(args, who)
  if not (who and who.authenticated) then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  cellar.exec('DELETE FROM favorite WHERE user_id = ? AND listing_id = ?', { who.user_id, lid })
  return { favorited = false, listing_id = lid }
end

-- My saved listings (any status, so a sold/expired save still shows — with its
-- status — rather than vanishing), newest-saved first.
local function favorites(args, who)
  if not (who and who.authenticated) then return nil end
  local rows = cellar.query(
    'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.photos, l.status, ' ..
    'f.created_at AS saved_at ' ..
    'FROM favorite f JOIN listings l ON l.id = f.listing_id ' ..
    'WHERE f.user_id = ? ORDER BY f.created_at DESC LIMIT 200', { who.user_id })
  return { favorites = rows }
end

-- ── A1.6: contact (phone-reveal + events) ──────────────────────────────────
-- Owner sets the contact details for their listing (kept out of the listings
-- row so it can never leak via a public read). Login-gated, owner-only.
local function set_listing_contact(args, who)
  if not (who and who.authenticated) then return nil end
  local id = args and args.listing_id
  if not id then return nil end
  local l = cellar.query('SELECT seller_id FROM listings WHERE id = ?', { id })[1]
  if not l then return nil end
  if l.seller_id ~= who.user_id and who.role ~= 'admin' then return nil end
  cellar.exec(
    'INSERT INTO listing_contact(listing_id, phone, whatsapp, updated_at) VALUES (?,?,?,?) ' ..
    'ON CONFLICT(listing_id) DO UPDATE SET phone=excluded.phone, whatsapp=excluded.whatsapp, updated_at=excluded.updated_at',
    { id, args.phone, args.whatsapp, now_iso() })
  return { ok = true, listing_id = id }
end

-- Reveal a contact channel for a listing. LOGIN-GATED (kills bulk number
-- scraping; ties each reveal to a real account) and logged as a contact_event
-- (the demand signal), except when the viewer is the seller. `channel` is
-- 'phone' (default) or 'whatsapp'; the listing's channel flag must allow it.
local function reveal_contact(args, who)
  if not (who and who.authenticated) then return nil end
  local id = args and args.listing_id
  if not id then return nil end
  local l = cellar.query(
    'SELECT seller_id, status, allow_call, allow_whatsapp FROM listings WHERE id = ?', { id })[1]
  if not l or l.status ~= 'active' then return nil end

  local c = cellar.query('SELECT phone, whatsapp FROM listing_contact WHERE listing_id = ?', { id })[1] or {}
  local out, kind = { listing_id = id }, nil
  if (args.channel or 'phone') == 'whatsapp' then
    if tonumber(l.allow_whatsapp) ~= 1 then return nil end
    out.whatsapp, kind = c.whatsapp, 'whatsapp_clicked'
  else
    if tonumber(l.allow_call) ~= 1 then return nil end
    out.phone, kind = c.phone, 'number_revealed'
  end

  if who.user_id ~= l.seller_id then       -- don't log the seller viewing their own
    cellar.exec(
      'INSERT INTO contact_event(listing_id, actor_id, seller_id, kind, created_at) VALUES (?,?,?,?,?)',
      { id, who.user_id, l.seller_id, kind, now_iso() })
    cellar.emit('contact', { actor = who.user_id, subject = id,         -- generic telemetry
                             props = '{"kind":"' .. kind .. '"}' })
  end
  return out
end

-- ── A1.6 chat: start a conversation + inbox ─────────────────────────────────
-- POST /rpc/start_conversation {"listing_id": "..."} → create-or-get the buyer's
-- conversation about a listing (login-gated; can't chat with yourself; the
-- listing must be active + allow_chat). Idempotent: dedups on (listing, buyer)
-- and ensures both membership rows exist (self-heals a partial prior run). Logs
-- chat_started once, on first creation. Membership rows are what gate message
-- reads + realtime subscribe.
local function start_conversation(args, who)
  if not (who and who.authenticated) then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  local l = cellar.query('SELECT seller_id, status, allow_chat FROM listings WHERE id = ?', { lid })[1]
  if not l or l.status ~= 'active' or tonumber(l.allow_chat) ~= 1 then return nil end
  local buyer, seller = who.user_id, l.seller_id
  if buyer == seller then return nil end

  -- Race-safe create-or-get (the rpc path autocommits per statement, no txn):
  -- INSERT OR IGNORE against UNIQUE(listing_id, buyer_id), then SELECT the id
  -- back. Two concurrent calls both no-op-or-insert; the SELECT always returns
  -- the single winner's id, and only the winner sees its own uuid (is_new) — so
  -- chat_started is logged exactly once and the loser doesn't 400.
  local mine = server_uuid()
  local ts = now_iso()
  cellar.exec('INSERT OR IGNORE INTO conversation(id, listing_id, buyer_id, seller_id, created_at, last_message_at) VALUES (?,?,?,?,?,?)',
              { mine, lid, buyer, seller, ts, ts })
  local cid = cellar.query('SELECT id FROM conversation WHERE listing_id = ? AND buyer_id = ?', { lid, buyer })[1].id
  local is_new = (cid == mine)
  -- membership: idempotent, so this also repairs a conversation left memberless
  cellar.exec('INSERT OR IGNORE INTO conversation_member(conversation_id, user_id) VALUES (?,?)', { cid, buyer })
  cellar.exec('INSERT OR IGNORE INTO conversation_member(conversation_id, user_id) VALUES (?,?)', { cid, seller })
  if is_new then
    cellar.exec('INSERT INTO contact_event(listing_id, actor_id, seller_id, kind, created_at) VALUES (?,?,?,?,?)',
                { lid, buyer, seller, 'chat_started', now_iso() })
  end
  return { conversation_id = cid, existing = not is_new }
end

-- POST /rpc/inbox → the caller's conversations (as buyer or seller), newest
-- first, with the listing title, the counterpart, and a last-message preview.
local function inbox(args, who)
  if not (who and who.authenticated) then return nil end
  local me = who.user_id
  local rows = cellar.query(
    'SELECT c.id, c.listing_id, c.buyer_id, c.seller_id, c.last_message_at, ' ..
    'l.title AS listing_title, ' ..
    'CASE WHEN c.buyer_id = ? THEN c.seller_id ELSE c.buyer_id END AS counterpart, ' ..
    '(SELECT body FROM message WHERE conversation_id = c.id ORDER BY created_at DESC LIMIT 1) AS last_message ' ..
    'FROM conversation c JOIN listings l ON l.id = c.listing_id ' ..
    'WHERE c.buyer_id = ? OR c.seller_id = ? ORDER BY c.last_message_at DESC LIMIT 100',
    { me, me, me })
  return { conversations = rows }
end

-- ── A2.1: listing lifecycle — renew ─────────────────────────────────────────
-- POST /rpc/renew_listing {listing_id} (owner): reset the expiry window and
-- reactivate (e.g. after it expired). Mark-sold/withdrawn is a normal owner PATCH
-- of status; auto-expiry is the recurring expire_listings job.
local function renew_listing(args, who)
  if not (who and who.authenticated) then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  local l = cellar.query('SELECT seller_id, status FROM listings WHERE id = ?', { lid })[1]
  if not l then return nil end
  if l.seller_id ~= who.user_id and who.role ~= 'admin' then return nil end
  if l.status == 'removed' or l.status == 'sold' then return nil end   -- nothing to renew
  cellar.exec("UPDATE listings SET status='active', expires_at=?, updated_at=? WHERE id=?",
              { iso_in(EXPIRY_DAYS), now_iso(), lid })
  return { listing_id = lid, expires_at = iso_in(EXPIRY_DAYS) }
end

-- ── A2.3: saved searches + the matcher ──────────────────────────────────────
local function save_search(args, who)
  if not (who and who.authenticated) then return nil end
  args = args or {}
  local q, cat, city = args.q, args.category, args.city
  if (not q or q == '') and (not cat or cat == '') and (not city or city == '') then
    return nil                          -- need at least one criterion
  end
  local id, ts = server_uuid(), now_iso()
  cellar.exec(
    'INSERT INTO saved_search(id, user_id, name, q, category_id, city_id, created_at, last_run_at) ' ..
    'VALUES (?,?,?,?,?,?,?,?)', { id, who.user_id, args.name or '', q or '', cat, city, ts, ts })
  return { id = id }
end

local function my_saved_searches(args, who)
  if not (who and who.authenticated) then return nil end
  return { searches = cellar.query(
    'SELECT id, name, q, category_id, city_id, notify, created_at FROM saved_search ' ..
    'WHERE user_id = ? ORDER BY created_at DESC', { who.user_id }) }
end

local function delete_saved_search(args, who)
  if not (who and who.authenticated) then return nil end
  local id = args and args.id
  if not id then return nil end
  cellar.exec('DELETE FROM saved_search WHERE id = ? AND user_id = ?', { id, who.user_id })
  return { ok = true }
end

-- The matcher job: for each saved search, notify its owner of active listings
-- created since last_run_at that match (q via FTS + category + city), then
-- advance the cursor. Global so the `job` hook (defined earlier) resolves it.
-- (Facet-filter matching is a later refinement; q/category/city covers the
-- common alerts and never over-notifies past the cursor.)
function match_saved_searches()
  local searches = cellar.query(
    'SELECT id, user_id, q, category_id, city_id, last_run_at FROM saved_search WHERE notify = 1')
  for _, ss in ipairs(searches) do
    local match = fts_query(ss.q)
    local from = 'listings l'
    local conds = { "l.status = 'active'", 'l.created_at > ?' }
    local binds = { ss.last_run_at or '' }
    if match ~= '' then
      from = 'listings_fts JOIN listings l ON l.rowid = listings_fts.rowid'
      conds[#conds+1] = 'listings_fts MATCH ?'; binds[#binds+1] = match
    end
    if ss.category_id and ss.category_id ~= '' then conds[#conds+1] = 'l.category_id = ?'; binds[#binds+1] = ss.category_id end
    if ss.city_id and ss.city_id ~= '' then conds[#conds+1] = 'l.city_id = ?'; binds[#binds+1] = ss.city_id end

    local rows = cellar.query(
      'SELECT l.id, l.title FROM ' .. from .. ' WHERE ' .. table.concat(conds, ' AND ') ..
      ' ORDER BY l.created_at LIMIT 50', binds)
    for _, r in ipairs(rows) do
      notify(ss.user_id, 'saved_search', 'Новое по вашему поиску', r.title, r.id)
    end
    cellar.exec('UPDATE saved_search SET last_run_at = ? WHERE id = ?', { now_iso(), ss.id })
  end
end

-- ── A2.2: notification feed rpcs ────────────────────────────────────────────
local function my_notifications(args, who)
  if not (who and who.authenticated) then return nil end
  local lim = math.min(tonumber(args and args.limit) or 30, 100)
  local off = math.max(tonumber(args and args.offset) or 0, 0)
  local cond = (args and args.unread_only) and ' AND read_at IS NULL' or ''
  local rows = cellar.query(
    'SELECT id, type, title, body, subject_id, read_at, created_at FROM notification ' ..
    'WHERE user_id = ?' .. cond .. ' ORDER BY created_at DESC LIMIT ? OFFSET ?',
    { who.user_id, lim, off })
  return { notifications = rows }
end

local function unread_count(args, who)
  if not (who and who.authenticated) then return nil end
  local r = cellar.query('SELECT count(*) AS n FROM notification WHERE user_id = ? AND read_at IS NULL',
                         { who.user_id })
  return { unread = r[1].n }
end

-- mark_read {id?}: one by id (owner-scoped) or all of mine; returns new unread count.
local function mark_read(args, who)
  if not (who and who.authenticated) then return nil end
  local ts = now_iso()
  if args and args.id then
    cellar.exec('UPDATE notification SET read_at = ? WHERE id = ? AND user_id = ? AND read_at IS NULL',
                { ts, args.id, who.user_id })
  else
    cellar.exec('UPDATE notification SET read_at = ? WHERE user_id = ? AND read_at IS NULL',
                { ts, who.user_id })
  end
  return unread_count(args, who)
end

-- ── repair / ops primitive ──────────────────────────────────────────────────
-- POST /rpc/rebuild_facets {"id": "<listing-id>"}  (admin) — reconcile one
-- listing's facets if an after() fault ever left them stale.
--
-- NOTES (engine authz model):
--   * /api list+get now DEFER to policy (fixed in cellar) — anon may read a table
--     that lists "anon"; search/listing rpcs remain for ranked/visibility queries.
--   * /rpc has NO authorization yet (no auth gate, _rpc whitelist not enforced) —
--     a known cellar gap (rpc authz lands in a later engine phase). Every rpc is
--     world-callable, so sensitive ones MUST self-guard: rebuild_facets checks
--     who.role == 'admin' itself.
function rpc(name, args, who)
  if name == 'search' then
    return search(args)
  end
  if name == 'listing' then
    return get_listing(args, who)
  end
  if name == 'set_listing_contact' then
    return set_listing_contact(args, who)
  end
  if name == 'reveal_contact' then
    return reveal_contact(args, who)
  end
  if name == 'start_conversation' then
    return start_conversation(args, who)
  end
  if name == 'inbox' then
    return inbox(args, who)
  end
  if name == 'favorite' then
    return favorite(args, who)
  end
  if name == 'unfavorite' then
    return unfavorite(args, who)
  end
  if name == 'favorites' then
    return favorites(args, who)
  end
  if name == 'notifications' then
    return my_notifications(args, who)
  end
  if name == 'unread_count' then
    return unread_count(args, who)
  end
  if name == 'mark_read' then
    return mark_read(args, who)
  end
  if name == 'category_form' then
    return category_form(args and args.category)
  end
  if name == 'enqueue_job' then            -- admin: schedule a background job
    if who.role ~= 'admin' then return nil end
    local t = args and args.type
    if not t then return nil end
    -- dedup recurring jobs by type so re-seeding (e.g. the daily expire sweep on
    -- every deploy) never piles up duplicates.
    if args.repeat_every and tonumber(args.repeat_every) and tonumber(args.repeat_every) > 0 then
      local ex = cellar.query("SELECT id FROM job WHERE type = ? AND repeat_every > 0 LIMIT 1", { t })[1]
      if ex then return { id = ex.id, existing = true } end
    end
    local id = cellar.enqueue_job(t, args.payload,
                                  { run_at = args.run_at, repeat_every = args.repeat_every })
    return { id = id }
  end
  if name == 'renew_listing' then
    return renew_listing(args, who)
  end
  if name == 'save_search' then
    return save_search(args, who)
  end
  if name == 'saved_searches' then
    return my_saved_searches(args, who)
  end
  if name == 'delete_saved_search' then
    return delete_saved_search(args, who)
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
