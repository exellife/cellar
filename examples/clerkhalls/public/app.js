// app.js — ClerkHalls, on petite-vue. The template lives in index.html (#app); this
// is the reactive scope: auth, hash routing, and per-screen data/methods. Rows come
// from db.state (the sync engine's reactive projection); writes go through db.*.
import { db } from "/db.js";

const route = () => (location.hash.replace(/^#\/?/, "") || "venues").split("/")[0];
const byOrder = (a, b) =>
  (a.sort_order ?? 0) - (b.sort_order ?? 0) || String(a.name || "").localeCompare(b.name || "");
const field = (key, label, required, val, type) =>
  ({ key, label, required: !!required, val: val ?? "", type: type || "text" });

PetiteVue.createApp({
  db,
  s: db.state,                                  // reactive store: s.<table> = live rows
  route: route(),
  authed: db.authed,
  email: localStorage.getItem("ch_email") || "owner@clerkhalls.local",
  password: "clerkhalls",
  authMsg: "",
  modal: null,                                  // { title, fields, save, model }

  async init() {
    window.addEventListener("hashchange", () => { this.route = route(); });
    if (this.authed) await this.start();
  },
  async start() { db.startRealtime(); await db.sync(); },

  // ---- auth ----
  async signin(register) {
    try {
      if (register) await db.register(this.email.trim(), this.password);
      await db.login(this.email.trim(), this.password);
      this.authed = true;
      await this.start();
    } catch (e) {
      this.authMsg = e.status === 401 ? "Wrong email or password." : (e.message || "Sign-in failed.");
    }
  },
  signout() { db.logout(); location.reload(); },
  go(r) { location.hash = "#/" + r; },

  // ---- venues & rooms ----
  venues() { return [...this.s.event_halls].sort(byOrder); },
  rooms(vid) { return this.s.halls.filter(r => r.event_hall_id === vid).sort(byOrder); },

  newVenue() {
    this.open("New venue", [field("name", "Name", true), field("address", "Address"), field("phone", "Phone")],
      v => db.save("event_halls", { ...v, sort_order: this.s.event_halls.length }));
  },
  editVenue(x) {
    this.open("Edit venue",
      [field("name", "Name", true, x.name), field("address", "Address", false, x.address), field("phone", "Phone", false, x.phone)],
      v => db.save("event_halls", { id: x.id, ...v }));
  },
  delVenue(x) {
    const n = this.rooms(x.id).length;
    if (confirm(`Delete venue "${x.name}"${n ? ` and its ${n} room(s)` : ""}?`)) db.remove("event_halls", x.id);
  },
  newRoom(vid) {
    this.open("New room", [field("name", "Name", true), field("capacity", "Capacity", false, "", "number")],
      v => db.save("halls", { event_hall_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null, sort_order: this.rooms(vid).length }));
  },
  editRoom(vid, x) {
    this.open("Edit room",
      [field("name", "Name", true, x.name), field("capacity", "Capacity", false, x.capacity, "number")],
      v => db.save("halls", { id: x.id, event_hall_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null }));
  },
  delRoom(x) { if (confirm(`Delete room "${x.name}"?`)) db.remove("halls", x.id); },

  // ---- generic modal editor ----
  open(title, fields, save) {
    this.modal = { title, fields, save, model: Object.fromEntries(fields.map(f => [f.key, f.val])) };
  },
  saveModal() {
    const m = this.modal;
    if (m.fields.some(f => f.required && !String(m.model[f.key] ?? "").trim())) return;
    m.save({ ...m.model });
    this.modal = null;
  },
}).mount("#app");
