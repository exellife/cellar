// views/reports.js — revenue + outstanding balances, computed client-side from the
// local mirror (offline-first: the whole org dataset is synced, so totals are exact).
//   contracted = Σ items − discount   (confirmed + completed bookings)
//   collected  = Σ payments
//   outstanding = Σ max(0, contracted − collected)
//   pipeline   = contracted of tentative bookings
// Cancelled bookings are excluded everywhere.
import { db } from "/db.js";
import { som } from "/util.js";

export const ReportsView = {
  data() { return { period: "all" }; },
  computed: {
    months() {
      const set = new Set(db.state.bookings.filter((b) => b.status !== "cancelled").map((b) => String(b.start_date).slice(0, 7)));
      return [...set].sort().reverse();
    },
    rows() {
      const inPeriod = (b) => this.period === "all" || String(b.start_date).slice(0, 7) === this.period;
      return db.state.bookings.filter((b) => b.status !== "cancelled" && inPeriod(b)).map((b) => {
        const items = db.state.booking_items.filter((r) => r.booking_id === b.id).reduce((s, r) => s + (+r.quantity || 0) * (+r.unit_price || 0), 0);
        const collected = db.state.payments.filter((p) => p.booking_id === b.id).reduce((s, p) => s + (+p.amount_kgs || 0), 0);
        const contracted = items - (+b.discount || 0);
        return { status: b.status, venueId: this.venueOf(b.hall_id), contracted, collected, balance: contracted - collected };
      });
    },
    committed() { return this.rows.filter((r) => r.status === "confirmed" || r.status === "completed"); },
    summary() {
      const sum = (arr, k) => arr.reduce((s, x) => s + x[k], 0);
      const tentative = this.rows.filter((r) => r.status === "tentative");
      return {
        count: this.rows.length,
        confirmed: this.rows.filter((r) => r.status === "confirmed").length,
        completed: this.rows.filter((r) => r.status === "completed").length,
        tentative: tentative.length,
        contracted: sum(this.committed, "contracted"),
        collected: sum(this.committed, "collected"),
        outstanding: this.committed.reduce((s, r) => s + Math.max(0, r.balance), 0),
        pipeline: sum(tentative, "contracted"),
      };
    },
    byVenue() {
      const m = {};
      for (const r of this.committed) {
        const v = (m[r.venueId] ??= { contracted: 0, collected: 0, outstanding: 0, count: 0 });
        v.contracted += r.contracted; v.collected += r.collected; v.outstanding += Math.max(0, r.balance); v.count++;
      }
      return Object.entries(m).map(([vid, v]) => ({ name: this.venueName(vid), ...v })).sort((a, b) => b.contracted - a.contracted);
    },
  },
  methods: {
    som,
    venueOf(hallId) { return (db.state.halls.find((h) => h.id === hallId) || {}).venue_id; },
    venueName(vid) { return (db.state.venues.find((v) => v.id === vid) || {}).name || "—"; },
    monthLabel(m) { const [y, mo] = m.split("-"); return new Date(+y, +mo - 1, 1).toLocaleDateString(undefined, { month: "long", year: "numeric" }); },
  },
  template: `
    <div>
      <div class="page-head">
        <h1>Reports</h1>
        <select class="venue-select" v-model="period">
          <option value="all">All time</option>
          <option v-for="m in months" :key="m" :value="m">{{ monthLabel(m) }}</option>
        </select>
      </div>
      <div class="stats">
        <div class="stat"><div class="label">Contracted</div><div class="value">{{ som(summary.contracted) }}</div><div class="sub">{{ summary.confirmed }} confirmed · {{ summary.completed }} completed</div></div>
        <div class="stat accent"><div class="label">Collected</div><div class="value">{{ som(summary.collected) }}</div><div class="sub">payments received</div></div>
        <div class="stat warn"><div class="label">Outstanding</div><div class="value">{{ som(summary.outstanding) }}</div><div class="sub">owed to you</div></div>
        <div class="stat"><div class="label">Pipeline</div><div class="value">{{ som(summary.pipeline) }}</div><div class="sub">{{ summary.tentative }} tentative</div></div>
      </div>
      <h3>By venue</h3>
      <p class="empty" v-if="!byVenue.length">No confirmed or completed bookings in this period.</p>
      <table class="grid" v-else>
        <thead><tr><th>Venue</th><th class="r">Bookings</th><th class="r">Contracted</th><th class="r">Collected</th><th class="r">Outstanding</th></tr></thead>
        <tbody>
          <tr v-for="v in byVenue" :key="v.name">
            <td>{{ v.name }}</td><td class="r">{{ v.count }}</td>
            <td class="r">{{ som(v.contracted) }}</td><td class="r">{{ som(v.collected) }}</td><td class="r">{{ som(v.outstanding) }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
};
