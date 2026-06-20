// cellar.js — a tiny dependency-free browser client for a cellar app.
//
// Two layers, both optional to use:
//   - REST:     login/register + CRUD (/auth, /api/<table>, /rpc/<name>)
//   - Realtime: subscribe to a table over the WebSocket and get live CHANGE events
//
// It talks to the same origin it was served from, so the Host header routes the
// request to this app automatically. Drop it in any cellar bundle's public/.

export class Cellar {
  constructor(base = "") {
    this.base = base;                                  // "" = same origin
    this.token = localStorage.getItem("cel_token") || null;
  }

  // ---- auth ---------------------------------------------------------------
  async login(email, password) {
    const b = await this._json("POST", "/auth/login", { email, password });
    this.token = b.token;
    localStorage.setItem("cel_token", this.token);
    return b.user;
  }
  async register(email, password, role = "member") {
    return this._json("POST", "/auth/register", { email, password, role });
  }
  logout() { this.token = null; localStorage.removeItem("cel_token"); }
  get authed() { return !!this.token; }

  // ---- REST CRUD ----------------------------------------------------------
  // list("tasks", { order: "-priority", limit: 100, status: "todo" }) -> {rows,count}
  list(table, params = {}) {
    const q = new URLSearchParams(params).toString();
    return this._json("GET", `/api/${table}${q ? "?" + q : ""}`);
  }
  get(table, id)         { return this._json("GET",    `/api/${table}/${id}`); }
  create(table, row)     { return this._json("POST",   `/api/${table}`, row); }
  update(table, id, row) { return this._json("PATCH",  `/api/${table}/${id}`, row); }
  remove(table, id)      { return this._json("DELETE", `/api/${table}/${id}`); }

  // ---- custom endpoints (hooks.lua `rpc`) ---------------------------------
  rpc(name, args = {}) { return this._json("POST", `/rpc/${name}`, args); }

  // ---- realtime -----------------------------------------------------------
  // Subscribe to a table; `onChange({table, op, row})` fires on every delivered
  // INSERT/UPDATE/DELETE. The server's on_realtime hook decides what reaches us.
  // Returns a handle with .close(). Auto-reconnects with backoff.
  subscribe(table, onChange) {
    const OP_SUBSCRIBE = 0x20, OP_CHANGE = 0x22;
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const url = `${proto}://${location.host}/`;
    let ws, closed = false, backoff = 500, mid = 1;

    const frame = (opcode, obj) => {
      const body = new TextEncoder().encode(JSON.stringify(obj));
      const buf = new ArrayBuffer(8 + body.length);
      const dv = new DataView(buf);
      dv.setUint8(0, opcode);            // opcode
      dv.setUint8(1, 0);                 // flags
      dv.setUint16(2, mid++, false);     // message_id (big-endian)
      dv.setUint32(4, body.length, false);
      new Uint8Array(buf, 8).set(body);
      return buf;
    };
    const parse = (buf) => {
      const dv = new DataView(buf);
      const len = dv.getUint32(4, false);
      const json = len ? new TextDecoder().decode(new Uint8Array(buf, 8, len)) : "{}";
      return { opcode: dv.getUint8(0), body: JSON.parse(json) };
    };

    const connect = () => {
      ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        backoff = 500;
        ws.send(frame(OP_SUBSCRIBE, { token: this.token, table }));  // REST token reused
      };
      ws.onmessage = (ev) => {
        const { opcode, body } = parse(ev.data);
        if (opcode === OP_CHANGE) onChange(body);
      };
      ws.onclose = () => {
        if (closed) return;
        setTimeout(connect, backoff);
        backoff = Math.min(backoff * 2, 8000);
      };
      ws.onerror = () => ws.close();
    };
    connect();
    return { close() { closed = true; ws && ws.close(); } };
  }

  // ---- internals ----------------------------------------------------------
  async _json(method, path, body) {
    const headers = {};
    if (body !== undefined) headers["Content-Type"] = "application/json";
    if (this.token) headers["Authorization"] = "Bearer " + this.token;
    const r = await fetch(this.base + path, {
      method, headers, body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const text = await r.text();
    const data = text ? JSON.parse(text) : {};
    if (!r.ok) throw Object.assign(new Error(data.message || r.statusText), { status: r.status, data });
    return data;
  }
}
