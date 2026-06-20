// app.js — ClerkHalls, on petite-vue. The template lives in index.html (#app); this
// is the reactive scope: auth, hash routing, and per-screen data/methods. Rows come
// from db.state (the sync engine's reactive projection); writes go through db.*.
import { db } from "/db.js";

const route = () => (location.hash.replace(/^#\/?/, "") || "venues").split("/")[0];
const byOrder = (a, b) =>
  (a.sort_order ?? 0) - (b.sort_order ?? 0) || String(a.name || "").localeCompare(b.name || "");
const field = (key, label, required, val, type, options) =>
  ({ key, label, required: !!required, val: val ?? "", type: type || "text", options });

const opt = (value, label) => ({ value, label });
const SESSIONS = [opt("morning", "Morning"), opt("afternoon", "Afternoon"), opt("evening", "Evening")];
const STATUSES = [opt("tentative", "Tentative"), opt("confirmed", "Confirmed"), opt("completed", "Completed"), opt("cancelled", "Cancelled")];
const EVENT_TYPES = [
  "wedding:Wedding", "bride_farewell:Bride farewell", "anniversary:Anniversary", "birthday:Birthday",
  "beshik_toi:Beshik toi", "zhentek_toi:Zhentek toi", "tushoo_toi:Tushoo toi", "quran_reading:Quran reading",
  "memorial:Memorial", "graduation:Graduation", "corporate:Corporate", "conference:Conference", "other:Other",
].map(s => { const [v, l] = s.split(":"); return opt(v, l); });
const labelOf = (list, v) => (list.find(o => o.value === v) || {}).label || v || "";

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
  async start() {
    // a stale/expired token (e.g. server restarted) -> drop to the login screen.
    db.onAuthLost = () => { this.authed = false; this.authMsg = "Session expired — please sign in again."; };
    db.startRealtime();          // instant updates while the WS is connected
    await db.sync();             // initial catch-up
    // safety net: poll so devices converge even if a realtime event is missed / the
    // WS drops, and sync the moment you switch back to the tab.
    setInterval(() => db.sync(), 12000);
    window.addEventListener("focus", () => db.sync());
    document.addEventListener("visibilitychange", () => { if (!document.hidden) db.sync(); });
  },

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

  // ---- venues & halls ----
  venues() { return [...this.s.venues].sort(byOrder); },
  halls(vid) { return this.s.halls.filter(h => h.venue_id === vid).sort(byOrder); },

  newVenue() {
    this.open("New venue", [field("name", "Name", true), field("address", "Address"), field("phone", "Phone")],
      v => db.save("venues", { ...v, sort_order: this.s.venues.length }));
  },
  editVenue(x) {
    this.open("Edit venue",
      [field("name", "Name", true, x.name), field("address", "Address", false, x.address), field("phone", "Phone", false, x.phone)],
      v => db.save("venues", { id: x.id, ...v }));
  },
  delVenue(x) {
    const n = this.halls(x.id).length;
    if (confirm(`Delete venue "${x.name}"${n ? ` and its ${n} hall(s)` : ""}?`)) db.remove("venues", x.id);
  },
  newHall(vid) {
    this.open("New hall", [field("name", "Name", true), field("capacity", "Capacity", false, "", "number")],
      v => db.save("halls", { venue_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null, sort_order: this.halls(vid).length }));
  },
  editHall(vid, x) {
    this.open("Edit hall",
      [field("name", "Name", true, x.name), field("capacity", "Capacity", false, x.capacity, "number")],
      v => db.save("halls", { id: x.id, venue_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null }));
  },
  delHall(x) { if (confirm(`Delete hall "${x.name}"?`)) db.remove("halls", x.id); },

  // ---- bookings ----
  SESSIONS, STATUSES, EVENT_TYPES,
  bookings() {
    return [...this.s.bookings].sort((a, b) =>
      String(a.start_date).localeCompare(b.start_date) || String(a.session).localeCompare(b.session));
  },
  hallOptions() {
    const vname = (id) => (this.s.venues.find(v => v.id === id) || {}).name || "?";
    return [...this.s.halls].sort(byOrder).map(h => opt(h.id, `${vname(h.venue_id)} · ${h.name}`));
  },
  hallLabel(id) { return labelOf(this.hallOptions(), id); },
  sessionLabel(v) { return labelOf(SESSIONS, v); },
  eventLabel(v) { return labelOf(EVENT_TYPES, v); },
  guests(b) { return b.guest_count_max > b.guest_count_min ? `${b.guest_count_min}–${b.guest_count_max}` : `${b.guest_count_max || b.guest_count_min || 0}`; },

  conflicts(hallId, session, startDate, endDate, excludeId) {
    return this.s.bookings.filter(b =>
      b.id !== excludeId && b.hall_id === hallId && b.session === session &&
      b.status !== "cancelled" && b.status !== "completed" &&
      String(b.end_date) >= startDate && String(b.start_date) <= endDate);
  },

  bookingFields(b) {
    return [
      field("hall_id", "Hall", true, b?.hall_id, "select", this.hallOptions()),
      field("event_type", "Event type", true, b?.event_type || "wedding", "select", EVENT_TYPES),
      field("session", "Session", true, b?.session || "evening", "select", SESSIONS),
      field("start_date", "Date", true, b?.start_date, "date"),
      field("end_date", "End date (multi-day)", false, b?.end_date, "date"),
      field("customer_name", "Customer", true, b?.customer_name),
      field("customer_phone", "Phone", false, b?.customer_phone),
      field("guest_count_min", "Guests (min)", false, b?.guest_count_min, "number"),
      field("guest_count_max", "Guests (max)", false, b?.guest_count_max, "number"),
      field("price_per_person", "Price / guest (som)", false, b?.price_per_person, "number"),
      field("status", "Status", false, b?.status || "tentative", "select", STATUSES),
      field("notes", "Notes", false, b?.notes, "textarea"),
    ];
  },
  newBooking() { this.open("New booking", this.bookingFields(), this._saveBooking(null)); },
  editBooking(b) { this.open("Edit booking", this.bookingFields(b), this._saveBooking(b)); },
  delBooking(b) { if (confirm(`Delete booking for ${b.customer_name}?`)) db.remove("bookings", b.id); },
  _saveBooking(existing) {
    return (v) => {
      const end = v.end_date || v.start_date;
      const c = this.conflicts(v.hall_id, v.session, v.start_date, end, existing?.id);
      if (c.length && !confirm(
        `"${this.hallLabel(v.hall_id)}" is already booked for ${this.sessionLabel(v.session)} on ${v.start_date} (${c[0].customer_name}). Book anyway?`))
        return false;
      db.save("bookings", {
        id: existing?.id, hall_id: v.hall_id, session: v.session, event_type: v.event_type,
        start_date: v.start_date, end_date: end,
        customer_name: v.customer_name, customer_phone: v.customer_phone || null,
        guest_count_min: +v.guest_count_min || 0, guest_count_max: +v.guest_count_max || 0,
        price_per_person: +v.price_per_person || 0, status: v.status || "tentative",
        notes: v.notes || null,
      });
    };
  },

  // ---- generic modal editor ----
  open(title, fields, save) {
    this.modal = { title, fields, save, model: Object.fromEntries(fields.map(f => [f.key, f.val])) };
  },
  saveModal() {
    const m = this.modal;
    if (m.fields.some(f => f.required && !String(m.model[f.key] ?? "").trim())) return;
    if (m.save({ ...m.model }) === false) return;   // save aborted (e.g. conflict declined) — keep open
    this.modal = null;
  },
}).mount("#app");
