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

-- A2.5 progressive-trust velocity limits (base, scaled by trust). Env-tunable.
local POST_LIMIT_24H   = tonumber(os.getenv('CLS_POST_LIMIT') or '') or 10
local CONTACT_LIMIT_1H = tonumber(os.getenv('CLS_CONTACT_LIMIT') or '') or 25

local function now_iso()        return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time()) end
local function iso_in(days)     return os.date('!%Y-%m-%dT%H:%M:%SZ', os.time() + days * 86400) end

-- Truncate to at most `max` BYTES without splitting a UTF-8 codepoint. Bodies are
-- Cyrillic (multibyte), so a raw string.sub(1,80) can land mid-sequence and emit
-- invalid UTF-8 that breaks JSON decode on the client. Back the cut off any
-- trailing continuation bytes (0x80–0xBF) so it ends on a complete character.
local function utf8_trunc(s, max)
  s = tostring(s)
  if #s <= max then return s end
  local i = max
  while i > 0 do
    local b = s:byte(i + 1)
    if not b or b < 0x80 or b >= 0xC0 then break end   -- next byte starts a new char → clean cut
    i = i - 1
  end
  return s:sub(1, i)
end

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
  -- Off-site delivery (email / web-push) via NotifChannel — reaches the user when
  -- they're NOT in the app; the feed + rt_emit above cover the online case. data
  -- carries type+subject so the client can deep-link, mirroring the realtime row.
  -- TODO (when push lands / prefs exist): suppress if seen in-app within N minutes.
  cellar.notify(user_id, {
    title = title, body = body,
    data = '{"type":"' .. tostring(ntype) .. '","subject_id":"' .. tostring(subject or '') .. '"}',
  })
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
        -- Membership check in SQL. Options are EITHER bare strings ["A"] (legacy: the
        -- string IS the code) OR objects [{code,labels}] (decoupled i18n: match the
        -- code, NOT a localized label). COALESCE(code, value) handles both formats, so
        -- the canonical stored value is always the code — never a display string.
        local ok = cellar.query(
          'SELECT 1 AS ok FROM category_attribute ' ..
          "WHERE id = ? AND EXISTS (SELECT 1 FROM json_each(options) je " ..
          "WHERE (CASE WHEN je.type='object' THEN json_extract(je.value,'$.code') ELSE je.value END) = ?)",
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
  if p == nil then return true end                       -- unpriced ("negotiable")
  return type(p) == 'number' and p > 0 and p == math.floor(p)   -- positive whole; 0 is invalid (use is_free)
end

-- Coerce a JSON-ish boolean to 0/1 (nil passes through). Accepts booleans, 1/0, and
-- the common checkbox/string encodings so a truthy value isn't silently dropped.
local function to_bool01(v)
  if v == nil then return nil end
  if v == true or v == 1 then return 1 end
  if v == false or v == 0 then return 0 end
  local s = tostring(v):lower()
  return (s == '1' or s == 'true' or s == 'on' or s == 'yes') and 1 or 0
end

-- A2.5: trust multiplier from account age + email-verified (read from the
-- engine's cel_users — same db). Brand-new + unverified = 1×; some trust = 3×;
-- established (verified + aged ≥ 7d) = 10×. Higher trust → higher velocity caps.
local function trust_mult(user_id)
  local u = cellar.query('SELECT created_at, email_verified_at FROM cel_users WHERE id = ?', { user_id })[1]
  if not u then return 1 end
  local age_days = (os.time() - (tonumber(u.created_at) or os.time())) / 86400
  local verified = u.email_verified_at ~= nil
  if verified and age_days >= 7 then return 10 end
  if verified or age_days >= 1  then return 3 end
  return 1
end

-- Email-verified gate (item 7): a verified email is required to PARTICIPATE —
-- post a listing, reveal a seller's contact, or start a chat. Browsing stays fully
-- open. Admins exempt. The client keys on the stable 'email_not_verified' signal to
-- open the verify prompt.
--
-- Per-action toggles: CLS_REQUIRE_VERIFIED_POST / CLS_REQUIRE_VERIFIED_CONTACT each
-- override the shared CLS_REQUIRE_VERIFIED default (all gates on unless set to 0).
-- e.g. CLS_REQUIRE_VERIFIED=0 CLS_REQUIRE_VERIFIED_POST=1 → gate posting only.
local REQUIRE_VERIFIED_POST    = (os.getenv('CLS_REQUIRE_VERIFIED_POST')    or os.getenv('CLS_REQUIRE_VERIFIED') or '1') ~= '0'
local REQUIRE_VERIFIED_CONTACT = (os.getenv('CLS_REQUIRE_VERIFIED_CONTACT') or os.getenv('CLS_REQUIRE_VERIFIED') or '1') ~= '0'

-- Predicate: may `who` do `action` ('post' | 'contact')? True if that action's gate
-- is off, the caller is an admin, or their email is verified.
local function is_verified(who, action)
  local gate_on = (action == 'post') and REQUIRE_VERIFIED_POST or REQUIRE_VERIFIED_CONTACT
  if not gate_on then return true end
  if who and who.role == 'admin' then return true end
  local u = who and cellar.query('SELECT email_verified_at FROM cel_users WHERE id = ?', { who.user_id })[1]
  return u ~= nil and u.email_verified_at ~= nil
end

-- rpc form for the contact actions (reveal_contact / start_conversation): the stable
-- gate error table, or nil to proceed.
local function require_verified(who)
  if is_verified(who, 'contact') then return nil end
  return { ok = false, error = 'email_not_verified' }
end

-- posts in the last 24h vs the trust-scaled cap (admins exempt).
local function over_post_limit(who)
  if who.role == 'admin' then return false end
  local n = cellar.query("SELECT count(*) AS n FROM listings WHERE seller_id = ? AND created_at > ?",
                         { who.user_id, iso_in(-1) })[1].n
  return n >= POST_LIMIT_24H * trust_mult(who.user_id)
end

-- contacts initiated (chat_started + reveals) in the last hour vs the cap.
local function over_contact_limit(who)
  if who.role == 'admin' then return false end
  local since = os.date('!%Y-%m-%dT%H:%M:%SZ', os.time() - 3600)
  local n = cellar.query("SELECT count(*) AS n FROM contact_event WHERE actor_id = ? AND created_at > ?",
                         { who.user_id, since })[1].n
  return n >= CONTACT_LIMIT_1H * trust_mult(who.user_id)
end

-- FEEDBACK ask 3: validate admin taxonomy writes beyond what the schema enforces —
-- friendly errors + the integrity SQLite can't express (well-formed enum options;
-- depends_on points at a real sibling key). Policy already limits these to admins.
local ATTR_TYPES = { enum = true, int = true, number = true, bool = true, text = true }

local function before_category(op, input)
  if op == 'delete' then return true end
  if op == 'create' and (not input.name or input.name == '') then return false, 'name is required' end
  if op == 'create' and (not input.slug or input.slug == '') then return false, 'slug is required' end
  if input.slug ~= nil and not tostring(input.slug):match('^[a-z0-9%-]+$') then
    return false, 'slug must be lowercase letters, digits, or hyphens'   -- (uniqueness is enforced by the schema)
  end
  if input.parent_id ~= nil and input.parent_id ~= '' and
     not cellar.query('SELECT 1 AS ok FROM category WHERE id = ?', { input.parent_id })[1] then
    return false, 'parent_id does not reference an existing category'
  end
  return true
end

local function before_category_attribute(op, input)
  if op == 'delete' then return true end
  if input.type ~= nil and not ATTR_TYPES[tostring(input.type)] then
    return false, 'type must be one of enum | int | number | bool | text'
  end
  if op == 'create' then
    if not input.key or input.key == '' then return false, 'key is required' end
    if not input.category_id or input.category_id == '' then return false, 'category_id is required' end
    if not input.label or input.label == '' then return false, 'label is required' end
  end
  if input.key ~= nil and not tostring(input.key):match('^[a-z][a-z0-9_]*$') then
    return false, 'key must be a lowercase identifier (letter, then letters/digits/underscore)'
  end
  if input.category_id ~= nil and input.category_id ~= '' and
     not cellar.query('SELECT 1 AS ok FROM category WHERE id = ?', { input.category_id })[1] then
    return false, 'category_id does not reference an existing category'
  end
  -- options: must be a non-empty JSON array; an enum needs it, non-enums shouldn't.
  local has_opts = input.options ~= nil and input.options ~= ''
  if has_opts then
    local r = cellar.query('SELECT json_valid(?) AS v, json_type(?) AS t, json_array_length(?) AS n',
                           { input.options, input.options, input.options })[1]
    if not r or r.v ~= 1 or r.t ~= 'array' or (tonumber(r.n) or 0) < 1 then
      return false, 'options must be a non-empty JSON array — ["A","B"] or [{"code":"a","labels":{"ky":"…"}}]'
    end
    -- Each option is a bare string (legacy: the string IS the code) OR an object with a
    -- non-empty string `code`; codes must be UNIQUE. (labels, if present, is display-only —
    -- never validated as the value, so display strings never leak into the stored data.)
    local c = cellar.query(
      "SELECT " ..
      "(SELECT count(*) FROM json_each(?) je WHERE " ..
      "   CASE WHEN je.type='object' " ..
      "        THEN (json_extract(je.value,'$.code') IS NULL OR json_extract(je.value,'$.code') = '') " ..
      "        ELSE (je.value IS NULL OR je.value = '') END) AS bad, " ..
      "(SELECT count(DISTINCT CASE WHEN je.type='object' THEN json_extract(je.value,'$.code') ELSE je.value END) " ..
      "   FROM json_each(?) je) AS uniq, " ..
      "json_array_length(?) AS n",
      { input.options, input.options, input.options })[1]
    if tonumber(c.bad) > 0 then return false, 'every option needs a non-empty `code` (or be a bare string)' end
    if tonumber(c.uniq) ~= tonumber(c.n) then return false, 'option codes must be unique' end
  end
  -- enum needs options — on create OR when a PATCH sets type='enum' (else a PATCH
  -- {type:"enum"} on a text attr would land a choiceless enum that bricks its category).
  if input.type == 'enum' and not has_opts then
    return false, 'an enum attribute needs an options array'
  end
  -- depends_on must reference an existing key in the SAME category (not a FK → check
  -- here). Requires category_id in the write so the target can be verified (an UPDATE
  -- omitting it can't be validated, so we reject rather than skip).
  if input.depends_on ~= nil and input.depends_on ~= '' then
    if input.category_id == nil or input.category_id == '' then
      return false, 'category_id is required when setting depends_on'
    end
    if not cellar.query('SELECT 1 AS ok FROM category_attribute WHERE category_id = ? AND key = ?',
                        { input.category_id, input.depends_on })[1] then
      return false, 'depends_on must reference an existing attribute key in the same category'
    end
  end
  return true
end

function before(op, tbl, input, who)
  if tbl == 'message' then return before_message(op, input, who) end
  if tbl == 'category' then return before_category(op, input) end
  if tbl == 'category_attribute' then return before_category_attribute(op, input) end
  if tbl ~= 'listings' then return true end

  if op == 'create' then
    if not is_verified(who, 'post') then return false, 'email_not_verified' end   -- item 7: verified email to post
    if over_post_limit(who) then return false, 'daily posting limit reached — try again later' end
    input.seller_id  = who.user_id            -- server-owned (anti-spoof); id via column DEFAULT
    input.created_at = now_iso()              -- server-owned (drives the "newest" sort)
    input.updated_at = input.created_at
    input.expires_at = iso_in(EXPIRY_DAYS)    -- server-owned
    -- "Free": normalize to 0/1; a free listing carries no price.
    input.is_free = to_bool01(input.is_free)
    if input.is_free == 1 then input.price = nil; input.price_negotiable = 0 end
    if not valid_price(input.price) then return false, 'price must be a whole positive number (omit for a negotiable price, or set is_free)' end
    local pok, perr = validate_photos(input.photos)
    if not pok then return false, perr end
    return validate_attrs(input.category_id, input.attributes)

  elseif op == 'update' then
    input.updated_at = now_iso()
    input.seller_id  = nil                    -- ownership can't be reassigned via update
    input.created_at = nil                    -- can't be backdated via a PATCH
    if input.is_free ~= nil then
      input.is_free = to_bool01(input.is_free)
      if input.is_free == 1 then input.price = nil; input.price_negotiable = 0 end   -- after() nulls the stored price
    elseif type(input.price) == 'number' and input.price > 0 then
      input.is_free = 0                        -- a priced update is definitionally not free (clears a stale is_free)
    end
    if not valid_price(input.price) then return false, 'price must be a whole positive number (omit for a negotiable price, or set is_free)' end
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
        notify(m.user_id, 'message', 'New message', utf8_trunc(row.body, 80), row.conversation_id)
      end
    end
    return
  end
  if tbl ~= 'listings' then return end
  if op == 'create' or op == 'update' then
    -- Enforce the free/price invariant authoritatively. before() can't write SQL NULL
    -- on update (a nil assignment UNSETS the key rather than nulling the column), so a
    -- free listing that still carries a price is reconciled here where row.id is known.
    if tonumber(row.is_free) == 1 and row.price ~= nil then
      cellar.exec('UPDATE listings SET price = NULL, price_negotiable = 0 WHERE id = ?', { row.id })
    end
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
      notify(l.seller_id, 'listing_expired', 'Listing expired',
             'Your listing has expired', l.id)
    end
    cellar.log.info('job expire_listings: expired ' .. tostring(#due) .. ' listing(s)')
  elseif name == 'match_saved_searches' then
    match_saved_searches()               -- global (defined below); alerts on new matches
  elseif name == 'rollup_interest' then
    rollup_interest()                    -- global (defined below); event stream → interest profile
  end
end

-- ── Localized auth emails (RU) ──────────────────────────────────────────────
-- render_email(kind, ctx) -> { subject?, text?, html? } (nil => engine default).
-- verify_email carries a 6-digit ctx.code (engine email-verification flow); we
-- render it in Russian. Other kinds fall through to the engine's built-in body.
function render_email(kind, ctx)
  if kind == 'verify_email' and ctx and ctx.code then
    local code = tostring(ctx.code)
    return {
      subject = 'Jarchy verification code: ' .. code,
      text = 'Your verification code: ' .. code ..
             '\r\n\r\nEnter it in the app to confirm your email address. ' ..
             'The code is valid for 15 minutes.\r\n\r\n' ..
             'If you did not sign up for Jarchy, please ignore this email.\r\n',
      html = '<div style="font-family:sans-serif;max-width:420px;margin:0 auto;color:#1c1917">' ..
             '<h2 style="color:#c2410c;margin:0 0 12px">Jarchy</h2>' ..
             '<p>Your verification code:</p>' ..
             '<p style="font-size:30px;font-weight:700;letter-spacing:6px;margin:8px 0">' .. code .. '</p>' ..
             '<p style="color:#57534e">Enter it in the app. The code is valid for 15 minutes.</p>' ..
             '<p style="color:#a8a29e;font-size:12px;margin-top:20px">' ..
             'If you did not sign up for Jarchy, please ignore this email.</p></div>',
    }
  end
  return nil
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

  -- breadcrumb: walk parents up, then reverse to root→leaf. Carry `labels` on every
  -- node (KY-first UI renders labels[locale] ?? name for headings + breadcrumbs).
  local chain, cur, guard = {}, cat, 0
  while cur and guard < 16 do
    table.insert(chain, 1, { id = cur.id, slug = cur.slug, name = cur.name, labels = cur.labels })
    if not cur.parent_id then break end
    local p = cellar.query('SELECT id, parent_id, slug, name, labels FROM category WHERE id = ?', { cur.parent_id })
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
      -- Options as [{code, labels}]: `code` is the canonical value the client sends
      -- back; `labels` is the per-locale display map (JSON string, or nil). Legacy
      -- bare-string options normalize to {code=<string>, labels=nil} — the client falls
      -- back to the code for display. No language is privileged.
      local opts = cellar.query(
        "SELECT CASE WHEN je.type='object' THEN json_extract(je.value,'$.code') ELSE je.value END AS code, " ..
        "CASE WHEN je.type='object' THEN json_extract(je.value,'$.labels') END AS labels " ..
        'FROM category_attribute ca, json_each(ca.options) je WHERE ca.id = ?', { d.id })
      local list = {}
      for _, o in ipairs(opts) do list[#list + 1] = { code = o.code, labels = o.labels } end
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
-- Card "highlights": a short, category-composed specs preview built from the typed
-- listing_facet rows (no JSON parse needed). Per category, an ordered subset of keys;
-- numeric values get a thousands separator + unit, enum/text values pass through.
local HL_KEYS = {
  ['cat-cars']       = { 'year', 'mileage', 'transmission' },
  ['cat-moto']       = { 'year', 'engine_cc' },
  ['cat-apartments'] = { 'rooms', 'area', 'floor' },
  ['cat-houses']     = { 'rooms', 'area', 'land' },
  ['cat-phones']     = { 'brand', 'storage' },
  ['cat-computers']  = { 'kind', 'ram' },
}
local HL_UNIT = { mileage='km', area='m²', engine_cc='cc', land='sotka', storage='GB', ram='GB', rooms='rooms', floor='fl' }
local function grp3(n)   -- 78000 -> "78 000"
  local s = tostring(math.floor(n)); local out, c = '', 0
  for i = #s, 1, -1 do out = s:sub(i, i) .. out; c = c + 1; if c % 3 == 0 and i > 1 then out = ' ' .. out end end
  return out
end
-- fetch typed facets for a page of listing ids -> { listing_id = { key = {num=,text=} } }
local function facets_for(ids)
  if #ids == 0 then return {} end
  local ph = {}; for i = 1, #ids do ph[i] = '?' end
  local rows = cellar.query('SELECT listing_id, key, num_value, text_value FROM listing_facet '
                            .. 'WHERE listing_id IN (' .. table.concat(ph, ',') .. ')', ids)
  local by = {}
  for _, r in ipairs(rows) do
    local m = by[r.listing_id]; if not m then m = {}; by[r.listing_id] = m end
    m[r.key] = { num = r.num_value, text = r.text_value }
  end
  return by
end
local function highlights_for(cat_id, fmap)
  local keys = HL_KEYS[cat_id]; if not keys or not fmap then return nil end
  local out = {}
  for _, k in ipairs(keys) do
    local fv = fmap[k]
    if fv and fv.num ~= nil then
      -- unit-bearing numbers get a thousands separator (78 000 km); unitless ones
      -- (e.g. year) render plain so 2019 doesn't become "2 019".
      local u = HL_UNIT[k]
      out[#out+1] = u and (grp3(fv.num) .. ' ' .. u) or tostring(math.floor(fv.num))
    elseif fv and fv.text ~= nil and fv.text ~= '' then
      out[#out+1] = tostring(fv.text)
    end
  end
  return (#out > 0) and out or nil
end

-- Raw card highlights for i18n: [{key, value}] with the STORED value (a number, or an
-- enum `code`) — NOT a composed/localized string. The client localizes: number → format
-- + unit client-side; enum code → option label (from category_form). Sits alongside the
-- legacy composed `highlights` (which stays for back-compat until the client switches).
local function highlights_kv_for(cat_id, fmap)
  local keys = HL_KEYS[cat_id]; if not keys or not fmap then return nil end
  local out = {}
  for _, k in ipairs(keys) do
    local fv = fmap[k]
    if fv and fv.num ~= nil then
      out[#out+1] = { key = k, value = fv.num }
    elseif fv and fv.text ~= nil and fv.text ~= '' then
      out[#out+1] = { key = k, value = fv.text }
    end
  end
  return (#out > 0) and out or nil
end

-- Expand a category id-or-slug to itself + all descendants (marketplace subtree
-- browse: clicking a parent like "Transport" must return the whole subtree — its
-- leaves hold the listings, the parent holds none). Returns a list of ids, empty
-- if the category is unknown. The tree is small and idx_category_parent covers
-- the recursion. Accepts a slug too (id OR slug), like category_form.
local function category_subtree(arg)
  local rows = cellar.query([[
    WITH RECURSIVE sub(id) AS (
      SELECT id FROM category WHERE id = ? OR slug = ?
      UNION ALL
      SELECT c.id FROM category c JOIN sub s ON c.parent_id = s.id
    )
    SELECT id FROM sub]], { arg, arg })
  local ids = {}
  for _, r in ipairs(rows) do ids[#ids+1] = r.id end
  return ids
end

-- Minimal JSON string-content escaper: escape backslash + double-quote, and
-- neutralize control chars (raw control bytes are invalid in JSON; UTF-8 multibyte
-- is untouched since %c matches only ASCII controls). Enough for opaque telemetry
-- props/values; not a general encoder.
local function json_str(s) return (tostring(s or ''):gsub('[\\"]', '\\%0'):gsub('%c', ' ')) end

local function search(args, who)
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
  if args.category and args.category ~= '' then
    -- subtree-inclusive: a parent matches all its descendants, a leaf just itself.
    local sub = category_subtree(args.category)
    if #sub > 0 then
      local ph = {}
      for _, id in ipairs(sub) do ph[#ph+1] = '?'; binds[#binds+1] = id end
      conds[#conds+1] = 'l.category_id IN (' .. table.concat(ph, ',') .. ')'
    else
      -- unknown category → match nothing (preserves the prior 0-result behavior)
      conds[#conds+1] = 'l.category_id = ?'; binds[#binds+1] = args.category
    end
  end
  if args.city and args.city ~= '' then conds[#conds+1] = 'l.city_id = ?'; binds[#binds+1] = args.city end

  -- snapshot the base (facet counts use it WITHOUT the user's facet selections,
  -- so the sidebar still shows the other available refinements)
  local base_from, base_where, base_binds = from, table.concat(conds, ' AND '), {}
  for i = 1, #binds do base_binds[i] = binds[i] end

  add_facet_conds(args.filters, conds, binds)
  if args.free then conds[#conds+1] = 'l.is_free = 1' end   -- "Free" filter (facet-like; free_count is over the base set)
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
    'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.district_id, ' ..
    'l.condition, l.price_negotiable, l.is_free, l.seller_id, l.photos, l.created_at, ' ..
    'up.display_name AS seller_name, ' ..
    '(SELECT count(*) FROM favorite fav WHERE fav.listing_id = l.id) AS saved_count ' ..
    'FROM ' .. from .. ' LEFT JOIN user_profile up ON up.id = l.seller_id ' ..
    'WHERE ' .. where .. ' ORDER BY ' .. order .. ' LIMIT ? OFFSET ?', pbinds)

  -- card highlights: one facet query for the page, composed per category
  local ids = {}; for _, r in ipairs(rows) do ids[#ids+1] = r.id end
  local fmap = facets_for(ids)
  for _, r in ipairs(rows) do
    r.highlights = highlights_for(r.category_id, fmap[r.id])
    r.highlights_kv = highlights_kv_for(r.category_id, fmap[r.id])
  end

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

  -- "Free" count over the base set (like a facet), so the sidebar can offer the toggle
  local free_count = cellar.query(
    'SELECT count(*) AS n FROM ' .. base_from .. ' WHERE ' .. base_where .. ' AND l.is_free = 1', base_binds)[1].n

  -- Behavioral capture for interest inference / recs (drained later by a rollup).
  -- Fire on a text query OR a category browse — a category browse is the strongest
  -- interest signal, and was previously dropped. Not fired on the bare home feed.
  -- Capture the actor (empty for anon) + intent (q/category/city). Can't backfill.
  if match ~= '' or (args.category and args.category ~= '') then
    cellar.emit('search', {
      actor = (who and who.user_id) or '',
      props = '{"q":"' .. json_str(args.q) .. '","category":"' .. json_str(args.category)
              .. '","city":"' .. json_str(args.city) .. '"}'
    })
  end
  return { results = rows, total = total, limit = limit, offset = offset, facets = facets, free_count = free_count }
end

-- ── Home feed (non-personalized + personalized) ─────────────────────────────
-- POST /rpc/feed {city?, limit?, offset?} — the "what to show when you open the app"
-- surface (no query/facets — that's search). Ranks ACTIVE listings by a blended,
-- BOUNDED score (pure arithmetic — no SQLite math-ext dependency):
--   fresh = 1/(1 + age_days/TAU)                                    ∈ (0,1]
--   near  = same-city-as-viewer boost                              ∈ {0,1}
--   pop   = raw/(raw+K), raw = 3·favs + 5·contacts + 1·views       ∈ [0,1)
--   aff   = s/(s+K), s = user_interest.score for the listing's cat ∈ [0,1)
-- Logged in → the affinity term personalizes; anon (uid '') → the user_interest join
-- misses → aff=0 → non-personalized. Same rpc serves both.
-- Scale note: scores the whole active set per call (correlated pop subqueries) — fine at
-- launch volume; promote to a rollup-maintained popularity counter if it gets hot.
local FEED_LIMIT_DEF = 20
local FEED_FRESH_TAU = 7.0     -- days
local FEED_POP_K     = 5.0     -- popularity saturation constant
local FEED_AFF_K     = 5.0     -- affinity saturation constant
local FEED_W_FRESH   = 1.0
local FEED_W_NEAR    = 0.6
local FEED_W_POP     = 0.8
local FEED_W_AFF     = 1.2     -- personalization pulls hard when a profile exists

local FEED_SCORE = string.format(
  '( %s*(1.0/(1.0 + (julianday(\'now\') - julianday(l.created_at))/%s)) '
  .. '+ %s*(CASE WHEN ? <> \'\' AND l.city_id = ? THEN 1.0 ELSE 0.0 END) '
  .. '+ %s*(CAST(l.raw_pop AS REAL)/(l.raw_pop + %s)) '
  .. '+ %s*(COALESCE(ui.score,0)/(COALESCE(ui.score,0) + %s)) ) AS feed_score',
  FEED_W_FRESH, FEED_FRESH_TAU, FEED_W_NEAR, FEED_W_POP, FEED_POP_K, FEED_W_AFF, FEED_AFF_K)

local function feed(args, who)
  args = args or {}
  local limit = tonumber(args.limit) or FEED_LIMIT_DEF
  if limit < 1 or limit > 50 then limit = FEED_LIMIT_DEF end
  local offset = tonumber(args.offset) or 0
  if offset < 0 then offset = 0 end
  local city = (type(args.city) == 'string') and args.city or ''
  local uid  = (who and who.user_id) or ''

  local total = cellar.query("SELECT count(*) AS n FROM listings WHERE status = 'active'")[1].n

  local rows = cellar.query(
    'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.district_id, ' ..
    'l.condition, l.price_negotiable, l.is_free, l.seller_id, l.photos, l.created_at, ' ..
    'up.display_name AS seller_name, ' ..
    '(SELECT count(*) FROM favorite fav WHERE fav.listing_id = l.id) AS saved_count, ' ..
    FEED_SCORE .. ' ' ..
    'FROM ( SELECT l0.*, ' ..
    '         (3*(SELECT count(*) FROM favorite f      WHERE f.listing_id  = l0.id) ' ..
    '        + 5*(SELECT count(*) FROM contact_event ce WHERE ce.listing_id = l0.id) ' ..
    "        + 1*(SELECT count(*) FROM event ev WHERE ev.type = 'listing_viewed' AND ev.subject_id = l0.id)) AS raw_pop " ..
    "       FROM listings l0 WHERE l0.status = 'active' ) l " ..
    'LEFT JOIN user_interest ui ON ui.user_id = ? AND ui.category_id = l.category_id ' ..
    'LEFT JOIN user_profile up ON up.id = l.seller_id ' ..
    'ORDER BY feed_score DESC, l.created_at DESC LIMIT ? OFFSET ?',
    { city, city, uid, limit, offset })

  local ids = {}; for _, r in ipairs(rows) do ids[#ids+1] = r.id end
  local fmap = facets_for(ids)
  for _, r in ipairs(rows) do
    r.highlights = highlights_for(r.category_id, fmap[r.id])
    r.highlights_kv = highlights_kv_for(r.category_id, fmap[r.id])
  end

  -- Cards = search's card columns + highlights + `feed_score` (the blended ranking
  -- score). feed_score is an INTENTIONAL feed-only field (search has no ranking score):
  -- a strict superset of the search card, derived only from public signals + the
  -- caller's own affinity (no cross-user leak). raw_pop stays inside the subquery.
  return { results = rows, total = total, limit = limit, offset = offset, personalized = (uid ~= '') }
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
  -- public seller card (phone stays OUT — reveal-only via reveal_contact). name is
  -- NULL until the seller sets a profile; member_since is a unix epoch (client formats).
  local su = cellar.query(
    'SELECT u.created_at AS member_since, (u.email_verified_at IS NOT NULL) AS verified, ' ..
    'up.display_name AS name FROM cel_users u LEFT JOIN user_profile up ON up.id = u.id ' ..
    'WHERE u.id = ?', { l.seller_id })[1]
  if su then
    local lc = cellar.query("SELECT count(*) AS n FROM listings WHERE seller_id = ? AND status = 'active'",
                            { l.seller_id })[1]
    out.seller = { id = l.seller_id, name = su.name, member_since = su.member_since,
                   verified = (su.verified == 1), listing_count = lc.n }
  end
  -- behavioral telemetry (anon included) for the feed/recs — can't be backfilled
  cellar.emit('listing_viewed', { subject = id, actor = (who and who.user_id) or '' })
  return out
end

-- ── A1.7: favorites (save/unsave + my list) ─────────────────────────────────
-- All login-gated via the engine _rpc whitelist (self-guards are defense-in-depth). Toggle by listing id.
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
    'SELECT l.id, l.title, l.price, l.currency, l.is_free, l.price_negotiable, l.category_id, l.city_id, l.photos, l.status, ' ..
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
  local gate = require_verified(who); if gate then return gate end   -- item 7: verified email to contact
  if over_contact_limit(who) then return nil end     -- A2.5 velocity gate (anti number-scraping)
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
  local gate = require_verified(who); if gate then return gate end   -- item 7: verified email to contact
  if over_contact_limit(who) then return nil end     -- A2.5 velocity gate (anti spam-DM)
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
  -- 'pending' is a moderation hold (auto-hide) — only admin reinstate clears it,
  -- never the owner's renew (else a brigaded seller self-un-hides). 'removed'/'sold'
  -- are terminal for renew too.
  if l.status == 'removed' or l.status == 'sold' or l.status == 'pending' then return nil end
  cellar.exec("UPDATE listings SET status='active', expires_at=?, updated_at=? WHERE id=?",
              { iso_in(EXPIRY_DAYS), now_iso(), lid })
  return { listing_id = lid, expires_at = iso_in(EXPIRY_DAYS) }
end

-- A2.5: what the UI shows ("you can post N more today").
local function my_limits(args, who)
  if not (who and who.authenticated) then return nil end
  local mult = trust_mult(who.user_id)
  local used = cellar.query("SELECT count(*) AS n FROM listings WHERE seller_id = ? AND created_at > ?",
                            { who.user_id, iso_in(-1) })[1].n
  return { trust = mult, post_limit_24h = POST_LIMIT_24H * mult, posts_used_24h = used,
           contact_limit_1h = CONTACT_LIMIT_1H * mult }
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
      'SELECT l.id, l.title, l.created_at FROM ' .. from .. ' WHERE ' .. table.concat(conds, ' AND ') ..
      ' ORDER BY l.created_at LIMIT 50', binds)
    for _, r in ipairs(rows) do
      notify(ss.user_id, 'saved_search', 'New match for your search', r.title, r.id)
    end
    -- Advance the cursor only as far as we actually processed. If we hit the page
    -- limit there may be more matches between the last row and now, so park the
    -- cursor on the last row's created_at (the next run picks up the rest);
    -- only jump to now when the page wasn't full (we drained everything).
    local cursor = (#rows >= 50) and rows[#rows].created_at or now_iso()
    cellar.exec('UPDATE saved_search SET last_run_at = ? WHERE id = ?', { cursor, ss.id })
  end
end

-- ── Interest-profile rollup (recs) ──────────────────────────────────────────
-- Drain the EventSink `event` stream by id cursor and fold it into per-user
-- category affinity (user_interest). Intent-weighted; existing scores decay each
-- run (recency bias). ATOMIC: decay + drain + upsert + cursor advance run in one
-- transaction, so a crash/retry re-processes from the UNCHANGED cursor cleanly
-- (no double-decay / double-count). Anon (actor='') and category-less events skip.
local ROLLUP_BATCH  = 5000          -- events processed per run (bounded)
local ROLLUP_DECAY  = 0.9           -- multiply existing scores each run (~1-week half-life at daily cadence)
local ROLLUP_WEIGHT = {             -- intent-scaled contribution per event type
  contact = 5.0, favorite = 3.0, search = 2.0, search_view = 2.0,
  result_click = 1.5, listing_viewed = 1.0, dwell = 1.0,
  card_view = 0.1, impression = 0.1,
}

local function rollup_drain()
  local st = cellar.query("SELECT cursor FROM rollup_state WHERE name = 'user_interest'")[1]
  local cursor = st and tonumber(st.cursor) or 0
  cellar.exec('UPDATE user_interest SET score = score * ?', { ROLLUP_DECAY })   -- decay (recency bias)
  local rows = cellar.query(
    'SELECT id, type, actor_id, subject_id, props FROM event WHERE id > ? ORDER BY id LIMIT ?',
    { cursor, ROLLUP_BATCH })
  local last, ts = cursor, now_iso()
  for _, e in ipairs(rows) do
    last = e.id
    local w, actor = ROLLUP_WEIGHT[e.type], e.actor_id
    if w and actor and actor ~= '' then
      local cat                                        -- searches carry the category in props; others via the listing
      if e.type == 'search' or e.type == 'search_view' then
        local c = cellar.query("SELECT json_extract(?, '$.category') AS c", { e.props })[1]
        local raw = c and c.c
        if raw and raw ~= '' then                       -- props.category is CLIENT-supplied (id OR slug OR junk):
          local m = cellar.query('SELECT id FROM category WHERE id = ? OR slug = ? LIMIT 1', { raw, raw })[1]
          cat = m and m.id                              -- canonicalize to a real category_id; unknown → skip
        end                                             -- (keeps the feed join valid + blocks arbitrary-row injection)
      elseif e.subject_id and e.subject_id ~= '' then
        local l = cellar.query('SELECT category_id FROM listings WHERE id = ?', { e.subject_id })[1]
        cat = l and l.category_id
      end
      if cat and cat ~= '' then
        cellar.exec(
          'INSERT INTO user_interest(user_id, category_id, score, updated_at) VALUES (?,?,?,?) ' ..
          'ON CONFLICT(user_id, category_id) DO UPDATE SET score = score + ?, updated_at = ?',
          { actor, cat, w, ts, w, ts })
      end
    end
  end
  if st then cellar.exec("UPDATE rollup_state SET cursor = ? WHERE name = 'user_interest'", { last })
  else       cellar.exec("INSERT INTO rollup_state(name, cursor) VALUES ('user_interest', ?)", { last }) end
  return #rows, last
end

function rollup_interest()
  cellar.exec('BEGIN')
  local ok, a, b = pcall(rollup_drain)
  if ok then
    cellar.exec('COMMIT')
    cellar.log.info('job rollup_interest: processed ' .. tostring(a) .. ' event(s), cursor -> ' .. tostring(b))
  else
    cellar.exec('ROLLBACK')
    error(a)                                            -- propagate → job fails → retry from unchanged cursor
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

-- ── Trust & safety: report → moderation queue → takedown ────────────────────
-- Public (login-gated) reporting + admin review/action. Takedown flips a listing
-- to 'removed' (out of public search/detail, which enforce status='active');
-- reinstate reverses it. All access is via these rpcs — the listing_report table
-- is admin-only for /api (defense-in-depth).
local REPORT_REASONS = { spam=true, scam=true, prohibited=true, offensive=true,
                         duplicate=true, miscat=true, other=true }

-- Optional auto-hide: once a listing accrues >= N DISTINCT open reports, move it
-- to 'pending' (hidden from the public, REVERSIBLE via reinstate) pending admin
-- review. 0 = off (default) → rely on manual moderation. Env-tunable. Counts
-- distinct reporters (the UNIQUE index guarantees one row per reporter), so a
-- single account can't trip it — real brigading needs N genuine accounts.
local AUTO_HIDE_REPORTS = tonumber(os.getenv('CLS_AUTO_HIDE_REPORTS') or '') or 0

-- report_listing {listing_id, reason, note?} (user): flag a listing for review.
-- Idempotent per (listing, reporter): a repeat is a silent no-op (returns ok).
local function report_listing(args, who)
  if not (who and who.authenticated) then return nil end
  local lid = args and args.listing_id
  local reason = args and args.reason
  if not lid or type(reason) ~= 'string' or not REPORT_REASONS[reason] then
    return { ok = false, error = 'listing_id and a valid reason are required' }
  end
  local l = cellar.query('SELECT id, seller_id, status FROM listings WHERE id = ?', { lid })[1]
  -- Only ACTIVE (publicly-visible) listings are reportable. Collapse absent +
  -- non-active into the SAME 'no such listing' response so report_listing can't be
  -- used as an existence/ownership oracle for removed/draft/pending/sold rows
  -- (search()/get_listing() already hide those from non-owners).
  if not l or l.status ~= 'active' then return { ok = false, error = 'no such listing' } end
  if l.seller_id == who.user_id then
    return { ok = false, error = 'you cannot report your own listing' }
  end
  local id, ts = server_uuid(), now_iso()
  -- Dedupe is on OPEN reports only (partial index): a repeat while open is ignored,
  -- but a reporter can re-flag after their prior report was resolved (reoffense).
  cellar.exec(
    "INSERT OR IGNORE INTO listing_report(id, listing_id, reporter_id, reason, note, status, created_at) " ..
    "VALUES (?,?,?,?,?,'open',?)", { id, lid, who.user_id, reason, utf8_trunc(args.note or '', 1000), ts })
  cellar.emit('listing_reported', { actor = who.user_id, subject = lid })   -- telemetry (T&S signals)
  if AUTO_HIDE_REPORTS > 0 then
    local n = cellar.query(
      "SELECT count(*) AS n FROM listing_report WHERE listing_id = ? AND status = 'open'", { lid })[1].n
    if n >= AUTO_HIDE_REPORTS then
      -- record the pre-hold status so reinstate/dismiss restore the true prior state.
      cellar.exec("UPDATE listings SET pre_moderation_status='active', status='pending', updated_at=? " ..
                  "WHERE id=? AND status='active'", { ts, lid })
    end
  end
  return { ok = true }
end

-- list_reports {status?, limit?, offset?} (admin): the moderation queue, one row
-- per reported listing with its report count + distinct reasons (what a queue UI
-- renders), newest report first. status defaults to 'open'.
local function list_reports(args, who)
  if not (who and who.role == 'admin') then return nil end
  local status = (args and args.status) or 'open'
  local lim = math.min(tonumber(args and args.limit) or 50, 200)
  local off = math.max(tonumber(args and args.offset) or 0, 0)
  local rows = cellar.query(
    "SELECT r.listing_id, l.title, l.status AS listing_status, l.seller_id, " ..
    "count(*) AS reports, group_concat(DISTINCT r.reason) AS reasons, max(r.created_at) AS last_reported " ..
    "FROM listing_report r JOIN listings l ON l.id = r.listing_id " ..
    "WHERE r.status = ? GROUP BY r.listing_id ORDER BY last_reported DESC LIMIT ? OFFSET ?",
    { status, lim, off })
  return { reports = rows }
end

-- listing_reports {listing_id} (admin): every report on one listing (drill-in).
local function listing_reports_rpc(args, who)
  if not (who and who.role == 'admin') then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  return { listing_id = lid, reports = cellar.query(
    "SELECT id, reporter_id, reason, note, status, created_at, resolved_at, resolved_by " ..
    "FROM listing_report WHERE listing_id = ? ORDER BY created_at DESC", { lid }) }
end

-- takedown_listing {listing_id, note?} (admin): remove from public view and
-- resolve its open reports as 'actioned'. Records the prior status (so reinstate
-- restores the TRUE state, not a guessed 'active') and is idempotent — a repeat
-- takedown on an already-'removed' listing is a silent no-op (no re-notify).
local function takedown_listing(args, who)
  if not (who and who.role == 'admin') then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  local l = cellar.query('SELECT id, seller_id, status, pre_moderation_status AS pms FROM listings WHERE id = ?', { lid })[1]
  if not l then return { ok = false, error = 'no such listing' } end
  if l.status == 'removed' then return { ok = true, listing_id = lid, status = 'removed' } end   -- already down: no-op
  -- prior status to restore later. If it was auto-hidden ('pending'), the true prior
  -- was already saved in pms (='active') — keep that, don't record 'pending'.
  local prev = (l.status == 'pending') and (l.pms ~= '' and l.pms or 'active') or l.status
  local ts = now_iso()
  cellar.exec("UPDATE listings SET pre_moderation_status=?, status='removed', updated_at=? WHERE id=?", { prev, ts, lid })
  cellar.exec("UPDATE listing_report SET status='actioned', resolved_at=?, resolved_by=? " ..
              "WHERE listing_id = ? AND status = 'open'", { ts, who.user_id, lid })
  cellar.emit('listing_takedown', { actor = who.user_id, subject = lid })
  notify(l.seller_id, 'listing_removed', 'Listing removed by a moderator',
         utf8_trunc(args.note or 'Your listing was removed for violating the marketplace rules.', 500), lid)
  return { ok = true, listing_id = lid, status = 'removed' }
end

-- reinstate_listing {listing_id} (admin): reverse a takedown / un-hide an
-- auto-hidden listing. Restores the RECORDED prior status (not a hardcoded
-- 'active', so a sold/expired item is never resurrected into public search) and
-- dismisses its open reports. Time-neutral: expiry is only refreshed if it has
-- already lapsed. No-op unless the listing is actually under a hold.
local function reinstate_listing(args, who)
  if not (who and who.role == 'admin') then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  local l = cellar.query('SELECT id, seller_id, status, pre_moderation_status AS pms FROM listings WHERE id = ?', { lid })[1]
  if not l then return { ok = false, error = 'no such listing' } end
  if l.status ~= 'removed' and l.status ~= 'pending' then
    return { ok = false, error = 'listing is not under a moderation hold' }   -- nothing to reinstate
  end
  local restore = (l.pms ~= '' and l.pms) or 'active'
  if restore == 'removed' or restore == 'pending' then restore = 'active' end   -- never restore to a hold state
  local ts = now_iso()
  cellar.exec(
    "UPDATE listings SET status=?, pre_moderation_status='', " ..
    "expires_at = CASE WHEN expires_at IS NULL OR expires_at = '' OR expires_at < ? THEN ? ELSE expires_at END, " ..
    "updated_at=? WHERE id=?", { restore, ts, iso_in(EXPIRY_DAYS), ts, lid })
  cellar.exec("UPDATE listing_report SET status='dismissed', resolved_at=?, resolved_by=? " ..
              "WHERE listing_id = ? AND status = 'open'", { ts, who.user_id, lid })
  notify(l.seller_id, 'listing_reinstated', 'Listing reinstated',
         'Your listing is active again.', lid)
  return { ok = true, listing_id = lid, status = restore }
end

-- dismiss_reports {listing_id} (admin): reviewed, reports were not actionable —
-- clear the open reports. If the listing was AUTO-HIDDEN ('pending'), dismissing
-- the reports un-hides it (restores the recorded prior status), so it is never
-- stranded invisible-and-off-the-queue.
local function dismiss_reports(args, who)
  if not (who and who.role == 'admin') then return nil end
  local lid = args and args.listing_id
  if not lid then return nil end
  local ts = now_iso()
  cellar.exec("UPDATE listing_report SET status='dismissed', resolved_at=?, resolved_by=? " ..
              "WHERE listing_id = ? AND status = 'open'", { ts, who.user_id, lid })
  cellar.exec("UPDATE listings SET status = CASE WHEN pre_moderation_status <> '' THEN pre_moderation_status ELSE 'active' END, " ..
              "pre_moderation_status='', updated_at=? WHERE id=? AND status='pending'", { ts, lid })
  return { ok = true, listing_id = lid }
end

-- ── Client behavioral event ingestion (recs telemetry) ──────────────────────
-- POST /rpc/track { events = [ {type, subject, props}, ... ] } — client-only signals
-- (impressions / clicks / dwell) the server can't observe. HARD invariants:
--   * server STAMPS the actor (client can NEVER set it) → no spoofing another user
--   * TYPE ALLOWLIST → client can't forge server-authoritative events (contact/
--     favorite/takedown) that trust / velocity / moderation depend on
--   * self-reported → recs/analytics ONLY, never a security signal
-- Fire-and-forget: invalid events are DROPPED, never fatal. Returns accepted/dropped.
local TRACK_TYPES     = { impression = true, result_click = true, card_view = true,
                          dwell = true, search_view = true }
local TRACK_BATCH_MAX = 20     -- events accepted per call
local TRACK_SCAN_MAX  = 100    -- max array slots scanned/call — events[i] is O(i) on the cJSON list,
                               -- so an uncapped scan of a huge client array is O(n^2) (anon CPU-DoS guard)
local TRACK_PROPS_MAX = 512    -- bytes of props JSON per event
local TRACK_RATE_1MIN = 300    -- per-actor track events / minute (authed only)

-- Coerce client props to a bounded, VALID JSON object string. Contract: props is a
-- JSON *string* (client serializes it); validated via SQLite json_valid + size cap
-- and stored verbatim (opaque). Anything else → '{}'. No Lua JSON encoder needed.
local function track_props(p)
  if type(p) ~= 'string' or #p == 0 or #p > TRACK_PROPS_MAX then return '{}' end
  local v = cellar.query('SELECT json_valid(?) AS v', { p })[1]
  return (v and tonumber(v.v) == 1) and p or '{}'
end

-- Reserve up to `want` track slots for `actor` this minute; returns granted count.
-- Single-row PK upsert on track_rate (no event-log scan). Anon (actor='') isn't
-- counted here — bounded by the batch cap + the engine per-IP /rpc limit.
local function track_grant(actor, want)
  if want <= 0 then return 0 end
  if actor == '' then return want end
  local now = os.time(); local wstart = now - (now % 60)
  local row = cellar.query('SELECT window_start AS w, count AS c FROM track_rate WHERE actor_id = ?', { actor })[1]
  local used  = (row and tonumber(row.w) == wstart) and tonumber(row.c) or 0
  local grant = TRACK_RATE_1MIN - used
  if grant < 0 then grant = 0 end
  if grant > want then grant = want end
  if grant > 0 then
    if row then
      cellar.exec('UPDATE track_rate SET window_start = ?, count = ? WHERE actor_id = ?', { wstart, used + grant, actor })
    else
      cellar.exec('INSERT INTO track_rate(actor_id, window_start, count) VALUES (?,?,?)', { actor, wstart, grant })
    end
  end
  return grant
end

local function track(args, who)
  local actor  = (who and who.user_id) or ''
  local events = args and args.events
  if type(events) ~= 'table' then return { ok = true, accepted = 0, dropped = 0 } end
  -- Fire-and-forget: a DB hiccup (e.g. SQLITE_BUSY in track_props/track_grant) must
  -- never surface as a 400 — pcall the whole body and fall back to a no-op result.
  local ok, res = pcall(function()
    -- Iterate by index until nil: the args proxy's __len is NOT honored for tables
    -- under LuaJIT (5.1 semantics), so #events is unreliable (cf. add_facet_conds).
    -- A boxed object/array element IS a Lua table; a scalar element is not → dropped.
    -- Bound the scan at TRACK_SCAN_MAX (events[i] is O(i) → uncapped is O(n^2)).
    local valid, dropped, i = {}, 0, 1
    while i <= TRACK_SCAN_MAX do
      local e = events[i]
      if e == nil then break end
      i = i + 1
      if #valid >= TRACK_BATCH_MAX then dropped = dropped + 1
      elseif type(e) ~= 'table' or not TRACK_TYPES[e.type] then dropped = dropped + 1
      else
        local subj = (type(e.subject) == 'string') and e.subject or ''
        if #subj > 64 then subj = subj:sub(1, 64) end
        valid[#valid + 1] = { type = e.type, subject = subj, props = track_props(e.props) }
      end
    end
    local grant = track_grant(actor, #valid)
    for j = 1, grant do
      local v = valid[j]
      cellar.emit(v.type, { actor = actor, subject = v.subject, props = v.props })
    end
    return { ok = true, accepted = grant, dropped = dropped + (#valid - grant) }
  end)
  if ok then return res end
  return { ok = true, accepted = 0, dropped = 0 }
end

-- ── repair / ops primitive ──────────────────────────────────────────────────
-- POST /rpc/rebuild_facets {"id": "<listing-id>"}  (admin) — reconcile one
-- listing's facets if an after() fault ever left them stale.
--
-- NOTES (engine authz model):
--   * /api list+get now DEFER to policy (fixed in cellar) — anon may read a table
--     that lists "anon"; search/listing rpcs remain for ranked/visibility queries.
--   * /rpc IS authorized by the engine now: the _rpc whitelist in policies.json is
--     enforced fail-closed (unlisted/unauthorized → 401/403 before this runs; a
--     superuser still needs the fn listed). The self-guards below (e.g. rebuild_facets
--     checking who.role == 'admin') are now defense-in-depth, not the primary gate.
function rpc(name, args, who)
  if name == 'search' then
    return search(args, who)
  end
  if name == 'track' then
    return track(args, who)
  end
  if name == 'feed' then
    return feed(args, who)
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
      -- exclude dead rows: a recurring job that exhausted max_attempts keeps
      -- repeat_every>0 but is never claimed again — re-seeding must revive it,
      -- not dedup against the corpse.
      local ex = cellar.query("SELECT id FROM job WHERE type = ? AND repeat_every > 0 AND state <> 'dead' LIMIT 1", { t })[1]
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
  if name == 'my_limits' then
    return my_limits(args, who)
  end
  if name == 'set_profile' then           -- set the caller's public display name
    if not (who and who.authenticated) then return nil, 'auth required' end
    local nm = args and args.name
    if type(nm) ~= 'string' or nm == '' then return { error = 'name required' } end
    nm = utf8_trunc(nm, 80)
    local ts = now_iso()
    cellar.exec('INSERT INTO user_profile(id, display_name, created_at, updated_at) VALUES (?,?,?,?) ' ..
      'ON CONFLICT(id) DO UPDATE SET display_name = excluded.display_name, updated_at = excluded.updated_at',
      { who.user_id, nm, ts, ts })
    return { ok = true, name = nm }
  end
  if name == 'my_listings' then           -- the caller's own listings + engagement stats
    if not (who and who.authenticated) then return nil, 'auth required' end
    local rows = cellar.query(
      'SELECT l.id, l.title, l.price, l.currency, l.category_id, l.city_id, l.status, l.photos, ' ..
      'l.created_at, l.expires_at, ' ..
      '(SELECT count(*) FROM event e WHERE e.type = ? AND e.subject_id = l.id) AS views_count, ' ..
      '(SELECT count(*) FROM contact_event ce WHERE ce.listing_id = l.id) AS contacts_count ' ..
      'FROM listings l WHERE l.seller_id = ? ORDER BY l.created_at DESC',
      { 'listing_viewed', who.user_id })
    return { results = rows }
  end
  if name == 'rebuild_facets' then
    if who.role ~= 'admin' then return nil end
    local id = args and args.id
    if not id then return nil end
    sync_facets(id)
    local n = cellar.query('SELECT count(*) AS n FROM listing_facet WHERE listing_id = ?', { id })
    return { listing_id = id, facets = n[1].n }
  end
  -- Trust & safety (report → queue → takedown)
  if name == 'report_listing'    then return report_listing(args, who)     end
  if name == 'list_reports'      then return list_reports(args, who)       end
  if name == 'listing_reports'   then return listing_reports_rpc(args, who) end
  if name == 'takedown_listing'  then return takedown_listing(args, who)   end
  if name == 'reinstate_listing' then return reinstate_listing(args, who)  end
  if name == 'dismiss_reports'   then return dismiss_reports(args, who)    end
  return nil
end
