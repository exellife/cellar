// views/bookings.js — Bookings: venue-scoped calendar (+ list view), the dedicated
// BookingForm modal, and offline conflict detection (one live booking per
// hall + session + date).
import { db } from "/db.js";
import { byOrder, opt, labelOf, SESSIONS, EVENT_TYPES } from "/util.js";
import { Calendar } from "/components/calendar.js";
import { BookingForm } from "/components/booking-form.js";

export const BookingsView = {
  components: { Calendar, BookingForm },
  template: `
    <div>
      <div class="page-head">
        <div class="head-left">
          <h1>Bookings</h1>
          <select class="venue-select" v-model="venue" v-if="venueOptions.length">
            <option v-for="v in venueOptions" :value="v.id">{{ v.name }}</option>
          </select>
        </div>
        <div class="seg">
          <button :class="{ active: view==='calendar' }" @click="view='calendar'">Calendar</button>
          <button :class="{ active: view==='list' }" @click="view='list'">List</button>
        </div>
      </div>
      <p class="muted" v-if="!venueOptions.length">Add a venue with at least one hall first.</p>

      <calendar v-else-if="view==='calendar'" :events="events" @day-click="onDay" @event-click="onEvent"></calendar>

      <template v-else>
        <p class="empty" v-if="!bookings.length">No bookings yet — click a day on the calendar to add one.</p>
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
      </template>

      <booking-form v-if="form" :booking="form.booking" :date="form.date" :halls="formHalls"
                    @submit="onSubmit" @cancel="form=null"></booking-form>
    </div>
  `,
  data() { return { view: "calendar", venue: "", form: null }; },
  watch: {
    // a single calendar is per-venue; default to the first venue once they load
    // (and re-pick the first if the selected venue is deleted). Venues arrive async.
    venueOptions: {
      immediate: true,
      handler(opts) { if (opts.length && !opts.some(v => v.id === this.venue)) this.venue = opts[0].id; },
    },
  },
  computed: {
    venueOptions() { return [...db.state.venues].sort(byOrder); },
    bookings() {
      const rows = this.venue
        ? db.state.bookings.filter(b => this.hallVenueId(b.hall_id) === this.venue)
        : [...db.state.bookings];
      return rows.sort((a, b) =>
        String(a.start_date).localeCompare(b.start_date) || String(a.session).localeCompare(b.session));
    },
    hallOptions() {
      const vname = (id) => (db.state.venues.find(v => v.id === id) || {}).name || "?";
      return [...db.state.halls].sort(byOrder).map(h => opt(h.id, `${vname(h.venue_id)} · ${h.name}`));
    },
    events() {
      return this.bookings.map(b => ({
        id: b.id, start: b.start_date, end: b.end_date || b.start_date, color: b.status,
        title: `${this.sessionShort(b.session)} · ${this.hallShort(b.hall_id)} · ${b.customer_name}`,
        data: b,
      }));
    },
    formHalls() {
      if (!this.form) return [];
      const vid = (this.form.booking && this.hallVenueId(this.form.booking.hall_id)) || this.venue;
      return this.hallOptsFor(vid);
    },
  },
  methods: {
    onDay(date) { if (this.hallOptsFor(this.venue).length) this.form = { booking: null, date }; },
    onEvent(ev) { this.editBooking(ev.data); },
    editBooking(b) { this.form = { booking: b, date: b.start_date }; },
    delBooking(b) { if (confirm(`Delete booking for ${b.customer_name}?`)) db.remove("bookings", b.id); },
    onSubmit(v) {
      const ex = this.form.booking;
      const end = ex?.end_date || v.start_date;
      const c = this.conflicts(v.hall_id, v.session, v.start_date, end, ex?.id);
      if (c.length && !confirm(
        `"${this.hallLabel(v.hall_id)}" is already booked for ${this.sessionLabel(v.session)} on ${v.start_date} (${c[0].customer_name}). Book anyway?`))
        return;
      db.save("bookings", {
        id: ex?.id, hall_id: v.hall_id, session: v.session, event_type: v.event_type,
        start_date: v.start_date, end_date: end, start_time: v.start_time,
        customer_name: v.customer_name, customer_phone: v.customer_phone,
        guest_count_min: v.guest_count_min, guest_count_max: v.guest_count_max,
        discount: ex?.discount || 0, status: ex?.status || "tentative", notes: ex?.notes || null,
      });
      this.form = null;
    },

    conflicts(hallId, session, startDate, endDate, excludeId) {
      return db.state.bookings.filter(b =>
        b.id !== excludeId && b.hall_id === hallId && b.session === session &&
        b.status !== "cancelled" && b.status !== "completed" &&
        String(b.end_date) >= startDate && String(b.start_date) <= endDate);
    },
    hallOptsFor(venueId) { return db.state.halls.filter(h => h.venue_id === venueId).sort(byOrder).map(h => opt(h.id, h.name)); },
    hallVenueId(id) { return (db.state.halls.find(h => h.id === id) || {}).venue_id; },
    hallShort(id) { return (db.state.halls.find(h => h.id === id) || {}).name || "?"; },
    sessionShort(s) { return ({ morning: "M", afternoon: "A", evening: "E" })[s] || "?"; },
    hallLabel(id) { return labelOf(this.hallOptions, id); },
    sessionLabel(v) { return labelOf(SESSIONS, v); },
    eventLabel(v) { return labelOf(EVENT_TYPES, v); },
    guests(b) { return b.guest_count_max > b.guest_count_min ? `${b.guest_count_min}–${b.guest_count_max}` : `${b.guest_count_max || b.guest_count_min || 0}`; },
  },
};
