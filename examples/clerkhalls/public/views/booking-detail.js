// views/booking-detail.js — one booking: header + the three itemized sections
// (Menu categorized, Wholesale + Extras flat) + grand-total card + payments.
// Every line/category/payment change persists through db (offline-first).
import { db } from "/db.js";
import { byOrder, opt, labelOf, som, prettyDate, SESSIONS, EVENT_TYPES } from "/util.js";
import { ItemizedTable } from "/components/itemized-table.js";
import { GrandTotalCard } from "/components/grand-total-card.js";
import { PaymentSection } from "/components/payment-section.js";
import { BookingForm } from "/components/booking-form.js";

const sumRows = (rows) => rows.reduce((s, r) => s + (+r.quantity || 0) * (+r.unit_price || 0), 0);

export const BookingDetailView = {
  components: { ItemizedTable, GrandTotalCard, PaymentSection, BookingForm },
  props: { id: { type: String, required: true } },
  data() { return { form: null }; },
  computed: {
    booking() { return db.state.bookings.find((b) => b.id === this.id) || null; },
    itemsOf() { return (kind) => db.state.booking_items.filter((r) => r.booking_id === this.id && r.kind === kind); },
    catsOf() { return (kind) => db.state.booking_item_categories.filter((c) => c.booking_id === this.id && c.kind === kind); },
    menuRows() { return this.itemsOf("menu"); },
    menuCats() { return this.catsOf("menu"); },
    wholesaleRows() { return this.itemsOf("wholesale"); },
    extrasRows() { return this.itemsOf("extra"); },
    payments() { return db.state.payments.filter((p) => p.booking_id === this.id); },
    menuSubtotal() { return sumRows(this.menuRows); },
    wholesaleSubtotal() { return sumRows(this.wholesaleRows); },
    extrasSubtotal() { return sumRows(this.extrasRows); },
    totalPaid() { return this.payments.reduce((s, p) => s + (+p.amount_kgs || 0), 0); },
    bookingVenueId() { const h = db.state.halls.find((x) => x.id === this.booking?.hall_id); return h?.venue_id; },
    formHalls() { return db.state.halls.filter((h) => h.venue_id === this.bookingVenueId).sort(byOrder).map((h) => opt(h.id, h.name)); },
  },
  methods: {
    som, prettyDate,
    uuid() { return crypto.randomUUID(); },
    back() { this.$router.push("/bookings"); },
    eventLabel(v) { return labelOf(EVENT_TYPES, v); },
    sessionLabel(v) { return labelOf(SESSIONS, v); },
    hallLabel(hallId) {
      const h = db.state.halls.find((x) => x.id === hallId); if (!h) return "?";
      const v = db.state.venues.find((x) => x.id === h.venue_id);
      return v ? `${v.name} · ${h.name}` : h.name;
    },

    // ---- itemized-table persistence (one db.save per change) ----
    addItem(kind, r) {
      db.save("booking_items", { id: r.id, booking_id: this.id, kind, category_id: r.category_id || null,
        name: r.name, quantity: String(r.quantity), unit_price: r.unit_price, sort_order: this.itemsOf(kind).length });
    },
    editItem(r) {
      db.save("booking_items", { id: r.id, booking_id: r.booking_id, kind: r.kind, category_id: r.category_id || null,
        name: r.name, quantity: String(r.quantity), unit_price: r.unit_price });
    },
    removeItem(itemId) { db.remove("booking_items", itemId); },
    addCat(kind, name) { db.save("booking_item_categories", { booking_id: this.id, kind, name, sort_order: this.catsOf(kind).length }); },
    renameCat({ id, name }) { db.save("booking_item_categories", { id, name }); },
    removeCat(catId) { db.remove("booking_item_categories", catId); },

    // ---- bill + payments ----
    setDiscount(v) { db.save("bookings", { id: this.id, discount: v }); },
    addPayment(p) { db.save("payments", { booking_id: this.id, ...p }); },
    removePayment(pid) { db.remove("payments", pid); },

    // ---- header edit / delete ----
    edit() { this.form = { booking: this.booking, date: this.booking.start_date }; },
    delBooking() { if (confirm(`Delete this booking for ${this.booking.customer_name}?`)) { db.remove("bookings", this.id); this.back(); } },
    onEditSubmit(v) {
      const b = this.booking;
      db.save("bookings", {
        id: b.id, hall_id: v.hall_id, session: v.session, event_type: v.event_type,
        start_date: v.start_date, end_date: b.end_date || v.start_date, start_time: v.start_time,
        customer_name: v.customer_name, customer_phone: v.customer_phone,
        guest_count_min: v.guest_count_min, guest_count_max: v.guest_count_max,
      });
      this.form = null;
    },
  },
  template: `
    <div class="bdetail">
      <button class="link back" @click="back">← Bookings</button>
      <p class="empty" v-if="!booking">Booking not found.</p>
      <template v-else>
        <div class="page-head">
          <div class="head-left">
            <h1>{{ booking.customer_name }}</h1>
            <span class="pill" :class="'st-'+booking.status">{{ booking.status }}</span>
          </div>
          <div class="row-actions">
            <button class="ghost" @click="edit">Edit</button>
            <button class="ghost danger" @click="delBooking">Delete</button>
          </div>
        </div>
        <div class="factrow muted">
          <span>{{ eventLabel(booking.event_type) }}</span>
          <span>{{ prettyDate(booking.start_date) }}<template v-if="booking.end_date && booking.end_date!==booking.start_date"> → {{ prettyDate(booking.end_date) }}</template></span>
          <span>{{ sessionLabel(booking.session) }}<template v-if="booking.start_time"> · {{ booking.start_time }}</template></span>
          <span>{{ hallLabel(booking.hall_id) }}</span>
          <span v-if="booking.customer_phone">{{ booking.customer_phone }}</span>
        </div>

        <div class="sec"><h3>Menu</h3>
          <itemized-table :rows="menuRows" :categories="menuCats" :money="som" :new-id="uuid"
            @row-add="r=>addItem('menu',r)" @row-edit="editItem" @row-remove="removeItem"
            @category-add="n=>addCat('menu',n)" @category-rename="renameCat" @category-remove="removeCat"></itemized-table>
        </div>
        <div class="sec"><h3>Wholesale</h3>
          <itemized-table :rows="wholesaleRows" :categorized="false" :money="som" :new-id="uuid"
            @row-add="r=>addItem('wholesale',r)" @row-edit="editItem" @row-remove="removeItem"></itemized-table>
        </div>
        <div class="sec"><h3>Extras</h3>
          <itemized-table :rows="extrasRows" :categorized="false" :money="som" :new-id="uuid"
            @row-add="r=>addItem('extra',r)" @row-edit="editItem" @row-remove="removeItem"></itemized-table>
        </div>

        <grand-total-card :menu="menuSubtotal" :wholesale="wholesaleSubtotal" :extras="extrasSubtotal"
          :discount="+booking.discount||0" @update:discount="setDiscount"
          :total-paid="totalPaid" :payments="payments"></grand-total-card>

        <payment-section :payments="payments" :new-id="uuid" @add="addPayment" @remove="removePayment"></payment-section>

        <booking-form v-if="form" :booking="form.booking" :date="form.date" :halls="formHalls"
          @submit="onEditSubmit" @cancel="form=null"></booking-form>
      </template>
    </div>
  `,
};
