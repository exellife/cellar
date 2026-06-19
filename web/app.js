/* pgforge admin — petite-vue (no build). Drives the /schema + /api endpoints. */
PetiteVue.createApp({
  // ---- auth ----
  token: localStorage.getItem('pgf_token') || '',
  user: JSON.parse(localStorage.getItem('pgf_user') || 'null'),
  loginEmail: '', loginPassword: '', loginError: '',

  // ---- data ----
  tables: [],
  current: null,
  rows: [],
  loading: false,
  error: '',

  // ---- form ----
  editing: null,   // the row being edited, or {} for new
  isNew: false,
  form: {},
  formError: '',

  get authed() { return !!this.token; },
  get pk() { return this.current && this.current.primary_key; },
  get editableColumns() {
    return this.current ? this.current.columns.filter(c => !c.primary_key) : [];
  },

  async api(method, path, body) {
    const headers = { 'Content-Type': 'application/json' };
    if (this.token) headers['Authorization'] = 'Bearer ' + this.token;
    const res = await fetch(path, { method, headers, body: body ? JSON.stringify(body) : undefined });
    let data = {};
    try { data = await res.json(); } catch (e) { /* empty body */ }
    if (res.status === 401) { this.logout(); throw new Error(data.message || 'unauthorized'); }
    if (!res.ok) throw new Error(data.message || ('HTTP ' + res.status));
    return data;
  },

  async init() { if (this.token) await this.loadSchema(); },

  async login() {
    this.loginError = '';
    try {
      const d = await this.api('POST', '/auth/login', { email: this.loginEmail, password: this.loginPassword });
      this.token = d.token; this.user = d.user;
      localStorage.setItem('pgf_token', this.token);
      localStorage.setItem('pgf_user', JSON.stringify(this.user));
      this.loginPassword = '';
      await this.loadSchema();
    } catch (e) { this.loginError = e.message; }
  },

  logout() {
    this.token = ''; this.user = null;
    localStorage.removeItem('pgf_token'); localStorage.removeItem('pgf_user');
    this.tables = []; this.current = null; this.rows = [];
  },

  async loadSchema() {
    try {
      const d = await this.api('GET', '/schema');
      this.tables = d.tables || [];
      if (this.tables.length) await this.select(this.tables[0]);
    } catch (e) { this.error = e.message; }
  },

  async select(t) { this.current = t; this.editing = null; this.error = ''; await this.loadRows(); },

  async loadRows() {
    if (!this.current) return;
    this.loading = true; this.error = '';
    try {
      const d = await this.api('GET', '/api/' + this.current.name + '?limit=200');
      this.rows = d.rows || [];
    } catch (e) { this.error = e.message; this.rows = []; }
    finally { this.loading = false; }
  },

  cell(row, col) {
    const v = row[col.name];
    if (v === null || v === undefined) return '';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
  },

  startCreate() {
    this.isNew = true; this.formError = ''; this.form = {};
    for (const c of this.editableColumns) this.form[c.name] = (c.type === 'bool') ? false : '';
    this.editing = {};
  },
  startEdit(row) {
    this.isNew = false; this.formError = ''; this.form = {};
    for (const c of this.editableColumns) {
      const v = row[c.name];
      this.form[c.name] = (v === null || v === undefined) ? '' :
        (typeof v === 'object' ? JSON.stringify(v) : v);
    }
    this.editing = row;
  },
  cancel() { this.editing = null; },

  coerce(c, v) {
    if (c.type === 'int' || c.type === 'bigint') return parseInt(v, 10);
    if (c.type === 'float' || c.type === 'numeric') return parseFloat(v);
    if (c.type === 'bool') return (v === true || v === 'true');
    if (c.type === 'json') { try { return JSON.parse(v); } catch (e) { return v; } }
    return v;
  },

  async save() {
    this.formError = '';
    const vals = {};
    for (const c of this.editableColumns) {
      let v = this.form[c.name];
      if (v === '' || v === null || v === undefined) {
        if (c.nullable) vals[c.name] = null;   // explicit null for nullable
        continue;                              // else omit (let DB default / required apply)
      }
      vals[c.name] = this.coerce(c, v);
    }
    try {
      if (this.isNew) await this.api('POST', '/api/' + this.current.name, vals);
      else await this.api('PATCH', '/api/' + this.current.name + '/' + this.editing[this.pk], vals);
      this.editing = null;
      await this.loadRows();
    } catch (e) { this.formError = e.message; }
  },

  async del(row) {
    if (!confirm('Delete this row?')) return;
    try {
      await this.api('DELETE', '/api/' + this.current.name + '/' + row[this.pk]);
      await this.loadRows();
    } catch (e) { this.error = e.message; }
  },
}).mount('#app');
