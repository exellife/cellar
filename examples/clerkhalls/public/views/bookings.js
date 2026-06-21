// views/bookings.js — Bookings list + create/edit form with offline conflict
// detection (one live booking per hall + session + date).
import { db } from "/db.js";
import { openModal } from "/store.js";
import { field, byOrder, opt, labelOf, SESSIONS, STATUSES, EVENT_TYPES } from "/util.js";

export const BookingsView = {
  template: `
    <div>
      <div class="page-head"><h1>Bookings</h1><button class="primary" @click="newBooking" :disabled="!hallOptions.length">+ Booking</button></div>
      <p class="muted" v-if="!hallOptions.length">Add a venue with at least one hall first.</p>
      <p class="empty" v-else-if="!bookings.length">No bookings yet — add one.</p>
      <table class="grid" v-else>
        <thead><tr><th>Date</th><th>Session</th><th>Hall</th><th>Event</th><th>Customer</th><th>Guests</th><th>Status</th><th></th></tr></thead>
        <tbody>
          <tr v-for="b in bookings" :key="b.id" class="brow" @click="editBooking(b)">
            <td class="nowrap">{{ b.start_date }}<template v-if="b.end_date && b.end_date !== b.start_date"> → {{ b.end_date }}</template></td>
            <td>{{ sessionLabel(b.session) }}</td>
            <td>{{ hallLabel(b.hall_id) }}</td>
            <td>{{ eventLabel(b.event_type) }}</td>
            <td>{{ b.customer_name }}<div class="muted small" v-if="b.customer_phone">{{ b.customer_phone }}</div></td>
            <td>{{ guests(b) }}</td>
            <td><span class="pill" :class="'st-'+b.status">{{ b.status }}</span></td>
            <td class="nowrap" @click.stop><button class="mini danger" @click="delBooking(b)">✕</button></td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  computed: {
    bookings() {
      return [...db.state.bookings].sort((a, b) =>
        String(a.start_date).localeCompare(b.start_date) || String(a.session).localeCompare(b.session));
    },
    hallOptions() {
      const vname = (id) => (db.state.venues.find(v => v.id === id) || {}).name || "?";
      return [...db.state.halls].sort(byOrder).map(h => opt(h.id, `${vname(h.venue_id)} · ${h.name}`));
    },
  },
  methods: {
    hallLabel(id) { return labelOf(this.hallOptions, id); },
    sessionLabel(v) { return labelOf(SESSIONS, v); },
    eventLabel(v) { return labelOf(EVENT_TYPES, v); },
    guests(b) { return b.guest_count_max > b.guest_count_min ? `${b.guest_count_min}–${b.guest_count_max}` : `${b.guest_count_max || b.guest_count_min || 0}`; },

    conflicts(hallId, session, startDate, endDate, excludeId) {
      return db.state.bookings.filter(b =>
        b.id !== excludeId && b.hall_id === hallId && b.session === session &&
        b.status !== "cancelled" && b.status !== "completed" &&
        String(b.end_date) >= startDate && String(b.start_date) <= endDate);
    },

    bookingFields(b) {
      return [
        field("hall_id", "Hall", true, b?.hall_id, "select", this.hallOptions),
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
    newBooking() { openModal("New booking", this.bookingFields(), this._saveBooking(null)); },
    editBooking(b) { openModal("Edit booking", this.bookingFields(b), this._saveBooking(b)); },
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
  },
};
