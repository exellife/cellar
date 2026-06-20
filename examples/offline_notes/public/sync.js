// sync.js — an offline-first sync client for a cellar syncable table.
//
// The whole offline loop in one class. State lives in sessionStorage, so EACH TAB is
// an independent "device" with its own local mirror, pending queue, and device_id —
// open two tabs to watch them converge. The protocol it speaks (cellar-sync-design.md):
//   - local optimistic writes queue as mutations (op, id, base_rev, values, mutation_id);
//   - sync() pushes the queue (idempotent via mutation_id, device_id for GC), then pulls
//     everything since its cursor and applies it (server is authoritative);
//   - while online it also applies realtime CHANGE events live.
const LS = sessionStorage;                 // per-tab = per-device
const uuid = () => crypto.randomUUID();
const now = () => Math.floor(Date.now() / 1000);

export class Sync {
  constructor(table) {
    this.table = table;
    this.device = LS.getItem("dev") || (LS.setItem("dev", uuid()), LS.getItem("dev"));
    this.token = LS.getItem("tok") || null;
    this.rows = new Map(JSON.parse(LS.getItem("rows") || "[]"));   // id -> row (incl tombstones)
    this.cursor = +(LS.getItem("cursor") || 0);                    // last pulled rev
    this.queue = JSON.parse(LS.getItem("queue") || "[]");          // pending mutations
    this.online = true;
    this.onchange = () => {};   // re-render
    this.onlog = () => {};      // sync-log line
  }

  persist() {
    LS.setItem("rows", JSON.stringify([...this.rows]));
    LS.setItem("cursor", String(this.cursor));
    LS.setItem("queue", JSON.stringify(this.queue));
  }
  log(m) { this.onlog(m); }
  get pending() { return this.queue.length; }
  liveRows() { return [...this.rows.values()].filter(r => !r.deleted); }

  // ---- auth ----
  async login(email, password) {
    const b = await this._post("/auth/login", { email, password });
    this.token = b.token; LS.setItem("tok", this.token);
    return b.user;
  }
  get authed() { return !!this.token; }

  // ---- local, optimistic writes (queued for push) ----
  put(id, values) {
    const prev = this.rows.get(id);
    const row = { ...(prev || { id, rev: 0, deleted: 0 }), ...values, id, updated_at: now() };
    this.rows.set(id, row);
    // coalesce: at most one pending put per id; base_rev is the last SYNCED rev.
    this.queue = this.queue.filter(m => !(m.op === "put" && m.id === id));
    this.queue.push({ mutation_id: uuid(), op: "put", table: this.table, id,
                      base_rev: prev ? prev.rev : undefined,
                      values: { title: row.title, body: row.body, updated_at: row.updated_at } });
    this._after();
  }
  del(id) {
    const r = this.rows.get(id); if (!r) return;
    this.queue = this.queue.filter(m => m.id !== id);   // a delete supersedes pending puts
    if (r.rev === 0) { this.rows.delete(id); }          // never synced → just forget it locally
    else { r.deleted = 1; this.rows.set(id, r);
           this.queue.push({ mutation_id: uuid(), op: "del", table: this.table, id, base_rev: r.rev }); }
    this._after();
  }
  _after() { this.persist(); this.onchange(); if (this.online) this.sync(); }

  // ---- the sync loop ----
  async sync() {
    if (!this.online || !this.authed || this._syncing) return;
    this._syncing = true;
    try {
      if (this.queue.length) {
        const res = await this._post("/sync/push", { mutations: this.queue, device_id: this.device });
        for (const r of res.results || []) {
          const local = this.rows.get(r.id);
          if (local && typeof r.rev === "number" && r.rev > 0) local.rev = r.rev;  // adopt server rev
          this.log(`push ${short(r.id)} → ${r.status}${r.winner ? "/" + r.winner : ""}` +
                   `${r.deduped ? " (dup)" : ""}`);
        }
        this.queue = [];                                // whole batch is acknowledged
        this.persist();
      }
      const pull = await this._post("/sync/pull", { since: this.cursor, device_id: this.device });
      const changes = (pull.changes || {})[this.table] || [];
      for (const row of changes) this.rows.set(row.id, { ...row });   // server is authoritative
      if (changes.length) this.log(`pull ← ${changes.length} change(s) (cursor ${this.cursor}→${pull.cursor})`);
      this.cursor = pull.cursor;
      this.persist(); this.onchange();
    } catch (e) {
      this.log("⚠ sync error: " + (e.message || e));
    } finally { this._syncing = false; }
  }

  setOnline(v) { this.online = v; this.log(v ? "— online —" : "— offline —"); if (v) this.sync(); }

  // ---- realtime: apply CHANGE events live while online ----
  startRealtime() {
    const OP_SUB = 0x20, OP_CHANGE = 0x22;
    const frame = (op, obj) => {
      const b = new TextEncoder().encode(JSON.stringify(obj));
      const buf = new ArrayBuffer(8 + b.length); const dv = new DataView(buf);
      dv.setUint8(0, op); dv.setUint32(4, b.length, false); new Uint8Array(buf, 8).set(b);
      return buf;
    };
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const connect = () => {
      const ws = new WebSocket(`${proto}://${location.host}/`); ws.binaryType = "arraybuffer";
      ws.onopen = () => ws.send(frame(OP_SUB, { token: this.token, table: this.table }));
      ws.onmessage = (ev) => {
        const dv = new DataView(ev.data);
        if (dv.getUint8(0) !== OP_CHANGE) return;
        if (!this.online) return;                       // "offline": ignore live events
        const len = dv.getUint32(4, false);
        const body = JSON.parse(new TextDecoder().decode(new Uint8Array(ev.data, 8, len)));
        if (!body.row || this.rows.get(body.row.id)?.rev >= body.row.rev) return;  // already have it
        this.rows.set(body.row.id, { ...body.row });
        if (body.row.rev > this.cursor) this.cursor = body.row.rev;
        this.log(`live ${body.op} ${short(body.row.id)}`); this.persist(); this.onchange();
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

const short = (id) => (id || "").slice(0, 6);
