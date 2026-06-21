// components/booking-form.js — the dedicated New/Edit booking modal with a
// multi-column layout. The date is set by the clicked calendar day (shown in the
// header). Emits submit(values) / cancel; the parent does conflict detection + save.
import { SESSIONS, EVENT_TYPES } from "/util.js";

export const BookingForm = {
  props: {
    booking: { type: Object, default: null },     // null = new
    date: { type: String, default: "" },          // preset date for a new booking
    halls: { type: Array, default: () => [] },     // [{value,label}] scoped to the venue
  },
  emits: ["submit", "cancel"],
  data() {
    const b = this.booking || {};
    return {
      SESSIONS, EVENT_TYPES,
      m: {
        customer_name: b.customer_name || "",
        customer_phone: b.customer_phone || "",
        event_type: b.event_type || "wedding",
        guest_count_min: b.guest_count_min ?? "",
        guest_count_max: b.guest_count_max ?? "",
        hall_id: b.hall_id || (this.halls[0]?.value || ""),
        session: b.session || "evening",
        start_time: b.start_time || "",
        price_per_person: b.price_per_person ?? "",
        deposit: b.deposit ?? "",
      },
    };
  },
  computed: {
    title() { return this.booking ? "Edit booking" : "New booking"; },
    dateLabel() {
      const s = this.booking ? this.booking.start_date : this.date;
      if (!s) return "";
      const [y, mo, d] = s.split("-").map(Number);
      return new Date(y, mo - 1, d).toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric", year: "numeric" });
    },
  },
  methods: {
    submit() {
      const m = this.m;
      if (!m.customer_name.trim() || !m.hall_id || !m.event_type || !m.session) return;
      this.$emit("submit", {
        customer_name: m.customer_name.trim(),
        customer_phone: m.customer_phone.trim() || null,
        event_type: m.event_type,
        guest_count_min: +m.guest_count_min || 0,
        guest_count_max: +m.guest_count_max || 0,
        hall_id: m.hall_id,
        session: m.session,
        start_time: m.start_time || null,
        price_per_person: +m.price_per_person || 0,
        deposit: +m.deposit || 0,
        start_date: this.booking ? this.booking.start_date : this.date,
      });
    },
  },
  template: `
    <div class="modal-backdrop" @click="$emit('cancel')">
      <div class="modal modal-wide" @click.stop>
        <h3>{{ title }} <span class="muted small">· {{ dateLabel }}</span></h3>
        <div class="form-grid">
          <label class="field col-3"><span>Name *</span><input v-model="m.customer_name" placeholder="Customer" @keyup.enter="submit"></label>
          <label class="field col-3"><span>Phone</span><input v-model="m.customer_phone" placeholder="Phone" @keyup.enter="submit"></label>

          <label class="field col-2"><span>Event type *</span>
            <select v-model="m.event_type"><option v-for="o in EVENT_TYPES" :value="o.value">{{ o.label }}</option></select></label>
          <label class="field col-2"><span>Guests (min)</span><input type="number" v-model="m.guest_count_min"></label>
          <label class="field col-2"><span>Guests (max)</span><input type="number" v-model="m.guest_count_max"></label>

          <label class="field col-2"><span>Hall *</span>
            <select v-model="m.hall_id"><option v-for="o in halls" :value="o.value">{{ o.label }}</option></select></label>
          <label class="field col-2"><span>Session *</span>
            <select v-model="m.session"><option v-for="o in SESSIONS" :value="o.value">{{ o.label }}</option></select></label>
          <label class="field col-2"><span>Time</span><input type="time" v-model="m.start_time"></label>

          <label class="field col-3"><span>Price / guest (som)</span><input type="number" v-model="m.price_per_person"></label>
          <label class="field col-3"><span>Deposit (som)</span><input type="number" v-model="m.deposit"></label>
        </div>
        <div class="modal-actions">
          <button class="ghost" @click="$emit('cancel')">Cancel</button>
          <button class="primary" @click="submit">Save</button>
        </div>
      </div>
    </div>
  `,
};
