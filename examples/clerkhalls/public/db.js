// db.js — ClerkHalls offline-first store + sync engine (multi-table).
//
// A local mirror of every syncable table, a pending-mutation queue, and the
// push→pull sync loop against cellar's /sync API. State is in localStorage so it
// persists; device_id is per-browser, so two browsers = two devices. The views
// read/write through this and never touch the network directly.

const SYNCABLE = [
  "organization", "venues", "halls", "bookings",
  "booking_item_categories", "booking_items", "payments",
  "menu_categories", "menu_items",
];

// Bump when the schema / table names change so stale local state (rows + queued
// mutations referencing renamed/removed tables) is cleared instead of poisoning sync.
const DATA_VERSION = "2";   // 2: event_halls -> venues

const LS = localStorage;
const uuid = () => crypto.randomUUID();
const nowIso = () => new Date().toISOString().replace(/\.\d+Z$/, "Z");
const j = (k, d) => { try { return JSON.parse(LS.getItem(k)) ?? d; } catch { return d; } };

class DB {
  constructor() {
    // schema/data migration: on a model change, drop the local mirror + queue +
    // cursor (a full re-pull rebuilds them) so stale-table mutations can't wedge sync.
    if (LS.getItem("ch_ver") !== DATA_VERSION) {
      for (const k of ["ch_rows", "ch_queue", "ch_cursor"]) LS.removeItem(k);
      LS.setItem("ch_ver", DATA_VERSION);
    }
    this.device = LS.getItem("ch_dev") || (LS.setItem("ch_dev", uuid()), LS.getItem("ch_dev"));
    this.token  = LS.getItem("ch_tok") || null;
    this.cursor = +(LS.getItem("ch_cursor") || 0);
    this.queue  = j("ch_queue", []);
    this.online = LS.getItem("ch_offline") !== "1";
    this.tables = {};                       // table -> Map(id -> row)
    const saved = j("ch_rows", {});
    for (const t of SYNCABLE) this.tables[t] = new Map(saved[t] || []);
    this._subs = new Set();
    this._syncing = false;
    // a reactive projection the petite-vue views bind to: state[table] = live rows.
    // Rebuilt on every change (app-scale data → cheap); keeps sync internals (Maps)
    // separate from reactivity.
    this.state = window.Vue.reactive({ online: this.online, pending: 0, log: "" });
    for (const t of SYNCABLE) this.state[t] = [];
    this._refresh();
  }

  _refresh() {
    for (const t of SYNCABLE) this.state[t] = [...this.tables[t].values()].filter(r => !r.deleted);
    this.state.online = this.online;
    this.state.pending = this.queue.length;
    this.state.log = this._log || (this.online ? "online" : "offline");
  }

  // ---- persistence + events ----
  _persist() {
    const rows = {};
    for (const t of SYNCABLE) rows[t] = [...this.tables[t]];
    LS.setItem("ch_rows", JSON.stringify(rows));
    LS.setItem("ch_cursor", String(this.cursor));
    LS.setItem("ch_queue", JSON.stringify(this.queue));
  }
  on(cb) { this._subs.add(cb); return () => this._subs.delete(cb); }
  _emit() { this._refresh(); for (const cb of this._subs) cb(); }   // reactive views update via _refresh
  log(m) { this._log = m; this._emit(); }     // last status line (views show it)
  get pending() { return this.queue.length; }

  // ---- auth ----
  async login(email, password) {
    const b = await this._post("/auth/login", { email, password });
    this.token = b.token; LS.setItem("ch_tok", this.token); LS.setItem("ch_email", email);
    return b.user;
  }
  async register(email, password) { return this._post("/auth/register", { email, password, role: "staff" }); }
  get authed() { return !!this.token; }
  logout() { this.token = null; LS.removeItem("ch_tok"); }

  // ---- reads (live rows only; tombstones hidden) ----
  all(table) { return [...(this.tables[table]?.values() || [])].filter(r => !r.deleted); }
  where(table, pred) { return this.all(table).filter(pred); }
  byId(table, id) { const r = this.tables[table]?.get(id); return r && !r.deleted ? r : null; }
  count(table, pred) { return (pred ? this.where(table, pred) : this.all(table)).length; }

  // ---- writes (optimistic; queued for push) ----
  save(table, obj) {                          // upsert: create if no id / unknown id
    const ex = obj.id ? this.tables[table].get(obj.id) : null;
    const id = obj.id || uuid();
    const row = { ...(ex || { id, rev: 0, deleted: 0 }), ...obj, id, updated_at: nowIso() };
    if (!ex) row.created_at = row.created_at || nowIso();
    this.tables[table].set(id, row);
    const values = { ...obj }; delete values.id; delete values.rev; delete values.deleted;
    values.updated_at = row.updated_at; if (!ex) values.created_at = row.created_at;
    this.queue = this.queue.filter(m => !(m.op === "put" && m.table === table && m.id === id));
    this.queue.push({ mutation_id: uuid(), op: "put", table, id,
                      base_rev: ex ? ex.rev : undefined, values });
    this._after(); return row;
  }
  remove(table, id) {
    const r = this.tables[table].get(id); if (!r) return;
    this.queue = this.queue.filter(m => !(m.table === table && m.id === id));
    if (r.rev === 0) { this.tables[table].delete(id); }   // never synced -> just forget
    else { r.deleted = 1; this.tables[table].set(id, r);
           this.queue.push({ mutation_id: uuid(), op: "del", table, id, base_rev: r.rev }); }
    this._after();
  }
  _after() { this._persist(); this._emit(); if (this.online) this.sync(); }

  // ---- the sync loop ----
  setOnline(v) { this.online = v; LS.setItem("ch_offline", v ? "0" : "1"); this.log(v ? "online" : "offline"); if (v) this.sync(); }
  async sync() {
    if (!this.online || !this.authed || this._syncing) return;
    this._syncing = true;
    try {
      if (this.queue.length) {
        const res = await this._post("/sync/push", { mutations: this.queue, device_id: this.device });
        for (const r of res.results || []) {
          const m = this.tables; const row = (() => { for (const t of SYNCABLE) if (m[t].has(r.id)) return m[t].get(r.id); })();
          if (row && typeof r.rev === "number" && r.rev > 0) row.rev = r.rev;
        }
        this.log(`pushed ${res.results?.length || 0}`); this.queue = []; this._persist();
      }
      const pull = await this._post("/sync/pull", { since: this.cursor, device_id: this.device });
      let n = 0;
      for (const [t, rows] of Object.entries(pull.changes || {})) {
        if (!this.tables[t]) continue;
        for (const row of rows) { this.tables[t].set(row.id, { ...row }); n++; }
      }
      if (n) this.log(`pulled ${n}`);
      this.cursor = pull.cursor; this._persist(); this._emit();
    } catch (e) {
      if (e.status === 401) { this.logout(); this.onAuthLost && this.onAuthLost(); this.log("session expired"); }
      else this.log("sync error: " + (e.message || e));
    }
    finally { this._syncing = false; }
  }

  // ---- realtime: apply CHANGE events live while online ----
  startRealtime() {
    const OP_SUB = 0x20, OP_CHANGE = 0x22;
    const frame = (op, mid, o) => {
      const b = new TextEncoder().encode(JSON.stringify(o));
      const buf = new ArrayBuffer(8 + b.length), dv = new DataView(buf);
      dv.setUint8(0, op); dv.setUint16(2, mid, false); dv.setUint32(4, b.length, false);
      new Uint8Array(buf, 8).set(b); return buf;
    };
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const connect = () => {
      const ws = new WebSocket(`${proto}://${location.host}/`); ws.binaryType = "arraybuffer";
      ws.onopen = () => SYNCABLE.forEach((t, i) => ws.send(frame(OP_SUB, i + 1, { token: this.token, table: t })));
      ws.onmessage = (ev) => {
        const dv = new DataView(ev.data);
        if (dv.getUint8(0) !== OP_CHANGE || !this.online) return;
        const len = dv.getUint32(4, false);
        const b = JSON.parse(new TextDecoder().decode(new Uint8Array(ev.data, 8, len)));
        if (!b.row || !this.tables[b.table]) return;
        const have = this.tables[b.table].get(b.row.id);
        if (have && have.rev >= b.row.rev) return;            // already have it
        this.tables[b.table].set(b.row.id, { ...b.row });
        if (b.row.rev > this.cursor) this.cursor = b.row.rev;
        this._persist(); this._emit();
      };
      ws.onclose = () => setTimeout(connect, 1500);
      ws.onerror = () => ws.close();
    };
    connect();
  }

  async _post(path, body) {
    const h = { "Content-Type": "application/json" };
    if (this.token) h["Authorization"] = "Bearer " + this.token;
    const r = await fetch(path, { method: "POST", headers: h, body: JSON.stringify(body) });
    const t = await r.text(); const d = t ? JSON.parse(t) : {};
    if (!r.ok) throw Object.assign(new Error(d.message || r.statusText), { status: r.status });
    return d;
  }
}

export const db = new DB();
export { SYNCABLE };
